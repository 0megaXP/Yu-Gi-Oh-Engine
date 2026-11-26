#!/usr/bin/env python3
"""The format's real end boards: what the combo libraries and the app's Deepen go second against.

usage: end_boards.py --out end_boards.jsonl [--max 400] <file.jsonl | dir> [...]

Takes, from every input (jsonl files, or the .jsonl files of a directory, in order), the lines of
the start of turn 2 where the player going first left a board: "kind":"board" lines (the follower
writes one per game followed to turn 2, with the cards nobody saw left out) and "kind":"start"
lines of turn 2 (both decks known). One board per game, the input given last winning; the newest
`max` are kept. An empty result leaves `out` as it was.
"""
import json
import os
import sys


def lines(paths):
    for p in paths:
        files = sorted(os.path.join(p, f) for f in os.listdir(p) if f.endswith(".jsonl")) if os.path.isdir(p) else [p]
        for f in files:
            if not os.path.isfile(f):
                continue
            with open(f, encoding="utf-8") as fh:
                for line in fh:
                    if ('"kind":"board"' in line or '"kind":"start"' in line) and '"turn":2,' in line:
                        yield line.strip()


def main():
    args = sys.argv[1:]
    out, keep = None, 400
    paths = []
    i = 0
    while i < len(args):
        if args[i] == "--out":
            out = args[i + 1]
            i += 2
        elif args[i] == "--max":
            keep = int(args[i + 1])
            i += 2
        else:
            paths.append(args[i])
            i += 1
    if not out:
        sys.exit(__doc__)
    boards = {}  # game -> line (insertion order: oldest first)
    for line in lines(paths):
        try:
            d = json.loads(line)
        except ValueError:
            continue
        placed = d.get("placed") or [[], []]
        if len(placed) < 2 or not any(c.get("l") in (4, 8) for c in placed[1]):
            continue
        game = d.get("game") or line
        boards.pop(game, None)
        boards[game] = line
    got = list(boards.values())[-keep:]
    print(len(got), "real end boards")
    if got:
        with open(out, "w", encoding="utf-8") as fh:
            fh.write("\n".join(got) + "\n")


if __name__ == "__main__":
    main()
