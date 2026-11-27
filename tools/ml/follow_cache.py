#!/usr/bin/env python3
"""The follow job's cache: each game's follow results kept from earlier runs, so only the games
whose upload or the follower changed are followed again (following everything again each run grew
with every game uploaded).

usage: follow_cache.py split <inputs.jsonl> <cache.jsonl.gz> <key> <todo.jsonl> <followed dir> <runner>
       follow_cache.py update <followed dir> <cache.jsonl.gz>

split: writes the games to follow to todo.jsonl (not in the cache, or uploaded again, or followed
  by another follower: `key`, a hash of the engine code following depends on); runner 1 writes the
  cached games' lines to <followed dir>/cache-pos.jsonl, so they count like games followed tonight.
  Also <followed dir>/follow-key.json: the key and each game's upload hash (for update).
update (train job, all runners' results in one folder): the new cache, every game in
  follow-key.json with its lines (followed tonight or from the cache).
"""
import collections
import glob
import gzip
import hashlib
import json
import os
import sys


def h(line):
    return hashlib.sha1(line.strip().encode()).hexdigest()[:16]


def load_cache(path):
    cache = {}
    if os.path.exists(path):
        try:
            with gzip.open(path, "rt", encoding="utf-8") as fh:
                for line in fh:
                    try:
                        e = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    cache[e["game"]] = e
        except (OSError, EOFError):
            pass
    return cache


def split(inputs, cache_path, key, todo, followed, runner):
    cache = load_cache(cache_path)
    hashes = {}
    hit = miss = 0
    os.makedirs(followed, exist_ok=True)
    with open(inputs, encoding="utf-8") as fi, open(todo, "w", encoding="utf-8") as ft:
        cached_out = open(os.path.join(followed, "cache-pos.jsonl"), "w", encoding="utf-8") if runner == "1" else None
        for line in fi:
            try:
                g = json.loads(line).get("game", "")
            except json.JSONDecodeError:
                continue
            if not g:
                continue
            hashes[g] = h(line)
            e = cache.get(g)
            if e and e.get("h") == hashes[g] and e.get("key") == key:
                hit += 1
                if cached_out:
                    for l in e["lines"]:
                        cached_out.write(l + "\n")
            else:
                miss += 1
                ft.write(line if line.endswith("\n") else line + "\n")
        if cached_out:
            cached_out.close()
    if runner == "1":
        json.dump({"key": key, "games": hashes}, open(os.path.join(followed, "follow-key.json"), "w"))
    print(f"follow cache: {hit} games from earlier runs, {miss} to follow")


def update(followed, cache_out):
    meta_path = os.path.join(followed, "follow-key.json")
    if not os.path.exists(meta_path):
        print("no follow-key.json: cache left as it was")
        return
    meta = json.load(open(meta_path))
    lines = collections.defaultdict(list)
    for path in sorted(glob.glob(os.path.join(followed, "**", "*.jsonl"), recursive=True)):
        with open(path, encoding="utf-8") as fh:
            for line in fh:
                line = line.rstrip("\n")
                if not line:
                    continue
                try:
                    g = json.loads(line).get("game", "")
                except json.JSONDecodeError:
                    continue
                if g:
                    lines[g].append(line)
    n = 0
    tmp = cache_out + ".tmp"
    with gzip.open(tmp, "wt", encoding="utf-8") as fh:
        for g, hh in meta["games"].items():
            if g in lines:  # (a game a runner didn't finish: followed again next time)
                fh.write(json.dumps({"game": g, "h": hh, "key": meta["key"], "lines": lines[g]}, separators=(",", ":")) + "\n")
                n += 1
    os.replace(tmp, cache_out)
    print(f"follow cache: {n} games kept")


if __name__ == "__main__":
    if sys.argv[1] == "split":
        split(*sys.argv[2:8])
    elif sys.argv[1] == "update":
        update(*sys.argv[2:4])
    else:
        sys.exit(__doc__)
