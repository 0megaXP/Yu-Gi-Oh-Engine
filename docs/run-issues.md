# Learn run issues

Everything that went wrong, or looked wrong, in the nightly Learn runs (`.github/workflows/ml.yml`),
newest run first. The run report (the scheduled task that reports each run) adds a section here for
every run it reports; fixes are noted on the issue they fix.

Status: **open** (not dealt with), **watch** (expected, or to check again on the next runs),
**fixed** (with the commit), **by design** (how it is meant to work).

---

## night-46093543091 — 28 Nov, 02:31–07:22 (Rome), scheduled (the 01:29 slot)

Built b3103b7. Success; kept 1d50909 (value network at half strength, non-engine timing, the format's
decks, end boards, deck libraries).

- **open — 3 stuck games** (`ci-stuck-46093543091-selfplay4`, `-selfplay7`, `-arena1`): a move
  that didn't return 120 s after being cancelled. Replayed here, the selfplay4 one (turn 1, P1
  chain) ends in 50 s, with the rules core's script budget stopping a card check twice: not a hang
  but a very slow move, because every one of the search's imagined games hits the same expensive
  check again, and CI's runners (4 games sharing the cores) are slower. Both new Toon decks were
  in the format this run (toon_diaz, tri-brigade-toon); Toon Black Luster Soldier's summon check
  is the known expensive one. Fix to make: remember a check that ran past the budget and fail it
  at once the next times, instead of spending the budget on it in every game.
- **open — one start position replayed over and over**: self-play starts 25% of its games from
  real start positions, and there is only 1 (`app/starts.jsonl`: both decks known, which YGO Omega
  replays almost never give). 4,270 of 50,610 positions (8%) come from that one position, which
  weighs it far too much in training. Fix to make: use a start position at most a few times a run,
  or drop the share when there are fewer than ~50.
- **watch — going second rarely wins for some decks**: Unchained 2 of 47 going-second lines won,
  Tri-Brigade Toon 6 of 57, Toon 4 of 61 (the others 10–44). These decks are grind decks going
  second (the bot plays them for card advantage); worth watching that the share doesn't fall
  further for the others.
- **by design — move policy pending** (0.500 ± 0.051, 1 run): a new policy, tested again next run.
- **by design — value network pending** (0.524 ± 0.033, 1 run), its half-strength copy kept
  (0.537 ± 0.022 over 3 runs).
- **watch — no new uploaded games**: 5,635 games followed, all from the follow cache (0 to
  follow, under a minute): the app sent no new games since the previous run.
- **harmless — 65 Node DEP0005 `Buffer()` warnings** from GitHub's own tools.

## night-46037060599 — 27 Nov, 17:51–22:23 (Rome), scheduled (the 13:29 slot)

Built 11fae26. Success; kept b3103b7 (non-engine timing, non-engine values, end boards).

- **open — the schedule**: this run is the 13:29 slot started 4 h 22 min late; the 19:29 slot
  came while it was running and never ran (as the 07:29 and 13:29 slots on 27 Nov). GitHub
  skips a scheduled run when one is in progress instead of queuing it, and starts slots hours
  late. Fix to consider: one slot every 6 hours leaves no room for a late start; fewer slots or
  a check that starts a skipped run when the previous one ends.
- **open — 1 stuck game** (`ci-stuck-46037060599-selfplay2`): turn 2, P0 to answer after Funny
  Dark Rabbit's chain (Toon). Same kind as the 28 Nov ones.
- **by design — move policy rejected**: 0.380 ± 0.049 after 40 pairs: clearly worse.
- **by design — value network rejected** after 4 runs (0.488 ± 0.019); its half-strength copy
  pending (0.498 ± 0.027, 2 runs).
- **watch — follow took 64 min**: the first run with the follow cache (it followed all 5,635
  games and kept them); the next run took under a minute.
- **open — one start position replayed over and over**: as on 28 Nov.
- **watch — going second rarely wins for some decks**: Unchained 2 of 38, Radiant Typhoon 11 of 53,
  Tri-Brigade Toon 15 of 68.
- **harmless — 65 Node DEP0005 `Buffer()` warnings**.
- **fixed (11fae26) — stuck Toon Black Luster Soldier games** of the 27 Nov 10:04 run: the rules
  core now stops a card script past its budget.
- **noted — 3 slow games from the app's training** (`local-slow-*`, 27 Nov 16:56–17:41): games cut at
  their 10-minute limit (2, 9 and 11 turns), sent by an app still on the engine before the watchdog.
