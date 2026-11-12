# State file format

A plain-text file that describes one position for `ygoe-cli analyze`. Each line is `key: value`. Lines starting with `#` are comments.

A key ending in `0` or `1` refers to that player. **Player 0 takes turn 1.** To analyse a position where the opponent is the turn player, put the opponent's cards on side 0 and set `me: 1`.

| Key | Example | Meaning |
|---|---|---|
| `deck0`, `deck1` | `deck0: ../decks/angelechy.ydk` | `.ydk` decklist, path relative to the state file. Must come before cards are named. |
| `me` | `me: 0` | The side the engine plays for. |
| `mode` | `mode: real` | `practice` (both hands known, default) or `real` (only your own hand is known). |
| `hand0`, `hand1` | `hand0: Ash Blossom & Joyous Spring; Angelechy Problem` | Hand, separated by `;`. Cards can be names, unique name prefixes or passcodes. |
| | `hand1: proxy 5` / `hand1: hidden 5` | Deal a random hand of that size from the decklist, resampled for every search sample. |
| `field0`, `field1` | `field0: mzone 2 atk; Angelechy Bastion` | A card on the field: `mzone`/`szone`, the zone index (0–4; 5 = Field Zone for `szone`), and an optional position (`atk`, `def`, `set`, `up`). |
| `grave0`, `grave1` | `grave0: Angelechy Opposition; Foolish Burial Goods` | Cards in the GY. |
| `banished0`, `banished1` | | Banished cards (face-up). |
| `lp0`, `lp1` | `lp1: 4300` | Life points (default 8000). |
| `seed` | `seed: 7` | Changes deck order and sampling. |

Cards named in hands, on the field, in the GY or banished are removed from that player's Main Deck (or Extra Deck) automatically.

## Limits

- The duel always starts at the beginning of turn 1, so once-per-turn uses, "summoned this turn" and similar flags are fresh.
- Setting up a board this way is exact for the cards, but approximate for anything that depends on what already happened earlier in the game.
- For exact mid-game analysis, play the game through the engine and pass the response history in a `RootState` (library API).
