#!/usr/bin/env python3
"""The app's real games for the nightly run: replays-*.jsonl.gz files (YGO Sandbox uploads them,
the newest has the largest timestamp in its name) -> training positions and start positions.

usage: replays.py <dir with replays-*.jsonl.gz> <positions.jsonl> <starts.jsonl> [follow_report.json]
       replays.py merge <positions.jsonl> <dir of followall outputs> <out.jsonl> [follow_report.json]
       replays.py inputs <dir with inputs-*.jsonl.gz> <inputs.jsonl>   (each game once: its newest upload)

The app also sends a line per game on how far the engine followed it ("kind":"follow": the
replay's turns, the turns followed, why and where it stopped). They are summed up in the
report: how many games were followed to the end, and the cards most often next where it
stopped (what to fix in the follower first).

A game sent again (the app re-sends its games when the engine learns to read more from them)
counts once: its positions from the newest file holding positions of it, its start positions
and its follow summary likewise (so an upload without positions never wipes older ones).
"""
import collections
import glob
import gzip
import json
import os
import sys


def main():
    if sys.argv[1] == "merge":
        merge(*sys.argv[2:6])
        return
    if sys.argv[1] == "inputs":
        inputs(*sys.argv[2:4])
        return
    src, pos_out, starts_out = sys.argv[1:4]
    report = sys.argv[4] if len(sys.argv) > 4 else None
    follows = []
    files = sorted(glob.glob(os.path.join(src, "replays-*.jsonl.gz")),
                   key=lambda p: int(os.path.basename(p).split("-")[1]) if os.path.basename(p).split("-")[1].isdigit() else 0,
                   reverse=True)
    seen = set()      # (game, part) taken
    seen_any = set()  # games
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
                # each part from the newest upload that has it: an upload without positions (a
                # summary only, or the engine of 24 Nov that lost them) never replaces older ones
                parts = {"follow": [], "start": [], "pos": []}
                for d, line in lines:
                    kind = d.get("kind")
                    part = "follow" if kind == "follow" else "start" if kind == "start" else "pos" if "f" in d else None
                    if part:
                        parts[part].append((d, line))
                if game not in seen_any:
                    seen_any.add(game)
                    games += 1
                for part, got in parts.items():
                    if not got or (game, part) in seen:
                        continue
                    seen.add((game, part))
                    for d, line in got:
                        if part == "follow":
                            follows.append(d)
                        elif part == "start":
                            so.write(line + "\n")
                            starts += 1
                        else:
                            po.write(line + "\n")
                            positions += 1
    print(f"{len(files)} files, {games} games: {positions} positions, {starts} start positions")
    follow_report(follows, report)

def follow_report(follows, report=None):
    """How far the games were followed, and the cards most often next where it stopped."""
    if not follows:
        return
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


def merge(app_positions, followed_dir, out, report=None):
    """The pipeline's own follows (followall on the app's uploaded game inputs) replace the
    positions the app made on the phone for the same games."""
    mine = collections.defaultdict(list)
    follows = []
    for path in sorted(glob.glob(os.path.join(followed_dir, "**", "*.jsonl"), recursive=True)):
        for line in open(path):
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            if d.get("kind") == "follow":
                follows.append(d)
            elif "f" in d:
                mine[d.get("game", "")].append(line.rstrip("\n"))
    games = {f["game"] for f in follows}
    kept = n_app = 0
    with open(out, "w") as o:
        for g, lines in mine.items():
            for l in lines:
                o.write(l + "\n")
                kept += 1
        if os.path.exists(app_positions):
            for line in open(app_positions):
                try:
                    g = json.loads(line).get("game", "")
                except json.JSONDecodeError:
                    continue
                if g not in games:
                    o.write(line)
                    n_app += 1
    print(f"{len(games)} games followed here: {kept} positions; {n_app} positions from the app for the other games")
    follow_report(follows, report)


def inputs(src, out):
    """The app's uploaded game inputs (moves and decks), each game once: from its newest upload."""
    files = sorted(glob.glob(os.path.join(src, "inputs-*.jsonl.gz")), reverse=True)
    seen = set()
    with open(out, "w") as o:
        for path in files:
            try:
                with gzip.open(path, "rt") as fh:
                    for line in fh:
                        try:
                            g = json.loads(line).get("game", "")
                        except json.JSONDecodeError:
                            continue
                        if g and g not in seen:
                            seen.add(g)
                            o.write(line if line.endswith("\n") else line + "\n")
            except (OSError, EOFError):
                continue
    print(f"{len(files)} files, {len(seen)} games to follow")

if __name__ == "__main__":
    main()
