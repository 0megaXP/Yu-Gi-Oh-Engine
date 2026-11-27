#!/usr/bin/env python3
"""Packs many small uploads of the ml-data release into one archive file (a release holds at
most 1000 assets).

usage: compact.py replays <dir> <out.jsonl.gz>   replays-*.jsonl.gz (and an older archive)
       compact.py inputs  <dir> <out.jsonl.gz>   inputs-*.jsonl.gz
       compact.py lines   <dir> <glob> <out.jsonl.gz>   any other jsonl(.gz) files: all lines

The archive is read exactly like the files it replaces: replays.py takes each game's parts
(follow, start, positions) from the newest upload that has them, and each game's inputs from its
newest upload, so the archive keeps only those, and its name sorts as the oldest
(replays-0-archive, inputs-0-z-archive: after inputs-0-attached, the repo's own): a newer upload still wins over it.
Prints the files it packed, one per line, to the file given by $COMPACT_LIST (to delete them
after the archive is uploaded).
"""
import glob
import gzip
import json
import os
import sys


def stamp(path):
    p = os.path.basename(path).split("-")
    return int(p[1]) if len(p) > 1 and p[1].isdigit() else 0


def read(path):
    opener = gzip.open if path.endswith(".gz") else open
    try:
        with opener(path, "rt", encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if line:
                    yield line
    except (OSError, EOFError):
        return


def replays(src, out):
    files = sorted(glob.glob(os.path.join(src, "replays-*.jsonl.gz")), key=stamp, reverse=True)
    seen = set()
    kept = []
    for path in files:
        by_game = {}
        for line in read(path):
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            kind = d.get("kind")
            part = "follow" if kind == "follow" else "start" if kind == "start" else "pos" if "f" in d else "other"
            by_game.setdefault((d.get("game", ""), part), []).append(line)
        for key, lines in by_game.items():
            if key in seen:
                continue
            seen.add(key)
            kept.extend(lines)
    return files, kept


def inputs(src, out):
    files = sorted(glob.glob(os.path.join(src, "inputs-*.jsonl.gz")), reverse=True)
    seen = set()
    kept = []
    for path in files:
        for line in read(path):
            try:
                g = json.loads(line).get("game", "")
            except json.JSONDecodeError:
                continue
            if g and g not in seen:
                seen.add(g)
                kept.append(line)
    return files, kept


def lines(src, pattern, out):
    files = sorted(glob.glob(os.path.join(src, pattern)))
    kept = [l for f in files for l in read(f)]
    return files, kept


def main():
    kind = sys.argv[1]
    if kind == "replays":
        files, kept = replays(sys.argv[2], sys.argv[3])
        out = sys.argv[3]
    elif kind == "inputs":
        files, kept = inputs(sys.argv[2], sys.argv[3])
        out = sys.argv[3]
    elif kind == "lines":
        files, kept = lines(sys.argv[2], sys.argv[3], sys.argv[4])
        out = sys.argv[4]
    else:
        sys.exit(__doc__)
    if not files:
        print("nothing to pack")
        return
    tmp = out + ".tmp"
    with gzip.open(tmp, "wt", encoding="utf-8") as fh:
        for l in kept:
            fh.write(l + "\n")
    os.replace(tmp, out)
    if os.environ.get("COMPACT_LIST"):
        with open(os.environ["COMPACT_LIST"], "w") as fh:
            for f in files:
                fh.write(os.path.basename(f) + "\n")
    print(f"{len(files)} files packed into {os.path.basename(out)}: {len(kept)} lines")


if __name__ == "__main__":
    main()
