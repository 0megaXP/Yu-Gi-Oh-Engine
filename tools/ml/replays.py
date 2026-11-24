#!/usr/bin/env python3
"""The app's real games for the nightly run: replays-*.jsonl.gz files (YGO Sandbox uploads them,
the newest has the largest timestamp in its name) -> training positions and start positions.

usage: replays.py <dir with replays-*.jsonl.gz> <positions.jsonl> <starts.jsonl> [follow_report.json]

The app also sends a line per game on how far the engine followed it ("kind":"follow": the
replay's turns, the turns followed, why and where it stopped). They are summed up in the
report: how many games were followed to the end, and the cards most often next where it
stopped (what to fix in the follower first).

A game sent again (the app re-sends its games when the engine learns to read more from them)
counts once: the lines of the newest file holding it.
"""
import collections
import glob
import gzip
import json
import os
import sys


def main():
    src, pos_out, starts_out = sys.argv[1:4]
    report = sys.argv[4] if len(sys.argv) > 4 else None
    follows = []
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
                    if d.get("kind") == "follow":
                        follows.append(d)
                    elif d.get("kind") == "start":
                        so.write(line + "\n")
                        starts += 1
                    elif "f" in d:
                        po.write(line + "\n")
                        positions += 1
    print(f"{len(files)} files, {games} games: {positions} positions, {starts} start positions")
    if follows:
        stops = collections.Counter()
        for f in follows:
            nxt = f.get("stop_next", "")
            if nxt:
                first = nxt.split("]")[0].lstrip("[")
                code = next((w[1:] for w in first.split() if w.startswith("c") and w[1:].isdigit() and w != "c0"), "hidden")
                stops[(first.split()[0], code)] += 1
        rep = {
            "games": len(follows),
            "complete": sum(1 for f in follows if f.get("complete")),
            "turns": sorted(collections.Counter(f.get("turns", 0) for f in follows).items()),
            "stopped_in_turn": sorted(collections.Counter(f.get("stop_turn", 0) for f in follows if not f.get("complete")).items()),
            "followed_share": round(sum(f.get("followed", 0) for f in follows) / max(1, sum(f.get("events", 0) for f in follows)), 3),
            "why": collections.Counter(f.get("stopped", "") for f in follows if not f.get("complete")).most_common(),
            "next_where_stopped": [[k, c, n] for (k, c), n in stops.most_common(25)],
        }
        print("followed:", json.dumps({k: v for k, v in rep.items() if k != "next_where_stopped"}))
        print("most common stops (event, card id, games):", rep["next_where_stopped"][:10])
        if report:
            json.dump(rep, open(report, "w"), indent=1)


if __name__ == "__main__":
    main()
