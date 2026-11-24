#!/usr/bin/env python3
"""The deck library the bots guess the opponent's deck from (engine/src/guess.h).

usage: decklib.py <decks dir> [--starts starts.jsonl] [--cards data/cards.tsv] --out data/deck_library.json

Lists: every .ydk in the decks folder (the format's top cuts: weight 1 each) and the decks of
real games, from the app's replay start positions (tools/ml/replays.py: each game's two
decks). A replay list counts once per game it was played in, so the decks met more often weigh
more. The replays are one player's games, so their own deck is in nearly all of them: a list in
more than a third of the games (at least 3) is theirs, not an opponent's, and counts once.
"""
import argparse
import collections
import glob
import json
import os


def read_ydk(path):
    main, part = [], None
    name = os.path.splitext(os.path.basename(path))[0]
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if line.startswith("#created by"):
            # "#created by Yu-Gi-Oh! Engine — Ars Magna Elfnote, Philipp Koller, ..." -> the deck's name
            desc = line.split("—", 1)[-1].strip()
            if desc and desc != line:
                name = desc.split(",")[0].strip() + " (" + name + ")"
            continue
        if line.startswith("#") or line.startswith("!"):
            part = line
            continue
        if part == "#main" and line.isdigit():
            main.append(int(line))
    return name, main


def names(path):
    out = {}
    try:
        for line in open(path, encoding="utf-8"):
            f = line.rstrip("\n").split("\t")
            if f and f[0].isdigit():
                out[int(f[0])] = f[-1]
    except OSError:
        pass
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("decks")
    ap.add_argument("--starts")
    ap.add_argument("--cards", default="data/cards.tsv")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    out = []
    for p in sorted(glob.glob(os.path.join(a.decks, "*.ydk"))):
        name, cards = read_ydk(p)
        if len(cards) >= 20:
            out.append({"name": name, "main": cards, "weight": 1, "src": "top cut"})
    games = collections.defaultdict(set)  # list -> games
    if a.starts and os.path.exists(a.starts):
        for line in open(a.starts):
            try:
                d = json.loads(line)
            except ValueError:
                continue
            if d.get("kind") != "start":
                continue
            for deck in d.get("decks", []):
                m = deck.get("main", [])
                if len(m) >= 20:
                    games[tuple(sorted(m))].add(d.get("game", line))
    n_games = len(set().union(*games.values())) if games else 0
    known = {tuple(sorted(x["main"])) for x in out}
    nm = names(a.cards)
    # cards in half the lists or more (staples) don't name a deck
    lists = [set(x["main"]) for x in out] + [set(k) for k in games]
    share = collections.Counter(c for l in lists for c in l)
    staple = {c for c, k in share.items() if k >= len(lists) / 2}
    for lst, gs in sorted(games.items(), key=lambda kv: -len(kv[1])):
        if lst in known:
            continue
        w = len(gs)
        if n_games >= 3 and w > n_games / 3:
            w = 1  # the player's own deck
        count = collections.Counter(lst)
        top = max(count, key=lambda c: (c not in staple, count[c], -c))
        out.append({"name": "real game: " + nm.get(top, str(top)) + " deck", "main": list(lst), "weight": w, "src": "replay"})
    json.dump({"version": 1, "decks": out}, open(a.out, "w"), separators=(",", ":"))
    print(json.dumps({"decks": len(out), "from_replays": sum(1 for x in out if x["src"] == "replay"), "games": n_games}))


if __name__ == "__main__":
    main()
