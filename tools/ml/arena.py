#!/usr/bin/env python3
"""Decides the arena (ygoe-cli arena results in a folder): which new parts to keep.

usage: arena.py <dir with arena-<part>-<seed>.json>   (prints the decision as JSON)

Each new part (value_net, policy, nonengine_timing) was tested alone: the current bots with
only that part replaced, against the current bots. Each deal was played twice, the bots swapping
seats, so a pair's score (0..1) has most of the luck of the draw cancelled. A part is kept when
its average pair score is at least 0.52 and more than one standard error above 0.5 (about 84%
sure it makes the bots better, not lucky), over at least 20 pairs.
"""
import glob
import json
import math
import os
import re
import sys


def decide(results):
    pairs, games, points = [], 0, 0.0
    for r in results:
        pairs += r.get("pairs", [])
        games += r["games"]
        points += r["score"] * r["games"]
    n = len(pairs)
    mean = sum(pairs) / n if n else 0.5
    sd = math.sqrt(sum((p - mean) ** 2 for p in pairs) / (n - 1)) if n > 1 else 0.5
    se = sd / math.sqrt(n) if n else 0.5
    return {"score": points / games if games else 0.5, "games": games, "pairs": n, "pair_mean": mean,
            "standard_error": se, "keep": n >= 20 and mean >= 0.52 and mean - se > 0.5}


def main():
    by_part = {}
    for f in sorted(glob.glob(os.path.join(sys.argv[1], "arena-*.json"))):
        m = re.match(r"arena-(?:(.+)-)?\d+\.json$", os.path.basename(f))
        part = (m.group(1) if m and m.group(1) else "all")
        try:
            by_part.setdefault(part, []).append(json.load(open(f)))
        except (OSError, ValueError):
            continue
    parts = {p: decide(rs) for p, rs in sorted(by_part.items())}
    print(json.dumps({"parts": parts, "keep": any(d["keep"] for d in parts.values())}))


if __name__ == "__main__":
    main()
