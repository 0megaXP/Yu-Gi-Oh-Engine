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
deep search ended up anyway.

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
  hundred), and `ygoe-cli nonengine` ranks a deck's non-engine slots by them.

## The nightly pipeline (`.github/workflows/ml.yml`, "Learn")

1. **The app's data**: real games the YGO Sandbox app re-played from the player's YGO Omega
   replays (`replays-*.jsonl.gz` in the `ml-data` release, see below), its combo pieces from
   replays and videos (`app-pieces.json`) and its move policy (`app-policy.json`).
2. **Combo pieces**: for each deck in `decks/`, `ygoe-cli library` searches 40 opening hands
   deeply and splits the best lines into pieces, adding the app's pieces the deck can play. The
   bots use them as the app's bots use a deck's reference boards.
3. **Self-play**: 8 runners × 4 processes play best-of-3 matches for 4.5 hours: game 1 with a
   coin flip for who goes first, then the loser goes first, games 2 and 3 with both decks sided
   for the matchup (`side.h`). Normal bots at 200 search iterations (the last 2 runners at 800),
   with the current network, the move policies and the combo pieces; in each player's first 2
   Main / Battle Phase decisions a bot plays another well-explored move a quarter of the time.
   A quarter of the games start instead from a real game's position (from the replays: both
   decks, hands, field, GY, banished cards and LP at a turn's Main Phase 1) and are played on.
   Games are cut at turn 12. Each Main Phase decision is written from both sides, and each
   search with something to learn as a policy example.
4. **Labels**: a finished game's result is 1 / 0. A game cut at turn 12 is labelled with the
   current evaluator's win chance there (cards in hand, board and GY count, not only LP).
   Training targets are TD(λ), λ = 0.95: a position's target blends the result with the
   network's own estimates at the later positions of the game, so an early position that was
   fine is not marked "lost" because of a later misplay.
5. **Train**: `train.py` learns the network from tonight's positions plus the last 7 nights'
   and the real games (each game once, counted 3 times), continuing from the current network.
   `ygoe-cli policy-train` learns the policy from tonight's searches plus the last 2 nights'.
   15% of the games are held out for both.
6. **Check**: each must predict the held-out games better than now (validation log-loss lower
   by 0.002: the network against the plain evaluator's, the policy against the current one's
   and the rules'). What passes is the candidate; the rest stays as it is.
7. **Arena**: 320 bot games, the candidate network and policy against the current ones, on
   8 runners. Each deal is played twice with the same decks and shuffles, the bots swapping
   seats, which cancels most of the luck of the draw (`tools/ml/arena.py`). The candidate is
   kept only if its average pair score is at least 0.52 and more than one standard error above
   0.5; the commit message carries the reports.

Real games come from the app: it follows each finished YGO Omega replay in the real rules core
(the engine's `follow_replay`, `follow.h`) and uploads what that writes: training positions (the
board after the actions of each Main Phase, from both sides, with who won, marked
`"src":"replay"`) and start positions (`"kind":"start"`). Positions are numbers only; start
positions hold the two decks and the board as card ids, no names, players or accounts. In a
replay the opponent's hand is hidden, so their hand features count unknown cards, and a start
position samples the cards nobody saw from their deck.

Run it by hand from the Actions tab ("Learn", Run workflow), with fewer hours or shards for a
quick try. Reports (`learn-report` artifact): validation log-losses, arena games, the candidate.

The app uses a new network or policy once its engine is updated to that commit: the app
downloads the latest engine (publish.yml) by itself.

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
