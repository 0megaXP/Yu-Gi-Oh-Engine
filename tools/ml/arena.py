#!/usr/bin/env python3
"""Decides the arena (ygoe-cli arena results in a folder): which new parts to keep.

usage: arena.py <dir with arena-<part>-<seed>.json> [--pending pending.json] [--out-pending state.json]
                [--run RUN_ID]          (prints the decision as JSON)

Each new part (value_net, policy, nonengine_timing) was tested alone: the current bots with
only that part replaced, against the current bots. Each deal was played twice, the bots swapping
seats, so a pair's score (0..1) has most of the luck of the draw cancelled.

One run's arena (about 60 pairs a part) only tells a part worth 5-8% from luck; a night of
learning is usually worth less. So a part that is neither clearly better nor clearly worse stays
a candidate: the next runs test the same part again (not a new one) and its pairs add up
(--pending: the pairs of the earlier runs, written by --out-pending), for up to MAX_RUNS runs.

  keep:    at least 20 pairs, average >= 0.51 and more than 1.5 standard errors above 0.5
           (stricter than one look needs, since a candidate is looked at up to 4 times)
  reject:  average more than 1.5 standard errors below 0.5, or MAX_RUNS runs without a keep
  pending: otherwise (tested again next run)
"""
import argparse
import glob
import json
import math
import os
import re

MAX_RUNS = 4


def stats(pairs):
    n = len(pairs)
    mean = sum(pairs) / n if n else 0.5
    sd = math.sqrt(sum((p - mean) ** 2 for p in pairs) / (n - 1)) if n > 1 else 0.5
    se = sd / math.sqrt(n) if n else 0.5
    return n, mean, se


def decide(results, earlier=None):
    earlier = earlier or {}
    tonight, games, points = [], 0, 0.0
    for r in results:
        tonight += r.get("pairs", [])
        games += r["games"]
        points += r["score"] * r["games"]
    pairs = list(earlier.get("pairs", [])) + tonight
    runs = int(earlier.get("runs", 0)) + 1
    n, mean, se = stats(pairs)
    if n >= 20 and mean >= 0.51 and mean - 1.5 * se > 0.5:
        decision = "keep"
    elif (n >= 20 and mean + 1.5 * se < 0.5) or runs >= MAX_RUNS:
        decision = "reject"
    else:
        decision = "pending"
    return {"score": points / games if games else 0.5, "games": games, "pairs_tonight": len(tonight),
            "pairs": n, "pair_mean": mean, "standard_error": se, "runs": runs,
            "since": earlier.get("since"), "decision": decision, "keep": decision == "keep",
            "_pairs": pairs}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--pending")
    ap.add_argument("--out-pending")
    ap.add_argument("--run", default="")
    a = ap.parse_args()
    pending = {}
    if a.pending and os.path.exists(a.pending):
        try:
            pending = json.load(open(a.pending))
        except ValueError:
            pending = {}
    by_part = {}
    for f in sorted(glob.glob(os.path.join(a.dir, "arena-*.json"))):
        m = re.match(r"arena-(?:(.+)-)?\d+\.json$", os.path.basename(f))
        part = (m.group(1) if m and m.group(1) else "all")
        try:
            by_part.setdefault(part, []).append(json.load(open(f)))
        except (OSError, ValueError):
            continue
    parts = {p: decide(rs, pending.get(p)) for p, rs in sorted(by_part.items())}
    kept = any(d["keep"] for d in parts.values())
    state = {}
    for p, d in parts.items():
        if d["decision"] != "pending":
            continue
        # a part kept changes the bots the others are measured against: their pairs start over
        # (their runs still count, so they still end)
        state[p] = {"pairs": [] if kept else d["_pairs"], "runs": d["runs"], "since": d["since"] or a.run}
    for p, st in pending.items():
        if p not in parts:  # its arena didn't run (a failed job): as it was
            state[p] = {**st, "pairs": [] if kept else st.get("pairs", [])}
    for d in parts.values():
        del d["_pairs"]
    if a.out_pending:
        json.dump(state, open(a.out_pending, "w"))
    print(json.dumps({"parts": parts, "keep": kept, "pending": sorted(state)}))


if __name__ == "__main__":
    main()
