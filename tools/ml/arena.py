#!/usr/bin/env python3
"""Decides the arena (ygoe-cli arena results in a folder): keep the new network and policy only
when the bots using them are clearly better.

usage: arena.py <dir with arena-*.json>   (prints the decision as JSON)

Each deal was played twice, the bots swapping seats, so a pair's score (0..1) has most of the
luck of the draw cancelled. Kept when the average pair score is at least 0.52 and more than one
standard error above 0.5 (about 84% sure the new bots are better, not lucky).
"""
import glob
import json
import math
import os
import sys


def main():
    pairs, games, points = [], 0, 0.0
    for f in sorted(glob.glob(os.path.join(sys.argv[1], "arena-*.json"))):
        try:
            r = json.load(open(f))
        except (OSError, ValueError):
            continue
        pairs += r.get("pairs", [])
        games += r["games"]
        points += r["score"] * r["games"]
    score = points / games if games else 0.5
    n = len(pairs)
    mean = sum(pairs) / n if n else 0.5
    sd = math.sqrt(sum((p - mean) ** 2 for p in pairs) / (n - 1)) if n > 1 else 0.5
    se = sd / math.sqrt(n) if n else 0.5
    keep = n >= 20 and mean >= 0.52 and mean - se > 0.5
    print(json.dumps({"score": score, "games": games, "pairs": n, "pair_mean": mean, "standard_error": se, "keep": keep}))


if __name__ == "__main__":
    main()
