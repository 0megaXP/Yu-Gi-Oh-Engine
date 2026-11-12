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
