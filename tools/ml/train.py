#!/usr/bin/env python3
"""Trains the value network (engine/src/valuenet.h) from self-play positions.

usage: train.py positions.jsonl [more.jsonl ...] --out value_net.json [--init old.json]
                [--hidden 32,16] [--epochs 60] [--seed 1] [--lambda 0.95] [--report report.json]

Each position has the evaluator's features `f`, its raw value `raw` (yours minus the opponent's)
and the game's result `z` (1, 0, 0.5 for a draw; for a game cut short, the evaluator's win chance
where it stopped). The engine's score is sigmoid(raw / 2.5); the network learns a correction c(f)
so that sigmoid((raw + c) / 2.5) predicts the result. The last layer starts at zero, so an
untrained network changes nothing.

Targets are TD(lambda): a position's target blends the result with the current network's
estimates at the later positions of the same game (from the same side), weighted
(1 - lambda) * lambda^(k-1) for the position k steps later and the rest on the result. So an early
position that was fine is not marked "lost" because of a misplay much later. lambda = 1 is the
plain result. Positions of real games (`"src":"replay"`) count 3 times.

Games are split 85/15 into training and validation; the report gives the validation log-loss
against the games' results (not the blended targets) of the plain evaluator and of the corrected
one (lower is better), which the nightly pipeline uses as a first check before the arena.
"""
import argparse
import json
import math
import os
import sys

import numpy as np

T = 2.5  # the evaluator's temperature: score = sigmoid(raw / T)


def load(paths):
    F, R, Z, G, S, W = [], [], [], [], [], []
    seq = {}
    for p in paths:
        if not os.path.isfile(p):
            print(f"skipping {p}: no such file", file=sys.stderr)
            continue
        with open(p) as fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                try:
                    d = json.loads(line)
                except json.JSONDecodeError:
                    continue  # a line cut by a stopped job
                if "f" not in d or "kind" in d:
                    continue  # not a position (a start position for self-play, ...)
                g = d.get("game", "")
                # the side and order within the game: written, or (older files, replays) the lines
                # come in pairs, one per side, in the game's order
                k = seq.get(g, 0)
                seq[g] = k + 1
                me = int(d.get("me", k % 2))
                i = int(d.get("i", k // 2))
                F.append(d["f"])
                R.append(d["raw"])
                Z.append(d["z"])
                G.append(g)
                S.append((g, me, i))
                W.append(3.0 if d.get("src") == "replay" else 1.0)
    if not F:
        sys.exit("no positions")
    n = len(F[0])
    keep = [i for i, f in enumerate(F) if len(f) == n]
    F = np.array([F[i] for i in keep], dtype=np.float64)
    R = np.array([R[i] for i in keep], dtype=np.float64)
    Z = np.array([Z[i] for i in keep], dtype=np.float64)
    W = np.array([W[i] for i in keep], dtype=np.float64)
    G = [G[i] for i in keep]
    S = [S[i] for i in keep]
    return F, np.clip(R, -40, 40), Z, G, S, W


def td_targets(R, Z, S, V, lam):
    """TD(lambda) targets: per game and side, from the last position back,
    target_t = (1 - lam) * V_{t+1} + lam * target_{t+1}, the last one's being its result."""
    if lam >= 1:
        return Z.copy()
    Y = Z.copy()
    groups = {}
    for idx, (g, me, i) in enumerate(S):
        groups.setdefault((g, me), []).append((i, idx))
    for lst in groups.values():
        lst.sort()
        nxt = None
        for _, idx in reversed(lst):
            if nxt is not None:
                Y[idx] = (1 - lam) * V[nxt] + lam * Y[nxt]
            nxt = idx
    return Y


def logloss(raw, c, z, w=None):
    p = 1 / (1 + np.exp(-(raw + c) / T))
    p = np.clip(p, 1e-6, 1 - 1e-6)
    l = -(z * np.log(p) + (1 - z) * np.log(1 - p))
    return float(np.average(l, weights=w))


class Net:
    def __init__(self, sizes, rng, scale=2.0):
        self.W, self.b = [], []
        for i in range(len(sizes) - 1):
            fan = sizes[i]
            w = rng.normal(0, 1 / math.sqrt(fan), (sizes[i + 1], fan))
            if i == len(sizes) - 2:
                w[:] = 0  # starts as "no correction"
            self.W.append(w)
            self.b.append(np.zeros(sizes[i + 1]))
        self.scale = scale

    def forward(self, X):
        acts = [X]
        a = X
        for i, (w, b) in enumerate(zip(self.W, self.b)):
            s = a @ w.T + b
            a = np.tanh(s) if i < len(self.W) - 1 else s
            acts.append(a)
        return acts

    def correction(self, X):
        return self.scale * self.forward(X)[-1][:, 0]


def train(F, R, Z, G, S, W, hidden, epochs, seed, init=None, lam=0.95, l2=1e-4, lr=3e-3, batch=256):
    rng = np.random.default_rng(seed)
    games = sorted(set(G))
    rng.shuffle(games)
    val_games = set(games[: max(1, int(len(games) * 0.15))])
    va = np.array([g in val_games for g in G])
    tr = ~va
    mean = F[tr].mean(0)
    std = F[tr].std(0)
    std[std < 1e-6] = 1.0
    X = (F - mean) / std
    sizes = [F.shape[1]] + hidden + [1]
    net = Net(sizes, rng)
    if init is not None and [l["out"] for l in init["layers"]] == sizes[1:] and init["layers"][0]["in"] == sizes[0]:
        # continue from the current network (same shape): weights in its own normalisation
        for i, l in enumerate(init["layers"]):
            net.W[i] = np.array(l["w"]).reshape(l["out"], l["in"])
            net.b[i] = np.array(l["b"])
        mean = np.array(init["mean"])
        std = np.array(init["std"])
        X = (F - mean) / std
        net.scale = init.get("scale", net.scale)
    # TD(lambda) targets from the starting network's estimates (the plain evaluator when new)
    V = 1 / (1 + np.exp(-(R + net.correction(X)) / T))
    Y = td_targets(R, Z, S, V, lam)
    m = [np.zeros_like(w) for w in net.W] + [np.zeros_like(b) for b in net.b]
    v = [np.zeros_like(w) for w in net.W] + [np.zeros_like(b) for b in net.b]
    t = 0
    idx = np.where(tr)[0]
    best = (logloss(R[va], net.correction(X[va]), Z[va], W[va]), [w.copy() for w in net.W], [b.copy() for b in net.b])
    for ep in range(epochs):
        rng.shuffle(idx)
        for s in range(0, len(idx), batch):
            bi = idx[s : s + batch]
            acts = net.forward(X[bi])
            c = net.scale * acts[-1][:, 0]
            p = 1 / (1 + np.exp(-(R[bi] + c) / T))
            # d loss / d out (out = last layer's value, c = scale * out)
            g = ((p - Y[bi]) * W[bi] / T * net.scale / W[bi].sum())[:, None]
            grads_w, grads_b = [None] * len(net.W), [None] * len(net.W)
            for i in range(len(net.W) - 1, -1, -1):
                grads_w[i] = g.T @ acts[i] + l2 * net.W[i]
                grads_b[i] = g.sum(0)
                if i > 0:
                    g = (g @ net.W[i]) * (1 - acts[i] ** 2)
            t += 1
            params = net.W + net.b
            grads = grads_w + grads_b
            for k in range(len(params)):
                m[k] = 0.9 * m[k] + 0.1 * grads[k]
                v[k] = 0.999 * v[k] + 0.001 * grads[k] ** 2
                mh = m[k] / (1 - 0.9**t)
                vh = v[k] / (1 - 0.999**t)
                params[k] -= lr * mh / (np.sqrt(vh) + 1e-8)
        vl = logloss(R[va], net.correction(X[va]), Z[va], W[va])
        if vl < best[0]:
            best = (vl, [w.copy() for w in net.W], [b.copy() for b in net.b])
    last_train = logloss(R[tr], net.correction(X[tr]), Z[tr], W[tr])
    net.W, net.b = best[1], best[2]
    report = {
        "last_epoch_train_logloss": last_train,
        "positions": int(len(Z)),
        "games": len(games),
        "validation_games": len(val_games),
        "base_logloss": logloss(R[va], np.zeros(int(va.sum())), Z[va], W[va]),
        "net_logloss": best[0],
        "base_train_logloss": logloss(R[tr], np.zeros(int(tr.sum())), Z[tr], W[tr]),
        "net_train_logloss": logloss(R[tr], net.correction(X[tr]), Z[tr], W[tr]),
        "lambda": lam,
        "replay_positions": int((W > 1).sum()),
    }
    return net, mean, std, report


def save(net, mean, std, path, ident):
    layers = []
    for w, b in zip(net.W, net.b):
        layers.append({"in": int(w.shape[1]), "out": int(w.shape[0]), "w": [round(float(x), 6) for x in w.ravel()],
                       "b": [round(float(x), 6) for x in b]})
    doc = {"version": 1, "id": ident, "features": int(len(mean)), "mean": [round(float(x), 6) for x in mean],
           "std": [round(float(x), 6) for x in std], "layers": layers, "scale": net.scale}
    with open(path, "w") as fh:
        json.dump(doc, fh, separators=(",", ":"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("data", nargs="+")
    ap.add_argument("--out", required=True)
    ap.add_argument("--init")
    ap.add_argument("--hidden", default="32,16")
    ap.add_argument("--epochs", type=int, default=60)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--lambda", dest="lam", type=float, default=0.95)
    ap.add_argument("--report")
    ap.add_argument("--id", default="")
    a = ap.parse_args()
    F, R, Z, G, S, W = load(a.data)
    init = json.load(open(a.init)) if a.init else None
    hidden = [int(x) for x in a.hidden.split(",") if x]
    net, mean, std, rep = train(F, R, Z, G, S, W, hidden, a.epochs, a.seed, init, a.lam)
    save(net, mean, std, a.out, a.id)
    print(json.dumps(rep, indent=1))
    if a.report:
        json.dump(rep, open(a.report, "w"), indent=1)


if __name__ == "__main__":
    main()
