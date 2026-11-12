"""Card names <-> passcodes, using the same data the engine reads.

data/cards.tsv holds EDOPro ids; data/passcode_map.tsv maps official passcodes to the
pre-release ids EDOPro still uses for new sets. Everything this tool writes uses
official passcodes, the same ids as .ydk files and the engine's JSON output.
"""
import difflib
import os
import re

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# card type bits we care about (ygopro constants)
TYPE_TOKEN = 0x4000


def _norm(name):
    name = name.lower().replace("’", "'").replace("“", '"').replace("”", '"')
    name = re.sub(r"[\"'!?.,:\-–—&()]", " ", name)
    return " ".join(name.split())


class CardDB:
    def __init__(self, data_dir=None):
        data_dir = data_dir or os.path.join(ROOT, "data")
        cards = os.path.join(data_dir, "cards.tsv")
        if not os.path.exists(cards):
            raise SystemExit(f"{cards} not found. Run tools/bootstrap.sh first.")
        self.to_official = {}  # script id -> official passcode
        self.to_script = {}    # official passcode -> script id
        pmap = os.path.join(data_dir, "passcode_map.tsv")
        if os.path.exists(pmap):
            for line in open(pmap, encoding="utf-8"):
                if line.strip() and not line.startswith("#"):
                    official, script = (int(x) for x in line.split("\t")[:2])
                    self.to_official[script] = official
                    self.to_script[official] = script
        self.name_of = {}   # official passcode -> name
        self.by_norm = {}   # normalized name -> official passcode
        for line in open(cards, encoding="utf-8"):
            f = line.rstrip("\n").split("\t")
            if len(f) < 11:
                continue
            cid, alias, ctype, name = int(f[0]), int(f[1]), int(f[3]), f[10]
            if not name or cid < 1000 or ctype & TYPE_TOKEN:
                continue
            code = self.to_official.get(cid, cid)
            self.name_of[code] = name
            # alternate artworks share a name; keep the base card
            key = _norm(name)
            if key not in self.by_norm or (alias == 0 and code < self.by_norm[key]):
                self.by_norm[key] = code
        self._all_keys = list(self.by_norm)

    def name(self, code):
        return self.name_of.get(code) or self.name_of.get(self.to_official.get(code, -1)) or str(code)

    def resolve(self, text, prefer=None):
        """Best card for a name read off a video. Returns (code, name, confidence) or (None, text, 0).

        prefer: passcodes to try first (the decks in play), so OCR noise snaps to a card
        that is actually in the list instead of a lookalike from the whole pool.
        """
        if not text:
            return None, text, 0.0
        text = str(text).strip()
        if text.isdigit() and int(text) in self.name_of:
            code = int(text)
            return code, self.name_of[code], 1.0
        key = _norm(text)
        if not key:
            return None, text, 0.0
        pools = []
        if prefer:
            pools.append({_norm(self.name(c)): c for c in prefer})
        pools.append(self.by_norm)
        for i, pool in enumerate(pools):
            if key in pool:
                return pool[key], self.name(pool[key]), 1.0
            # unique prefix ("Angelechy Op" -> the one card that starts like that)
            starts = [k for k in pool if k.startswith(key)]
            if len(starts) == 1 and len(key) >= 6:
                return pool[starts[0]], self.name(pool[starts[0]]), 0.9
            if len(starts) > 1:
                break  # a cut-off name that fits several cards: better unmatched than wrong
            cutoff = 0.8 if i == 0 and prefer else 0.86
            hit = difflib.get_close_matches(key, list(pool), n=1, cutoff=cutoff)
            if hit:
                score = difflib.SequenceMatcher(None, key, hit[0]).ratio()
                return pool[hit[0]], self.name(pool[hit[0]]), round(score, 2)
        return None, text, 0.0


def load_ydk(path):
    """Passcodes from a .ydk: {'main': [...], 'extra': [...], 'side': [...]}."""
    out = {"main": [], "extra": [], "side": []}
    section = "main"
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if line == "#main":
            section = "main"
        elif line == "#extra":
            section = "extra"
        elif line == "!side":
            section = "side"
        elif line.isdigit():
            out[section].append(int(line))
    return out


def deck_names(db, deck):
    """Unique card names in a deck, Main then Extra then Side."""
    seen, names = set(), []
    for part in ("main", "extra", "side"):
        for code in deck[part]:
            if code not in seen:
                seen.add(code)
                names.append(db.name(code))
    return names
