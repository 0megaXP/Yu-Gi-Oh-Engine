# Yu-Gi-Oh! Engine

A play-evaluation engine for Yu-Gi-Oh!. Given a game state it finds the strongest lines for a player, shows the end board each line builds, checks that board against the opponent's real deck by playing out their next turn, and suggests side-deck swaps for the matchup.

The rules are not re-implemented. Every card effect, chain and timing rule comes from **ygopro-core** (the EDOPro rules engine) running the **ProjectIgnis card scripts**, so any card EDOPro supports works here, including new archetypes as soon as they are scripted.

## What it does

**Analyse a position** (`ygoe-cli analyze`)
- Searches the player's turn with Monte Carlo tree search over real duel states.
- The opponent's responses during that turn (hand traps, quick effects) are part of the search.
- Reports the top lines: the steps, the end board, and the interruptions it holds. Each interruption is valued against the specific opponent deck.
- Then plays the opponent's next turn against each candidate board (phase B) and re-ranks the boards by how well they hold up. It shows the opponent's strongest reply it found and which of your cards answered it.

**How the bots play a turn** (the app's simulator)
- At a play of their own turn, the search above finds the best line it can, and the **turn planner** then goes through that whole line to the end of the turn. At each of the bot's choices in it (which card to activate, what to discard, Synchro Summon or not, which zone) it tries the other answers, plays the rest of the turn out, and keeps any line that ends better. Then each Main Phase play is taken out in turn: one the end of the turn doesn't need (an effect used just because it can be, a pointless Link Summon, a Normal Spell set for nothing) goes.
- Before that, at Main Phase 1, the turn is treated like a puzzle: the deck's combo pieces (short lines from its library, the ones it can play from this hand) are put together in different orders (up to 48 arrangements for the strong bot, 20 for the normal one) and each is played through the rules engine on every thread, before the planner's own time; the planner starts from the best of them when it ends better than the search's line.
- Combo pieces come from going first and from going second: the libraries (and the app's Deepen) also play the deck against real end boards of the format, and from turn 2 the pieces found there (breaking the board, then attacking or building) are preferred. The end boards (`data/end_boards.jsonl`) are what the player going first left at the start of turn 2 in the app's games, one per game the nightly run follows that far (`tools/ml/end_boards.py`); the cards nobody saw are left out, a face-down Spell/Trap nobody saw stays as a Set card.
- The deck's plan is read from its pieces: how many build an end board going first, how many break a board going second, and how many of those won. A deck whose going-second lines win spends more of the turn looking for a winning line. One whose lines mostly only break and rebuild (a grind deck: fewer than a quarter of its going-second lines won) spends less there, and from turn 2 on it plays for card advantage: each card it has in hand or on the field is worth a little more, so a line that spends cards for nothing loses to one that keeps them.
- The bot then plays that plan without searching again. It plans again only when something it didn't expect happens (a draw, the opponent's response), and what is left of the old plan is a candidate there.
- A Battle Phase with no monster that can attack is never a choice. On an open field every monster that can attack directly does, and the bot goes to battle before ending the turn when an attack gets through.
- Negates and removal on the opponent's turn answer an activated effect, on the chain link they stop. In an open window (a Summon, a phase change) they are held, except against a card with a lasting effect, a deck that can take its monster away in response (Elfnote Regina sending Lucina, so Veiler goes on Lucina before her effect), or in the End Phase.
- Battles in the search's imagined games are played the way players do (direct attacks on an open field, attacks into monsters they beat, the Battle Phase before ending the turn), so a lethal board (a Kusanagi attacking every monster) looks lethal and gets answered.
- An optional trigger that is now or never (Angelechy Endgame Problem's draw) is used whenever the search rates it higher; interruptions keep a margin for a better moment.
- The board is scored when the opponent's Main Phase 1 starts: their Draw and Standby Phase are played out first (they only draw; the bot's own Standby effects resolve: Endgame Problem's draw, Opening to e4).
- Values that depend on the board: tiered texts ("based on the number of ... ● 1+: ... ● 4+: ...") count from the board; "then if you control a Synchro Monster, you can negate" is worth half without one; an effect that targets a monster in the GY is worth half with none there.
- Traps that can be activated from the hand (Dominus, Opening to e4) are interruptions from the hand. What activating them from there locks is counted against the deck's own monsters, and setting one costs a zone (more in decks that place their own cards in the Spell & Trap Zone, like Angelechy).
- Zones: a card that wants the center Main Monster Zone (Elfnote Regina) goes there, other monsters leave it free, and a deck with Pendulum monsters keeps other cards out of its Pendulum Zones.
- `ygoe-cli simreplay <duel.json> --at N --bot strong` asks the bot what it would do at answer N of a saved simulator duel (`YGOE_BOT_DEBUG=1`, `YGOE_PLAN_DEBUG=1` for its search and plan; `YGOE_NO_PLANNER=1` to compare without the planner).

**Two knowledge modes**
- `practice`: the engine sees both hands. Use `hand1: proxy` to have it deal a random proxy hand from the opponent's list.
- `real`: the engine knows only your own hand and the public board. It samples the opponent's hidden hand from their decklist many times and plays one shared search tree across all samples (information-set MCTS). So it cannot "cheat" by seeing their cards.

**Deck profile** (`ygoe-cli profile`): what a deck depends on (monster effects, GY, Spell/Trap zone, searches, Special Summons), its key engine cards, and its interruptions.

**Side plan** (`ygoe-cli side`): swaps in and out for going first and for going second, each with a reason ("removes Spell/Trap-zone cards; 77% of their plays use the Spell/Trap zone").
- With `--simulate N`, it plays N opening turns for the opponent with proxy hands. It then measures what their boards actually contain and rates your board breakers against that.

## How the evaluation adapts to the opponent

1. **Static profile.** `tools/analyze_scripts.py` reads every card's Lua script: effect types, ranges, categories and once-per-turn limits.
   - From that, each card's interruptions are classified: negate activation, negate effect, negate summon, removal, deck hate, draw, and so on.
   - The opponent's deck is profiled from the same data.
   - An interruption's value is scaled by what the opponent's deck relies on. Ash Blossom is worth more against search-heavy decks, Effect Veiler against monster-effect decks, Spell/Trap removal against decks that live in the Spell/Trap zone.
2. **Simulation.** Phase B plays the opponent's actual cards against your board, so the final ranking comes from games rather than only the heuristic.

## Build

Requirements: a C++17 compiler, CMake ≥ 3.16, Python 3, git.

```sh
tools/bootstrap.sh          # fetches pinned ygopro-core, Lua, CardScripts, BabelCDB; exports data/*.tsv
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Run the commands from the repository root (or pass `--data` and `--scripts`).

## Usage

```sh
build/ygoe-cli analyze examples/angelechy_first_practice.txt
build/ygoe-cli analyze examples/angelechy_first_real.txt --iterations 6000 --samples 16
build/ygoe-cli analyze examples/angelechy_first_practice.txt --json --out result.json
build/ygoe-cli profile examples/decks/elfnote_koller_wien_2026-11.ydk
build/ygoe-cli side examples/decks/angelechy_luna_mardelplata_2026-11.ydk examples/decks/elfnote_koller_wien_2026-11.ydk --simulate 8
build/ygoe-cli proxy examples/decks/elfnote_koller_wien_2026-11.ydk 5
```

Useful `analyze` options:

| Option | Default | Meaning |
|---|---|---|
| `--iterations` | 3000 | Search iterations for your turn |
| `--seconds` | 60 | Time cap |
| `--threads` | all cores | Worker threads |
| `--samples` | 12 | Hidden-information samples (real mode / proxy hands) |
| `--lines` | 3 | Lines to report |
| `--verify-iterations` | 400 | Opponent-turn iterations per checked board |
| `--no-verify` | | Skip the opponent-turn check |
| `--horizon` | auto | Turns to search (auto: through the end of your next turn) |
| `--json`, `--out FILE` | | Machine-readable output for the app |

State files are described in [docs/state-format.md](docs/state-format.md).

## Android

`android/ygoe` is an Android library module containing:
- the native engine for arm64-v8a and x86_64 (emulator)
- the Kotlin API `com.ygoengine.YgoEngine`
- the asset pack: every card script and the card data, about 17 MB compressed in the APK

The API is described in [docs/api.md](docs/api.md). The build generates the pack with `tools/bootstrap.sh` and `tools/make_pack.py`, so it needs git and python3. GitHub Actions runners have both.

```sh
cd android && ./gradlew :ygoe:assembleRelease    # -> ygoe/build/outputs/aar/ygoe-release.aar
```

### Adding it to YGO Sandbox

The module uses the same AGP (8.7.3), Kotlin (2.0.21), minSdk 26 and JDK 17 as YGO Sandbox.

1. Add this repo as a submodule:
   ```sh
   git submodule add https://github.com/0megaXP/Yu-Gi-Oh-Engine engine
   ```
2. In `settings.gradle.kts`, add:
   ```kotlin
   include(":ygoe")
   project(":ygoe").projectDir = file("engine/android/ygoe")
   ```
3. In the root `build.gradle.kts`, add:
   ```kotlin
   id("com.android.library") version "8.7.3" apply false
   ```
4. In `app/build.gradle.kts`, add:
   ```kotlin
   implementation(project(":ygoe"))
   ```
5. In the workflow, check out with `submodules: true`.

## Tests

`tools/test.sh` builds everything on the desktop and runs:
- the CLI on the examples
- the JSON API
- the JNI bridge through a small Java harness (load, progress, cancel, profile, side, bad input)

CI (`.github/workflows/build.yml`) runs these and also builds the Android AAR.

## Matchup included

| Deck | Source |
|---|---|
| `examples/decks/angelechy_luna_mardelplata_2026-11.ydk` | Fernando Luna, Mar Del Plata WCQ Regional Top 8 (2026-11-07) |
| `examples/decks/elfnote_koller_wien_2026-11.ydk` | Philipp Koller (Ars Magna Elfnote), Wien WCQ Regional Top 8 (2026-11-07) |

Both lists come from YGOPRODeck. They are the fixed decks of the tests and examples; `decks/`
holds the format's decks the learning plays, picked again every Learn run from the last 2 weeks
of top cuts (`tools/ml/meta.py`, see `tools/ml/README.md`).

## Layout

```
engine/src/carddb.*    card data, script-derived effect metadata, effect texts
engine/src/duel.*      replayable wrapper around one ygopro-core duel; decision parsing
engine/src/deck.*      .ydk loading, proxy hands, state files, hidden-info sampling
engine/src/profile.*   interruption classification and deck profiles
engine/src/evaluate.*  board evaluation tuned to the opponent profile
engine/src/search.*    multithreaded information-set MCTS
engine/src/analyze.*   phase A + phase B pipeline, text/JSON reports
engine/src/side.*      side-deck planner
engine/src/api.*       JSON-in/JSON-out Engine class (what the app calls)
engine/src/json.*      small JSON reader
engine/jni/            JNI bridge for com.ygoengine.YgoNative
engine/cli/main.cpp    command-line front end
android/ygoe/          Android library: native build, asset pack, Kotlin API
tools/                 bootstrap, card export, script analysis, asset pack, tests
data/passcode_map.tsv  official passcodes → EDOPro ids for cards still on pre-release ids
```

## Known limits (v1)

- **Cost per iteration.** ygopro-core cannot copy a running duel, so every search iteration rebuilds the duel and replays the responses. Scripts are cached as precompiled Lua bytecode, which brings duel creation to about 5 ms. Expect a few hundred iterations per second per core. Long combos need several thousand iterations to be found reliably.
- **State files.** Puzzle setups can't express "already used this turn" or "summoned this turn". To analyse a live game exactly, drive the duel through the engine from turn 1 (the `history` of a `RootState`) instead of describing the board.
- **Heuristic parts.** The static interruption values and the side planner's card-text keywords are heuristics. The opponent-turn simulation is what grounds the final ranking.
- **Passcode map.** `data/passcode_map.tsv` maps Beyond the Brave passcodes to EDOPro's pre-release ids. Every row was checked against YGOPRODeck; decks sent by the app also carry card names, and the engine corrects any passcode whose name doesn't match.

## License note

ygopro-core is AGPL-3.0, and the engine links it. Anything that ships this engine, including an Android app, must comply with the AGPL. ProjectIgnis card scripts and BabelCDB keep their own licenses.

## Windows (desktop apps)

`tools/build_windows.sh <out>` cross-compiles the engine for 64-bit Windows on Linux (MinGW-w64) and
writes `<out>/ygoe_jni.dll` (needs only `KERNEL32.dll` and `msvcrt.dll`) and the asset pack
`<out>/ygoe/`. The Kotlin API for desktop JVM apps is `android/ygoe-jvm` (same sources as the Android
module, in `android/ygoe/src/shared/kotlin`): set the system property `ygoe.library` to the DLL's
full path, then call `YgoEngine.open(packDir)`.
