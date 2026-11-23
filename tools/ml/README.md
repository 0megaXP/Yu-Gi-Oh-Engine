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

## The nightly pipeline (`.github/workflows/ml.yml`, "Learn")

1. **Self-play**: 8 runners × 4 processes play bot games (all decks in `decks/` and
   `bench/decks/`, random pairs, Normal bots at 200 iterations, games cut at turn 12) for 4.5
   hours, with the current network. Each Main Phase decision is written from both sides.
2. **Train**: `train.py` learns from tonight's positions plus the last 7 nights' (stored in the
   `ml-data` release), continuing from the current network. 15% of the games are held out.
3. **Check**: the new network must predict the held-out results better than the plain evaluator
   (validation log-loss lower by 0.002).
4. **Arena**: 40 bot games, the new network against the current one, both seats and decks in
   turn. It replaces `data/value_net.json` only with a score of 0.55 or more; the commit message
   carries the training report.

Run it by hand from the Actions tab ("Learn", Run workflow), with fewer hours or shards for a
quick try. Reports (`learn-report` artifact): validation log-loss, arena games, the trained
network even when it was not kept.

The app uses a new network once its engine is updated to that commit (the usual engine bump).

## By hand

```
build/ygoe-cli selfplay decks/a.ydk decks/b.ydk --games 20 --out positions.jsonl
python3 tools/ml/train.py positions.jsonl --out value_net.json --report report.json
build/ygoe-cli arena decks/a.ydk decks/b.ydk --games 10 --net value_net.json --vs none
```
