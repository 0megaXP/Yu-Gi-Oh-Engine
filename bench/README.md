# Benchmark

A fixed set of test positions, to tell whether a change made the engine better or worse.

```
build/ygoe-cli bench bench/suite.json --out new.json --baseline bench/baseline.json
```

Every number is printed next to the baseline's, with the change. `--only lines,interrupts,hands,duels`
runs part of it; `--iterations N` overrides the search budget (the suite's `budget`).

## What it measures

Values are board values: deck-neutral, the scale of Play engine lines and reference boards.

| Part | Case | Numbers |
|---|---|---|
| `lines` | a player's going-first turn from a replay: hand, steps, end board | `followed`: share of the steps the line checker could replay; `verified`; `player_value`: the replayed board; `engine_value`: the best board the engine's own search finds from that hand with no interaction; `gap` = engine − player; `reached`: share of lines where the engine got within 0.02 of the player |
| `interrupts` | a replay hand plus the hand trap its opponent used | `engine_value` with that card in the opponent's hand; `kept`: share of the uninterrupted value |
| `hands` | seeded random opening hands of a deck | `engine_value` |
| `duels` | bot against bot, decks going first in turn | `deck0_score`: wins for the first deck (unfinished games count half) |

The totals average each number over its cases. Higher is better for all of them.

## The suite

`suite.json` is made from YGO Omega replays of Angelechy going first against Elfnote (November
2026). Decks are in `decks/`, one per distinct replay deck. Steps use the line format of the
`lines` call (docs/api.md, Video lines), including `optional` and `by`.

`baseline.json` is the last accepted run. After a change that improves the totals, run again with
`--out bench/baseline.json` and commit it with the change.

Adding cases: more replay lines and interruptions are the most useful, from more decks, so that
the numbers don't only follow Angelechy.
