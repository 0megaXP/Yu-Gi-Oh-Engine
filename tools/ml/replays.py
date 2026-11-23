#!/usr/bin/env python3
"""The app's real games for the nightly run: replays-*.jsonl.gz files (YGO Sandbox uploads them,
the newest has the largest timestamp in its name) -> training positions and start positions.

usage: replays.py <dir with replays-*.jsonl.gz> <positions.jsonl> <starts.jsonl>

A game sent again (the app re-sends its games when the engine learns to read more from them)
counts once: the lines of the newest file holding it.
"""
import glob
import gzip
import json
import os
import sys


def main():
    src, pos_out, starts_out = sys.argv[1:4]
    files = sorted(glob.glob(os.path.join(src, "replays-*.jsonl.gz")),
                   key=lambda p: int(os.path.basename(p).split("-")[1]) if os.path.basename(p).split("-")[1].isdigit() else 0,
                   reverse=True)
    seen = set()
    games = positions = starts = 0
    with open(pos_out, "w") as po, open(starts_out, "w") as so:
        for path in files:
            by_game = {}
            try:
                with gzip.open(path, "rt") as fh:
                    for line in fh:
                        line = line.strip()
                        if not line:
                            continue
                        try:
                            d = json.loads(line)
                        except json.JSONDecodeError:
                            continue
                        by_game.setdefault(d.get("game", ""), []).append((d, line))
            except (OSError, EOFError):
                continue
            for game, lines in by_game.items():
                if game in seen:
                    continue
                seen.add(game)
                games += 1
                for d, line in lines:
                    if d.get("kind") == "start":
                        so.write(line + "\n")
                        starts += 1
                    elif "f" in d:
                        po.write(line + "\n")
                        positions += 1
    print(f"{len(files)} files, {games} games: {positions} positions, {starts} start positions")


if __name__ == "__main__":
    main()
