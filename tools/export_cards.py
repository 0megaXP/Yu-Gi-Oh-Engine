#!/usr/bin/env python3
"""Export the card database the engine needs into one tab-separated file.

usage: export_cards.py <BabelCDB dir> <passcode_map.tsv> <out cards.tsv>

Columns: id alias setcode type atk def level race attribute category name
Earlier sources win, so official data beats release/pre-release/unofficial copies.
"""
import glob
import os
import sqlite3
import sys

src, pmap, out = sys.argv[1:4]
order = ["cards.cdb"]
order += sorted(os.path.basename(p) for p in glob.glob(os.path.join(src, "release-*.cdb")))
order += sorted(os.path.basename(p) for p in glob.glob(os.path.join(src, "prerelease-*.cdb"))
                if "rush" not in p)
order += ["cards-unofficial.cdb"]

rows = {}
for name in order:
    path = os.path.join(src, name)
    if not os.path.exists(path):
        continue
    con = sqlite3.connect(path)
    q = ("select d.id,d.alias,d.setcode,d.type,d.atk,d.def,d.level,d.race,d.attribute,"
         "d.category,t.name from datas d join texts t on d.id=t.id")
    for r in con.execute(q):
        rows.setdefault(r[0], r)
    con.close()

missing = []
for line in open(pmap, encoding="utf-8"):
    line = line.strip()
    if not line or line.startswith("#"):
        continue
    official, script_id = (int(x) for x in line.split("\t")[:2])
    if script_id not in rows:
        missing.append(script_id)

os.makedirs(os.path.dirname(out), exist_ok=True)
with open(out, "w", encoding="utf-8") as f:
    for cid in sorted(rows):
        r = list(rows[cid])
        r[10] = " ".join((r[10] or "").split())
        f.write("\t".join(str(x if x is not None else 0) for x in r) + "\n")
print(f"cards.tsv: {len(rows)} cards from {len(order)} databases")

# Effect descriptions (texts.str1..str16) so decisions can be labelled in plain words.
strings = {}
for name in order:
    path = os.path.join(src, name)
    if not os.path.exists(path):
        continue
    con = sqlite3.connect(path)
    cols = ",".join(f"str{i}" for i in range(1, 17))
    for r in con.execute(f"select id,desc,{cols} from texts"):
        if r[0] in strings:
            continue
        strings[r[0]] = r[1:]
    con.close()
with open(os.path.join(os.path.dirname(out), "strings.tsv"), "w", encoding="utf-8") as f:
    for cid in sorted(strings):
        # index 0 is the card text, 1..16 are its effect descriptions (str1..str16)
        for i, s in enumerate(strings[cid]):
            s = " ".join((s or "").split())
            if s:
                f.write(f"{cid}\t{i}\t{s}\n")
if missing:
    print("warning: passcode_map points at ids not in the database:", missing)
