#!/usr/bin/env python3
"""Build the engine's asset pack: everything the engine reads at run time, in 6 files.

usage: make_pack.py <CardScripts dir> <data dir> <out dir>

out/scripts.pack   all official, pre-release and core Lua scripts in one indexed file
out/cards.tsv, effects.tsv, strings.tsv, passcode_map.tsv
out/VERSION        content hash; the app re-installs the pack when it changes

scripts.pack layout (little endian):
  "YGOEPK1\\n", u32 count, count x (u16 name_len, name, u64 offset, u32 size), then the data.
Names are paths relative to CardScripts ("utility.lua", "official/c14558127.lua").
"""
import hashlib
import os
import shutil
import struct
import sys

scripts, data, out = sys.argv[1:4]
os.makedirs(out, exist_ok=True)

files = []
for name in sorted(os.listdir(scripts)):
    if name.endswith(".lua"):
        files.append(name)
for sub in ("official", "pre-release"):
    for name in sorted(os.listdir(os.path.join(scripts, sub))):
        if name.endswith(".lua"):
            files.append(f"{sub}/{name}")

blobs = [open(os.path.join(scripts, f), "rb").read() for f in files]
names = [f.encode() for f in files]
header = 8 + 4 + sum(2 + len(n) + 8 + 4 for n in names)
digest = hashlib.sha256()
with open(os.path.join(out, "scripts.pack"), "wb") as f:
    f.write(b"YGOEPK1\n")
    f.write(struct.pack("<I", len(files)))
    offset = header
    for n, b in zip(names, blobs):
        f.write(struct.pack("<H", len(n)))
        f.write(n)
        f.write(struct.pack("<QI", offset, len(b)))
        offset += len(b)
    for b in blobs:
        f.write(b)
        digest.update(b)

for name in ("cards.tsv", "effects.tsv", "strings.tsv", "passcode_map.tsv", "value_net.json", "policy.json",
             "nonengine_timing.json", "nonengine_values.json", "deck_library.json"):
    src = os.path.join(data, name)
    if name.endswith(".json") and not os.path.exists(src):
        continue  # what was learned (tools/ml) is optional
    shutil.copyfile(src, os.path.join(out, name))
    digest.update(open(src, "rb").read())

version = digest.hexdigest()[:16]
open(os.path.join(out, "VERSION"), "w").write(version + "\n")
size = sum(os.path.getsize(os.path.join(out, n)) for n in os.listdir(out))
print(f"pack {version}: {len(files)} scripts, {size / 1e6:.1f} MB in {out}")
