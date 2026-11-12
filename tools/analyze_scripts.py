#!/usr/bin/env python3
"""Static pass over the EDOPro Lua card scripts.

For every effect a card registers it records how and from where it is used and
what it does, so the engine can (a) tell which of a player's cards are
interruptions on the opponent's turn and (b) profile what an opponent's deck
depends on (graveyard, hand, field monster effects, spells/traps, ...).

usage: analyze_scripts.py <cards.tsv> <CardScripts dir> <out effects.tsv>
Columns: id  effect_index  type  code  range  category  property  count_limit
(type/code/range/category/property are the OR of the constants named in the script)
"""
import os
import re
import sys

cards_tsv, scripts_dir, out = sys.argv[1:4]

# --- read constants.lua so names resolve to the same numbers the core uses ---------
const = {}
num = re.compile(r"^\s*([A-Z_][A-Z0-9_]*)\s*=\s*(0x[0-9a-fA-F]+|\d+)\s*(?:--.*)?$")
expr = re.compile(r"^\s*([A-Z_][A-Z0-9_]*)\s*=\s*([A-Z0-9_|+\s]+?)\s*(?:--.*)?$")
pending = []
for line in open(os.path.join(scripts_dir, "constant.lua"), encoding="utf-8", errors="replace"):
    m = num.match(line)
    if m:
        const[m.group(1)] = int(m.group(2), 0)
        continue
    m = expr.match(line)
    if m:
        pending.append((m.group(1), m.group(2)))


def evaluate(text):
    total = 0
    for part in re.split(r"[|+]", text):
        part = part.strip()
        if not part:
            continue
        if re.fullmatch(r"0x[0-9a-fA-F]+|\d+", part):
            total |= int(part, 0)
        elif part in const:
            total |= const[part]
        else:
            return None
    return total


for _ in range(5):
    for name, text in pending:
        if name not in const:
            v = evaluate(text)
            if v is not None:
                const[name] = v

ids = []
for line in open(cards_tsv, encoding="utf-8"):
    ids.append(int(line.split("\t", 1)[0]))

SETTERS = {"SetType": "type", "SetCode": "code", "SetRange": "range",
           "SetCategory": "category", "SetProperty": "property"}
create = re.compile(r"local\s+(\w+)\s*=\s*Effect\.CreateEffect\(")
clone = re.compile(r"local\s+(\w+)\s*=\s*(\w+):Clone\(\)")
setter = re.compile(r"(\w+):(SetType|SetCode|SetRange|SetCategory|SetProperty)\(([^()]*(?:\([^()]*\))?[^()]*)\)")
countlim = re.compile(r"(\w+):SetCountLimit\(([^)]*)\)")
register = re.compile(r"(?:\w+):RegisterEffect\((\w+)")


def value_of(arg):
    arg = arg.split(",")[0]
    v = evaluate(arg.replace("(", "").replace(")", ""))
    return v or 0


def find_script(cid):
    for sub in ("official", "pre-release", "unofficial"):
        p = os.path.join(scripts_dir, sub, f"c{cid}.lua")
        if os.path.exists(p):
            return p
    return None


written = 0
with open(out, "w", encoding="utf-8") as f:
    for cid in ids:
        path = find_script(cid)
        if not path:
            continue
        src = open(path, encoding="utf-8", errors="replace").read()
        effects = {}
        events = []
        for line in src.splitlines():
            line = line.split("--")[0]
            m = create.search(line)
            if m:
                effects[m.group(1)] = {"type": 0, "code": 0, "range": 0, "category": 0,
                                       "property": 0, "count": 0}
                continue
            m = clone.search(line)
            if m and m.group(2) in effects:
                effects[m.group(1)] = dict(effects[m.group(2)])
                continue
            for m in setter.finditer(line):
                var, fn, arg = m.groups()
                if var in effects:
                    effects[var][SETTERS[fn]] = value_of(arg)
            for m in countlim.finditer(line):
                var, arg = m.groups()
                if var in effects:
                    try:
                        effects[var]["count"] = int(arg.split(",")[0].strip())
                    except ValueError:
                        effects[var]["count"] = 1
            for m in register.finditer(line):
                var = m.group(1)
                if var in effects:
                    events.append(dict(effects[var]))
        for i, e in enumerate(events):
            f.write(f"{cid}\t{i}\t{e['type']}\t{e['code']}\t{e['range']}\t{e['category']}\t{e['property']}\t{e['count']}\n")
            written += 1
print(f"effects.tsv: {written} effects")
