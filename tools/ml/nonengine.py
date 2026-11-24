#!/usr/bin/env python3
"""Non-engine learning (engine/src/nonengine.h) from self-play.

usage: nonengine.py timing <chances.jsonl> [...] --out nonengine_timing.json [--init old.json] [--report r.json]
       nonengine.py values <games.jsonl> [...] --out nonengine_values.json [--report r.json]

timing: each line is a chance a bot had to use a non-engine card on the opponent's play
  (`ygoe-cli selfplay --ne-out`): the situation's features `f`, whether the card was used `u`
  (self-play turns a share of the choices around, so both are seen) and the game's result for
  that player `z`. Two logistic models, P(win | used) and P(win | held), on the same features;
  the engine uses the difference as how much using the card now is worth. 15% of the games are
  held out; the report gives their log-loss with the features and with a constant per choice
  (the baseline), so the nightly run keeps the model only when the situation tells it something.

values: each line is a game (`ygoe-cli selfplay --games-out`): both players' archetypes (player 0
  went first), the result, and for each non-engine card in a player's Main Deck whether it was in
  their hand in the first 4 turns. A card's value in a matchup, going first or second, is the win
  rate when it was drawn early minus when it wasn't (each shrunk towards 0.5 by 10 games), also
  against the whole format (opp 0). Cards need 5 games each way. Also each matchup's win rate of
  the player going first (`first`: shrunk towards 0.5 by 10 games, also against the whole
  format), and of the player going second (`first` 0), for who chooses to go first or second after
  losing a game of a match.
"""
import argparse
import json
import math
import sys
import zlib

import numpy as np

NF = 20  # nonengine.h kNeFeatures


def lines(paths):
    for p in paths:
        try:
            fh = open(p)
        except OSError:
            print(f"skipping {p}: no such file", file=sys.stderr)
            continue
        with fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                try:
                    yield json.loads(line)
                except json.JSONDecodeError:
                    continue  # a line cut by a stopped job


def held_out(game):
    return zlib.crc32(game.encode()) % 100 < 15


def logloss(p, z):
    p = np.clip(p, 1e-6, 1 - 1e-6)
    return float(-np.mean(z * np.log(p) + (1 - z) * np.log(1 - p))) if len(z) else 0.0


def fit(X, z, w0=None, l2=1e-2, epochs=300, lr=0.5):
    w = np.zeros(X.shape[1]) if w0 is None else np.array(w0, dtype=np.float64)
    for _ in range(epochs):
        p = 1 / (1 + np.exp(-(X @ w)))
        g = X.T @ (p - z) / len(z) + l2 * w
        w -= lr * g
    return w


def timing(a):
    F, Z, U, V = [], [], [], []
    for d in lines(a.data):
        if len(d.get("f", [])) != NF:
            continue
        F.append(d["f"])
        Z.append(d["z"])
        U.append(int(d["u"]))
        V.append(held_out(d.get("g", "")))
    if len(F) < 200:
        sys.exit(f"only {len(F)} non-engine chances")
    F, Z, U, V = np.array(F, float), np.array(Z, float), np.array(U), np.array(V)
    init = json.load(open(a.init)) if a.init else None
    rep = {"chances": int(len(F)), "used_share": float(U.mean())}
    models = {}
    val_p = np.zeros(len(F))
    base_p = np.zeros(len(F))
    for name, u in (("use", 1), ("hold", 0)):
        tr = (U == u) & ~V
        va = (U == u) & V
        w = fit(F[tr], Z[tr], init[name] if init else None)
        models[name] = [round(float(x), 6) for x in w]
        val_p[va] = 1 / (1 + np.exp(-(F[va] @ w)))
        base_p[va] = Z[tr].mean() if tr.any() else 0.5
    rep["validation_chances"] = int(V.sum())
    rep["base_logloss"] = logloss(base_p[V], Z[V])
    rep["timing_logloss"] = logloss(val_p[V], Z[V])
    # how much using a card is worth, on average, by the model (positive: using is better)
    pu = 1 / (1 + np.exp(-(F @ np.array(models["use"]))))
    ph = 1 / (1 + np.exp(-(F @ np.array(models["hold"]))))
    rep["average_gain"] = float(np.mean(pu - ph))
    json.dump({"version": 1, "features": NF, **models}, open(a.out, "w"), separators=(",", ":"))
    print(json.dumps(rep, indent=1))
    if a.report:
        json.dump(rep, open(a.report, "w"), indent=1)


def values(a):
    # key -> [wins seen, n seen, wins unseen, n unseen]
    acc = {}
    first_acc = {}  # (me, opp, went first) -> [wins, games]
    games = 0
    for d in lines(a.data):
        arch = d.get("arch")
        if not arch or len(arch) != 2:
            continue
        games += 1
        for p in (0, 1):
            for o in (int(arch[1 - p]), 0):
                f = first_acc.setdefault((int(arch[p]), o, p == 0), [0.0, 0])
                f[0] += d["z0"] if p == 0 else 1 - d["z0"]
                f[1] += 1
        for p in (0, 1):
            z = d["z0"] if p == 0 else 1 - d["z0"]
            me, opp, first = int(arch[p]), int(arch[1 - p]), p == 0
            for c in d["ne"][p]:
                for o in (opp, 0):
                    k = (me, o, first, int(c["c"]))
                    s = acc.setdefault(k, [0.0, 0, 0.0, 0])
                    if c["seen"]:
                        s[0] += z
                        s[1] += 1
                    else:
                        s[2] += z
                        s[3] += 1
    out = []
    for (me, opp, first, code), (ws, ns, wu, nu) in sorted(acc.items()):
        if ns < 5 or nu < 5:
            continue
        seen = (ws + 5) / (ns + 10)
        unseen = (wu + 5) / (nu + 10)
        out.append({"me": me, "opp": opp, "first": 1 if first else 0, "code": code,
                    "delta": round(seen - unseen, 4), "games": ns + nu})
    first = [{"me": me, "opp": opp, "first": 1 if f else 0, "rate": round((w + 5) / (n + 10), 4), "games": n}
             for (me, opp, f), (w, n) in sorted(first_acc.items()) if n >= 5]
    json.dump({"version": 1, "games": games, "entries": out, "first": first}, open(a.out, "w"), separators=(",", ":"))
    rep = {"games": games, "entries": len(out), "first_matchups": len(first)}
    print(json.dumps(rep))
    if a.report:
        json.dump(rep, open(a.report, "w"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("what", choices=["timing", "values"])
    ap.add_argument("data", nargs="+")
    ap.add_argument("--out", required=True)
    ap.add_argument("--init")
    ap.add_argument("--report")
    a = ap.parse_args()
    timing(a) if a.what == "timing" else values(a)


if __name__ == "__main__":
    main()
