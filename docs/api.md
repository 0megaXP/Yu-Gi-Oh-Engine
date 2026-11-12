# App API

The Android module (`android/ygoe`) exposes the engine as the Kotlin class `com.ygoengine.YgoEngine`. Underneath, every call is one JSON request in and one JSON document out (`engine/src/api.h`). The same calls are available on the desktop as `ygoe-cli json <analyze|profile|side> request.json`.

## Kotlin

```kotlin
// Once, e.g. in the ViewModel. The first call copies the ~40 MB asset pack into app storage.
val engine = YgoEngine.open(context)

val request = AnalysisRequest(
    first = SideState(deck = myDeck, hand = listOf(80565021, 86457955, 14558127, 94145021, 17782288)),
    second = SideState(deck = theirDeck, hand = null),  // unknown: dealt from their list
    me = 0,
    knowledge = Knowledge.REAL,
    settings = SearchSettings(iterations = 2000),
)
val result = engine.analyze(request) { progress -> ui.progress = progress.fraction }

result.lines.forEach { line ->
    line.moves            // your steps; opponent responses start with "Opp: "
    line.endBoard         // EngineCard(code, name, where, note); code = official passcode
    line.interrupts       // what the board can stop, valued against this opponent
    line.opponentReply    // their strongest turn found; your responses start with "You: "
    line.combined         // ranking score 0..1
}

val profile = engine.profile(theirDeck)
val side = engine.side(mine = myDeck, theirs = theirDeck, simulateTurns = 4)
side.goingFirst.sideIn / sideOut, side.goingSecond.sideIn / sideOut  // SideCard(card, count, value, why)
```

Cancelling the calling coroutine stops the native search. `engine.cancel()` also stops it, but the call still returns what was found so far, with `cancelled = true`. One job runs at a time per engine. Call `engine.close()` when done.

All card ids are **official passcodes** (YGOPRODeck ids). The engine maps cards that EDOPro still ships under pre-release ids (`data/passcode_map.tsv`).

Rough cost on a phone: a few hundred iterations per second per big core. `iterations = 2000` with checking enabled takes roughly 10–30 s.

### Combo tree

```kotlin
val tree = engine.comboTree(deck, hand, opponent = theirDeck /* or null */) { p -> ui.progress = p.fraction }
tree.mainMoves; tree.mainBoard
tree.branches.forEach { b -> b.handTrap; b.usedOn; b.continuation; b.board; b.score }
```

1. **Main line:** the best line for the hand going first, with no interaction.
2. **Hand traps tested:** the non-engine cards in the opponent's list that interrupt from the hand, with Main Deck copies counting double Side Deck copies. With no opponent deck, the most played hand traps are used (Ash, Veiler, Imperm, Ogre, Droll, Nibiru, Fuwalos, Ghost Belle).
3. **Finding where each hurts:** the main line is replayed with that hand trap in the opponent's hand, to find every place it can be used (`choke_points`). Up to 4 of those places, spread over the line, are tried. Each one is searched again with the interruption forced.
4. **The branch:** the place where the interruption hurts most is kept, together with your best continuation from there. If the opponent decides first (for example the target), they pick the choice that is worst for you.
5. **Parts:** `tree.mainParts` is the main line split into sub-combos (see Combo pieces), and `branch.hitsPart` is the part each hand trap interrupts.

### Combo pieces

A line is split into sub-combos ("pieces"):

- **Tracking:** the line is replayed action by action, tracking where each card came from: the starting hand, or the action that put it in the hand, on the field or in the GY.
- **Cutting:** the line is cut where the fewest cards carry over to the rest, so pieces are mostly 2 to 5 actions long.
- **What each piece records:**
  - `needsHand`: the starting-hand cards it uses;
  - `needsEarlier`: what earlier pieces left for it;
  - `normalSummon`: whether it uses the Normal Summon;
  - `gives`: what later pieces use from it;
  - `leaves`: what stays at the end;
  - `stoppedBy`: the hand traps that can respond during it, each with the first step it can answer. These come from a second replay with the hand traps in the opponent's hand.
- **Starters:** a piece that needs nothing from earlier pieces is a `starter`.
- **Library:** reference end boards also return `ref.library`, the pieces merged across the sample lines by what they need: starters first, then the most common.
- **Using the library:** pass `ref.libraryJson` as `library` to `analyze` (in `AnalysisRequest`) or `comboTree`, or per player as `libraries` to `newDuel`.
  - **How the search uses it:** in 70% of iterations the search is steered along a plan built by chaining pieces. A plan starts with a piece the current cards allow, then adds pieces whose needs are met by what is left in hand plus what earlier pieces gave. Pieces from different lines get recombined this way.
  - **Following a plan:** an option matching one of the plan's next labels is strongly preferred, both in the tree and in playouts. Up to 3 labels may be skipped.
  - **After an interruption:** the plan is rebuilt from what is still available, so the search tries the pieces that can still be played.
  - **Measured effect:** on 12 unseen Angelechy hands, the best line found improved from 0.780 to 0.797 at 400 iterations (better on 8 hands, worse on 1). At 1500 iterations there was no clear difference. It helps most with small budgets (Normal bot, quick searches on the phone).
  - **Desktop:** `ygoe-cli pieces-bench deck.ydk reference.json --hands 12 --iterations 400` measures it.

### Learned move policy

The engine learns which options good lines choose, and its searches try those first. Every search option's playout weight becomes *rule weight × e^score*, where the score comes from a log-linear model.

```kotlin
var policy: String? = saved                                  // null the first time
val ref = engine.reference(deck, policy = policy)            // learns from the deck's best lines
policy = ref.policyJson                                      // keep it: one policy for all decks
val checked = engine.videoLines(list, lines, policy = policy)
policy = checked.policyJson ?: policy                        // verified video lines teach it too
engine.analyze(request.copy(policy = policy)) / comboTree(..., policy = policy) / newDuel(..., policy = policy)
```

- **Features:** the model sees the kind of decision and option, where the card is, the card's type and level, what its effects do (script categories: search, Special Summon, negate, ...), what a selection is for, whose turn it is, how far into the turn it is, and the card itself.
  - Most features aren't tied to one card, so what a deck teaches carries over to others.
  - The card features let each deck's own patterns be learned too.
- **Training:**
  - **Reference boards:** each hand's best line teaches the model which option was chosen at every one of the player's decisions.
  - **Verified video lines:** these count double.
  - **Learning is cumulative:** pass the policy you have and get back the updated one.
- **Only for the searching player:** in the Play engine, the combo tree and the bots, the policy steers only the searching player's options.
- **Measured** with `ygoe-cli policy-bench train.ydk[,more.ydk] test.ydk`, which searches new hands at a small budget with and without the policy:

| Trained on | Tested on | Budget | Average board | Hands better / worse |
|---|---|---|---|---|
| Angelechy (16 hands) | 12 new Angelechy hands | 300 | 0.800 → 0.847 | 12 / 0 |
| Angelechy (16 hands) | 8 new Angelechy hands | 1200 | 0.801 → 0.849 | 7 / 0 |
| Angelechy (16 hands) | 12 Elfnote hands (unseen deck) | 300 | 0.805 → 0.805 | 4 / 6 |
| Angelechy + Elfnote (16 each) | 12 new Elfnote hands | 300 | 0.807 → 0.894 | 12 / 0 |
| Angelechy + Elfnote (16 each) | 12 new Angelechy hands | 300 | 0.803 → 0.850 | 12 / 0 |

- **JSON:** `analyze`, `tree`, live duels, `reference` and `lines` take `"policy": {...}`. `reference` and `lines` return the updated `"policy"`; with `"learn": false` they leave it alone.

### Video lines

Combos read from videos (YGO Sandbox's Combos tab, or `tools/video_combo`) are replayed by the real rules, and the verified ones become combo pieces.

```kotlin
val res = engine.videoLines(deck, listOf(
    VideoLineInput(id = url, hand = hand, endBoard = fieldCards, steps = listOf(
        VideoStepInput("activate", 17782288, "discard Dominus Purge, Special Summon Enlisted"), ...)),
))
res.lines        // per line: verified, stepsMatched / stepsChecked, boardMatch, problem, moves, board, pieces
res.libraryJson  // the verified lines' pieces, merged; pass as `library` (alone or joined with a reference's)
```

- **How a line is replayed:** the deck goes first from the video's opening hand, with no opponent interaction.
  - **The player's actions:** at each action, the option matching the next video step is chosen: the same card, activated, Normal Summoned, Set or Special Summoned by its own procedure. Optional triggers ("Use X") count as activating X.
  - **Choices inside an action:** costs, targets and what to search are tried several ways. Cards the video names around that step are preferred.
  - **Beam:** a small beam keeps the replays that match the most steps and can play the video's next action right away.
- **Steps that are only seen:** searches, cards sent to the GY, banished or attached, and summons made by an effect count when that card turns up in the right place.
  - Up to 3 misread or merged steps may be skipped to find the next match.
  - Phases, attacks and unidentified cards aren't checked.
- **Verified:** at least 3 steps were checked, at least 75% of them were played, and the replay ends on at least 75% of the video's end-board cards.
  - A verified line is an exact engine line, so it is split into combo pieces like a reference sample.
  - A line that doesn't verify comes back with the reason in `problem`.
- **Measured:**
  - **Engine-made lines:** on 16 lines the engine itself found (8 Angelechy, 8 Elfnote), rewritten as bare video steps with no details, 12 verified.
  - **Impossible lines:** a line that can't be played from its hand doesn't verify.
  - **Detail helps:** real videos give details (costs, materials, search targets), which steer the choices inside each action.
- **Name-only steps:** the action `use` means "this card was played somehow" (what free text recognition can tell). The step matches any of the player's options with that card, or the card turning up in a new place from another card's effect.
  - **Without an end board:** steps alone decide. That takes at least 4 checked steps with 80% of them played, and the replayed board becomes the line's end board.
  - **Measured:** on the same 16 engine-made lines reduced to card names only, 7 verified (12 with actions and end board).
- **Request:** `{"deck", "lines": [{"id", "hand", "steps": [{"action", "code", "detail"}], "end_board"}], "beam", "tries", "seconds", "seed"}`. The actions are the ones the video reader writes (`activate`, `normal_summon`, `link_summon`, `search`, `send_to_gy`, ...).
- **Desktop:** `ygoe-cli json lines examples/request_lines.json`.

### Reference end boards

```kotlin
val ref = engine.reference(deck, hands = 20, iterations = 1500) { p -> ui.progress = p.fraction }
ref.samples       // best board first: hand, value, interrupts, board, moves
ref.commonCards   // cards most often on the field at the end
ref.library       // sub-combos across the samples (see Combo pieces); each sample has .pieces
ref.percentile(line.boardValue)   // where a Play engine line's board sits (0..1)
save(ref.json); ReferenceResult.fromJson(saved)
```

- **How the boards are built:** the engine searches random opening hands going first, with no opponent interaction (goldfish), and keeps the best repaired line for each hand.
- **What the value means:** values are deck-neutral, scored against an average competitive deck, so they compare boards of the same deck. Play engine lines report the same `boardValue`.
- **Desktop:** `ygoe-cli json reference request.json` runs it from the command line.

### Duels

```kotlin
val duel = engine.newDuel(first = deckA, second = deckB)    // player 0 goes first
var state = duel.state()                                     // board, hands, LP, phase, log, decision
state.decision?.let { d ->
    if (d.player == humanPlayer) duel.respond(chosenIndex)   // answer with an option index
    else duel.botMove(BotLevel.NORMAL)                       // or let the bot answer
}
duel.close()
```

- Choices that never matter to a person are answered automatically: ordering, counters, and windows with a single possible answer. Zone placement is automatic for bots; for the player passed as `human` to `newDuel`, every free zone is offered, named by column from that player's left ("Your Monster Zone, column 3"). So every `decision` the app sees is a real choice.
- Decks may carry `names` (passcode → card name). When the card a passcode resolves to has a different name, the engine switches to the card with that name, so a wrong or missing `passcode_map.tsv` row can't swap cards.
- The bot comes in three levels:
  - **Easy:** playout priorities only.
  - **Normal** and **Strong:** search the rest of the turn from the exact current state (about 350 and 1200 iterations), then follow that plan until play differs from it.
- **What the bot knows:**
  - **Turn 1:** the bot does not see the other player's hand. Its search samples up to 8 possible hands from their deck, keeping the cards they have already used, and checks that each sample replays to the same public position.
  - **Later turns:** it still searches the true position. Rebuilding a mid-duel position with a different hidden hand isn't reliable yet.
- **Chain windows:** the bot uses a card only for a clear gain over not responding, and it never follows moves its own search imagined for the other side.
- Each state carries the log lines since the previous one, `logPlayers` (who each line is about: 0, 1 or −1) and `moves`: every card that changed place, with `from`/`to` as player, location (ygopro `LOCATION_*` bits), slot and position, in order. The app uses them to colour the log and animate cards. `activations` lists each card or effect activated (`code`, `place`, and `after`: how many of `moves` happened before it).
- On the desktop, `ygoe-cli duel a.ydk b.ydk --bot0 strong --bot1 easy` plays bots against each other and prints the log.

## JSON

### analyze

```json
{
  "decks": [{"name": "...", "main": [ids], "extra": [ids], "side": [ids]}, {...}],
  "me": 0,
  "mode": "practice | real",
  "seed": 1,
  "hands": [[ids], null],
  "hand_sizes": [5, 5],
  "lp": [8000, 8000],
  "field": [[{"code": id, "zone": "mzone|szone", "seq": 0, "pos": "atk|def|set|up"}], []],
  "grave": [[ids], []],
  "banished": [[], []],
  "config": {"iterations": 2000, "seconds": 45, "threads": 0, "samples": 12, "lines": 3,
             "verify": true, "verify_iterations": 300, "horizon": 0}
}
```

- Side 0 takes turn 1.
- `null` for a hand means it is dealt from the deck, resampled for each sample.

The response contains:
- `current_score` and `current_interrupts`
- `my_profile` and `opponent_profile`
- `lines[]`, each with `first_move`, `moves`, `own_turn_score`, `after_opponent` (−1 if not checked), `combined`, `visits`, `end_board`, `interrupts`, `opponent_reply` and `board_after`
- `iterations`, `seconds` and `cancelled`
- `missing_cards`: passcodes in either deck that EDOPro has no script for yet; they do nothing in the analysis

### profile

The request is `{"deck": {...}}`. The response contains:
- the shares `monster_effects`, `hand_effects`, `gy_effects`, `spell_trap`, `searches` and `special_summons`
- `summary`
- `key_cards[]`
- `interrupts[]`, each with `card`, `copies`, `what`, `from`, `base_value` and `engine`

### side

The request is `{"mine": deck, "theirs": deck, "simulate": 0, "iterations": 400}`. The response contains:
- `opponent` (a profile)
- `board_shape`
- `going_first` and `going_second`, each with `side_in[]` and `side_out[]` lists of `{card, count, value, why}`

Errors come back as `{"error": "..."}`. The Kotlin layer throws `EngineException` for them.
