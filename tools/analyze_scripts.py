#!/usr/bin/env python3
"""Static pass over the EDOPro Lua card scripts.

For every effect a card registers it records how and from where it is used and
what it does, so the engine can (a) tell which of a player's cards are
interruptions on the opponent's turn and (b) profile what an opponent's deck
depends on (graveyard, hand, field monster effects, spells/traps, ...).

usage: analyze_scripts.py <cards.tsv> <CardScripts dir> <out effects.tsv>
Columns: id  effect_index  type  code  range  category  property  count_limit
         reasons  not_reasons  prev  by  now
(type/code/range/category/property are the OR of the constants named in the script)

The last four describe a trigger's condition (engine/src/trigger.h), read from its condition
function and the filters it calls: the REASON_* it checks for (reasons) or rules out
(not_reasons), the LOCATION_* the card had to come from (IsPreviousLocation), and whose action
it must be (by: 0 anyone, 1 the card's controller, 2 the opponent: "rp==1-tp"), and the
LOCATION_* it has to be in now (now: IsLocation, e.g. "placed in the Spell & Trap Zone").
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
condition = re.compile(r"(\w+):SetCondition\((s\.\w+)")
func_start = re.compile(r"^function\s+s\.(\w+)\s*\(")
reason_tok = re.compile(r"REASON_[A-Z_]+")
negated_reason = re.compile(r"not\s+[\w\.:\(\)]*?:IsReason\(([^)]*)\)")
prev_call = re.compile(r"(not\s+[\w\.:\(\)]*?)?:IsPreviousLocation\(([^)]*)\)")
prev_get = re.compile(r"GetPreviousLocation\(\)\s*[&=~]+\s*\(?([A-Z_|+ ]+)")
now_call = re.compile(r"(not\s+[\w\.:\(\)]*?)?:IsLocation\(([^)]*)\)")
loc_tok = re.compile(r"LOCATION_[A-Z_]+")
by_opp = re.compile(r"\brp\s*==\s*1\s*-\s*tp|\brp\s*~=\s*tp|1\s*-\s*tp\s*==\s*rp|\btp\s*~=\s*rp|GetReasonPlayer\(\)\s*(?:==\s*1\s*-\s*tp|~=\s*tp)")
by_own = re.compile(r"\brp\s*==\s*tp\b|\btp\s*==\s*rp\b|GetReasonPlayer\(\)\s*==\s*tp\b")


def functions(src):
    """s.name -> body text of the script's top-level functions."""
    out, name, body = {}, None, []
    for line in src.splitlines():
        m = func_start.match(line)
        if m:
            if name:
                out[name] = "\n".join(body)
            name, body = m.group(1), []
            continue
        if name:
            body.append(line.split("--")[0])
    if name:
        out[name] = "\n".join(body)
    return out


def const_or(names):
    v = 0
    for n in names:
        v |= const.get(n, 0)
    return v


def condition_info(fname, funcs):
    """(reasons, not_reasons, prev, by) of a condition function and the filters it calls."""
    body = funcs.get(fname, "")
    for h in set(re.findall(r"s\.(\w+)", body)):
        if h != fname and h in funcs:
            body += "\n" + funcs[h]
    neg = set()
    for m in negated_reason.finditer(body):
        neg.update(reason_tok.findall(m.group(1)))
    pos = set(reason_tok.findall(body)) - neg
    prev = set()
    for m in prev_call.finditer(body):
        if not m.group(1):
            prev.update(loc_tok.findall(m.group(2)))
    for m in prev_get.finditer(body):
        prev.update(loc_tok.findall(m.group(1)))
    now = set()
    for m in now_call.finditer(body):
        if not m.group(1):
            now.update(loc_tok.findall(m.group(2)))
    by = 2 if by_opp.search(body) else 1 if by_own.search(body) else 0
    return const_or(pos), const_or(neg), const_or(prev), by, const_or(now)


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
        funcs = functions(src)
        for line in src.splitlines():
            line = line.split("--")[0]
            m = create.search(line)
            if m:
                effects[m.group(1)] = {"type": 0, "code": 0, "range": 0, "category": 0,
                                       "property": 0, "count": 0}
                continue
            for m in condition.finditer(line):
                var, fn = m.groups()
                if var in effects:
                    effects[var]["cond"] = fn[2:]
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
            reasons, not_reasons, prev, by, now = condition_info(e["cond"], funcs) if e.get("cond") else (0, 0, 0, 0, 0)
            f.write(f"{cid}\t{i}\t{e['type']}\t{e['code']}\t{e['range']}\t{e['category']}\t{e['property']}\t{e['count']}"
                    f"\t{reasons}\t{not_reasons}\t{prev}\t{by}\t{now}\n")
            written += 1
print(f"effects.tsv: {written} effects")
