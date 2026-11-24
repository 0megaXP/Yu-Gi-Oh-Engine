# Learning (value network)

The engine's evaluator is hand-written: it turns a position into a value for each player and a
score `sigmoid(raw / 2.5)`, where `raw` is your value minus the opponent's. A small neural
network (`engine/src/valuenet.{h,cpp}`) learns a **correction** to `raw` from bot games, so that
the score predicts who wins. With no network the engine behaves exactly as before.

## Where the network lives

`data/value_net.json`. The engine loads it with the card data (`CardDB::load`), so every search,
bot and analysis uses it; `tools/make_pack.py` puts it in the asset pack, so the app gets it with
the engine. A bot can be given another one (`LiveDuel::set_value_net`), which is how two networks
are compared.

## What it reads

`Evaluator::features`: 48 numbers from one player's side. For each player (yours first): LP, hand
size, hand traps and interrupting cards in hand, dead cards in hand (Mulcharmy with a board),
monsters (all, face-up, total and highest ATK, Extra Deck monsters, interrupting ones), Set and
face-up Spell/Traps (interrupting Sets), Field Spell, GY size and GY effects, banished, Deck and
Extra Deck sizes, lasting locks, and the hand-written value. Then: whose turn is next, whose
turn it is, the turn number and the hand-written raw value. Cards are described by what they
do, not by name, so the network works for cards it never saw.

## The move policy

The bots' search tries the plays a move policy (`engine/src/policy.h`) rates highly first. It
has hand-written rules plus learned weights: the app's own (learned on the phone from video and
replay lines) and `data/policy.json`, learned here from self-play. Every search adds the two.
Here it learns from the bots' searches: at a decision, each option's share of the search's
visits is its target (the AlphaZero way), so the next searches spend their iterations where
deep search ended up anyway. Searches use it at half its weights (`Policy::kSelfPlayScale`): it
nudges the search rather than steering it while it learns from the bots' own searches.

## Non-engine (`engine/src/nonengine.h`)

The cards a deck plays besides its own combo: hand traps, interrupting Spells/Traps, board
breakers, floodgates. `ygoe-cli nonengine <deck.ydk> [opponents...]` lists them with their role,
what they stop and their measured value.

- **Classification**: from the script analysis; a card of the deck's archetype is engine, a
  card with no interaction at all is a combo piece.
- **Timing**: whenever a bot can use one on the opponent's play, self-play records the situation
  (the search's view of using vs holding, their plays this turn, a Deck play on the chain, a known
  choke point, interruptions left, board and hand sizes, what the card stops...) and whether it
  was used; 15% of these choices are turned around so both are seen. `nonengine.py timing` learns
  P(win) if used and if held; the bot adds the difference (at most ±5%) to its search's view.
  `data/nonengine_timing.json`, kept like the network: when it predicts better and wins the arena.
- **Values**: for each non-engine card, the win rate when it was in hand in the first 4 turns
  minus when it wasn't, per matchup (by archetype), going first and second, and against the whole
  format (`nonengine.py values`, `data/nonengine_values.json`, updated every run). Side plans
  (`side.h`) blend them into their card values as games add up (from 30 games, fully at a few
  hundred), and `ygoe-cli nonengine` ranks a deck's non-engine slots by them. The same file holds
  each matchup's win rates going first and going second (`first`), which `choose_first`
  (`nonengine.h`) uses for the loser's choice in a match (from 20 games each way, else against
  the whole format, else first).

## The pipeline (`.github/workflows/ml.yml`, "Learn"): every 6 hours

1. **The app's data**: the player's real games from their YGO Omega replays: each game's moves
   (the replay's game messages, card ids) and the decks it keeps, without the replay's header
   (no players or accounts): `inputs-*.jsonl.gz` in the `ml-data` release. The **follow** job
   follows every game in the rules core with this run's engine (`ygoe-cli followall`, 6 runners
   x 4 processes) and takes training positions along the way, so each fix to the follower counts
   for all the games; `tools/ml/replays.py merge` puts them in place of the positions older app
   versions made on the phone (`replays-*.jsonl.gz`), and writes `app/follow_report.json`: how
   many games were followed to the end and the cards most often next where it stopped. Also the
   app's combo pieces from replays and videos (`app-pieces.json`) and its move policy
   (`app-policy.json`).
1b. **The format's decks** (`tools/ml/meta.py`): YGOPRODeck's TCG tournament top cuts of the
   last 3 weeks, by archetype. Results: each list scores its placement (Winner 4, Runner-Up 3,
   Top 4 2, Top 8 1, lower 0.5) times the event's size (1 + log2(players / 32), at least 0.5).
   Usage: how many top-cut lists. The decks played are the 3 best archetypes by results, then
   the 5 most used of the others, each as its best scoring list; `decks/` is updated with them
   (and kept as it is when the site can't be read). All the window's lists go into the deck
   library. The run's `meta/report.json` (in the `engine` artifact) has both rankings.
2. **Combo pieces**: for each deck in `decks/`, `ygoe-cli library` searches 40 opening hands
   deeply and splits the best lines into pieces, adding the app's pieces the deck can play. The
   bots use them as the app's bots use a deck's reference boards. The libraries grow run after
   run through the format's shared pool (`combo-pool.json` in `ml-data`): each deck gets every
   pooled piece it can play, whichever deck found it (an Elfnote deck's Power Patron lines reach
   the Power Patron decks, a deck mixing archetypes gets the lines of each, a newer list of an
   archetype starts from its older lists' lines), merged with the run's (the same piece seen
   again counts its lines together and keeps the example from the best line), its 400 most
   played kept. The **pool** job then adds every deck's new pieces to the pool
   (`ygoe-cli pool-merge`, the 5000 most played kept).
3. **Self-play**: 8 runners × 4 processes play best-of-3 matches for 3.5 hours: game 1 with a
   coin flip for who goes first, then the loser chooses to go first or second (by the measured
   win rates of its deck in that matchup going first and second, see Values; a fifth of the
   choices turned around so both stay measured), games 2 and 3 with both decks sided
   for the matchup (`side.h`). Normal bots at 200 search iterations (the last 2 runners at 800),
   with the current network, the move policies and the combo pieces; in each player's first 2
   Main / Battle Phase decisions a bot plays another well-explored move a quarter of the time.
   A quarter of the games start instead from a real game's position (from the replays: both
   decks, hands, field, GY, banished cards and LP at a turn's Main Phase 1) and are played on.
   Games are cut at turn 12. Each Main Phase decision is written from both sides, and each
   search with something to learn as a policy example.
   The bots don't see what a player couldn't: at each decision the search plays out 8 guesses
   where the other player's hand cards it hasn't seen (drawn, never shown) are random cards of
   their Deck, and both Decks are shuffled (`LiveDuel::hidden_roots`, the rules core's
   `OCG_DuelSetHidden`, `tools/patches/ygopro-core/0003-set-hidden.patch`). Cards searched,
   returned to the hand or revealed stay known. (Before, only turn 1 was guessed.)
   Nor do they know the other player's list: each guess draws their unseen cards from a deck
   picked by how likely it is given the cards they have shown (`engine/src/guess.h`), out of
   the deck library `data/deck_library.json` (the last 3 weeks' top-cut lists and the decks of the
   app's real games, weighted by how often they were met; `tools/ml/decklib.py`, rebuilt and
   committed every run), or a "rogue" deck (the cards seen, their archetype, the staples).
4. **Labels**: a finished game's result is 1 / 0. A game cut at turn 12 is labelled with the
   current evaluator's win chance there (cards in hand, board and GY count, not only LP).
   Training targets are TD(λ), λ = 0.95: a position's target blends the result with the
   network's own estimates at the later positions of the game, so an early position that was
   fine is not marked "lost" because of a later misplay.
5. **Train**: `train.py` learns the network from this run's positions plus the last 20 runs'
   and the real games (each game once, counted 3 times), continuing from the current network.
   `ygoe-cli policy-train` learns the policy from this run's searches plus the last 5 runs'.
   15% of the games are held out for both.
6. **Check**: each must predict the held-out games better than now (validation log-loss lower
   by 0.002: the network against the plain evaluator's, the policy against the current one's
   and the rules'). What passes is the candidate; the rest stays as it is.
7. **Arena**: each new part that passed its check is tested alone: the current bots with only
   that part replaced, against the current bots (so a weak part can't hide behind a strong one).
   360 bot games on 9 runners, shared out among the parts. Each deal is played twice with the
   same decks and shuffles, the bots swapping seats, which cancels most of the luck of the draw
   (`tools/ml/arena.py`). One run's arena only tells a part worth 5-8% from luck, so a part
   neither clearly better nor clearly worse stays a candidate: the next runs test the same part
   again (not a newly trained one) and its pairs add up (`arena-pending.tar.gz` in `ml-data`),
   for up to 4 runs. Kept when its average pair score over all its pairs is at least 0.51 and
   more than 1.5 standard errors above 0.5; dropped when 1.5 standard errors below 0.5 or after 4
   runs. A part kept restarts the others' pairs (the bots they are measured against changed). The
   commit message carries the reports.

Real games come from the app: it follows each finished YGO Omega replay in the real rules core
(the engine's `follow_replay`, `follow.h`) and uploads what that writes: training positions (the
board after the actions of each Main Phase, from both sides, with who won, marked
`"src":"replay"`) and start positions (`"kind":"start"`). Positions are numbers only; start
positions hold the two decks and the board as card ids, no names, players or accounts. In a
replay the opponent's hand is hidden, so their hand features count unknown cards, and a start
position samples the cards nobody saw from their deck.

Run it by hand from the Actions tab ("Learn", Run workflow), with fewer hours or shards for a
quick try. Reports (`learn-report` artifact): validation log-losses, arena games, the candidate.

The app uses what was learned once its engine is updated to that commit: after a commit the run
starts the "Publish engine" workflow (a commit made by a workflow doesn't start it by itself), and
the app downloads the latest engine by itself.

## Decks

`decks/` holds the best decks of the current format, from recent top cuts: YCS Orlando
(November 2026) winner Angelechy (Kite), runner-up Elfnote Ars Magna (Bellafiore), Radiant
Typhoon (Dupee, Top 8), Power Patron Ars Magna (Shepard, Top 16) and Witchcrafter Fairy Tail
Magistus (Rodriguez Monzon, Top 32), plus the regional lists
of Angelechy (Luna, Mar del Plata) and Elfnote Ars Magna (Koller, Wien). Angelechy and
Elfnote Ars Magna made 21 of Orlando's 32 top decks, so they get two lists each. When the
format changes, replace them: the network learns from these games, so it learns the format
it plays.

## By hand

```
build/ygoe-cli library decks/a.ydk --out libs/a.json            # and the same for b
build/ygoe-cli selfplay decks/a.ydk decks/b.ydk --games 20 --libraries libs --bo3 1 --explore 0.25 \
  --out positions.jsonl --policy-out examples.jsonl
python3 tools/ml/train.py positions.jsonl --out value_net.json --report report.json
build/ygoe-cli policy-train examples.jsonl --out policy.json
build/ygoe-cli arena decks/a.ydk decks/b.ydk --games 10 --net value_net.json --vs none --policy policy.json --vs-policy none
```
