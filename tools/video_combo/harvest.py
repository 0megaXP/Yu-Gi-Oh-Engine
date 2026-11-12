"""Find an archetype's combo videos, and merge extracted combos into one library file.

search()        YouTube search via yt-dlp, filtered to recent, real combo videos
build_library() combos/<archetype>/*/combo.json -> data/combos/<archetype>.json
"""
import datetime
import glob
import json
import os
import re

# Titles that are almost never a combo walkthrough.
SKIP_WORDS = ["pack opening", "opening packs", "unboxing", "tier list", "banlist", "ban list", "reaction",
              "lore", "explained in", "anime", "price", "market watch", "podcast", "live stream", "livestream"]
GOOD_WORDS = ["combo", "combos", "line", "lines", "endboard", "end board", "tutorial", "guide", "how to play"]

LIBRARY_VERSION = 1


def _score(title, archetype):
    t = title.lower()
    if any(w in t for w in SKIP_WORDS):
        return -1
    words = [w for w in re.split(r"\W+", archetype.lower()) if len(w) > 2]
    if words and not all(w in t for w in words):
        return -1
    return 1 + sum(w in t for w in GOOD_WORDS)


def search(archetype, count=5, since=None, query=None, min_seconds=60, max_seconds=1800, log=print):
    """Recent combo videos for an archetype: [{id, url, title, uploader, upload_date, duration}].

    since: 'YYYY-MM-DD'; older videos are skipped (lines from before a banlist or a new
    support set are usually wrong now).
    """
    import yt_dlp

    q = query or f"{archetype} combo"
    since_d = since.replace("-", "") if since else None
    flat = {"quiet": True, "no_warnings": True, "extract_flat": True, "noplaylist": True}
    with yt_dlp.YoutubeDL(flat) as ydl:
        res = ydl.extract_info(f"ytsearch{max(count * 6, 20)}:{q}", download=False)
    cands = []
    for e in res.get("entries") or []:
        title = e.get("title") or ""
        s = _score(title, archetype)
        dur = e.get("duration")
        if s < 0 or (dur and not (min_seconds <= dur <= max_seconds)):
            continue
        cands.append((s, e))
    # best titles first, search rank breaks ties
    cands.sort(key=lambda x: -x[0])

    picked = []
    full = {"quiet": True, "no_warnings": True, "noplaylist": True, "skip_download": True}
    with yt_dlp.YoutubeDL(full) as ydl:
        for s, e in cands:
            if len(picked) >= count:
                break
            url = e.get("url") or f"https://www.youtube.com/watch?v={e['id']}"
            if not url.startswith("http"):
                url = f"https://www.youtube.com/watch?v={e['id']}"
            date = e.get("upload_date")
            dur = e.get("duration")
            if not date or not dur:  # flat search often leaves these out
                try:
                    info = ydl.extract_info(url, download=False)
                except Exception as err:  # private, region-locked, age-gated...
                    log(f"  skip {url}: {str(err).splitlines()[0][:80]}")
                    continue
                date, dur = info.get("upload_date"), info.get("duration")
            if since_d and date and date < since_d:
                continue
            if dur and not (min_seconds <= dur <= max_seconds):
                continue
            picked.append({"id": e["id"], "url": url, "title": e.get("title"), "uploader": e.get("uploader")
                           or e.get("channel"), "upload_date": date, "duration": dur})
    return picked


def _field(where):
    w = (where or "").lower()
    return not any(x in w for x in ("hand", "gy", "grave", "banish", "deck"))


def _jaccard(a, b):
    a, b = set(a), set(b)
    return len(a & b) / len(a | b) if a | b else 0.0


def build_library(combos_dir, archetype, db, deck_file=None, min_board_overlap=0.6):
    """Merge every combo.json under combos_dir into one archetype library.

    Videos whose end boards share most field cards are treated as the same line, so the
    library ranks lines by how many independent videos show them.
    """
    combos = []
    for path in sorted(glob.glob(os.path.join(combos_dir, "*", "combo.json"))):
        c = json.load(open(path, encoding="utf-8"))
        board = [x["code"] for x in c.get("end_board", []) if x.get("code") and _field(x.get("where"))]
        if not board or not c.get("steps"):
            continue  # the reading failed or the video was not a combo
        c["_board"] = board
        c["_dir"] = os.path.basename(os.path.dirname(path))
        combos.append(c)
    if not combos:
        raise SystemExit(f"No usable combo.json under {combos_dir}.")

    # group videos into lines by end board overlap (single link, greedy)
    groups = []
    for c in combos:
        for g in groups:
            if any(_jaccard(c["_board"], o["_board"]) >= min_board_overlap for o in g):
                g.append(c)
                break
        else:
            groups.append([c])

    def card(code):
        return {"code": code, "name": db.name(code)}

    def count(lists):
        tally = {}
        for codes in lists:
            for code in set(codes):
                tally[code] = tally.get(code, 0) + 1
        return tally

    lines = []
    for g in groups:
        # the representative video: the one whose board agrees most with the others
        rep = max(g, key=lambda c: sum(_jaccard(c["_board"], o["_board"]) for o in g))
        board_tally = count(o["_board"] for o in g)
        core = [code for code, n in sorted(board_tally.items(), key=lambda x: -x[1]) if n * 2 >= len(g)]
        starters = count([[h["code"] for h in o.get("starting_hand", []) if h.get("code")] for o in g])
        chokes = []
        for o in g:
            for ch in o.get("choke_points", []):
                if ch and ch not in chokes:
                    chokes.append(ch)
        lines.append({
            "support": len(g),
            "end_board_core": [card(x) for x in core],
            "end_board": [{**card(x["code"]), "where": x.get("where", ""), "note": x.get("note", "")}
                          for x in rep["end_board"] if x.get("code")],
            "hand": [card(h["code"]) for h in rep.get("starting_hand", []) if h.get("code")],
            "hand_cards_seen": [{**card(k), "videos": v} for k, v in sorted(starters.items(), key=lambda x: -x[1])],
            "steps": [{"action": s.get("action"), "code": s.get("code"), "card": s.get("card"),
                       "detail": s.get("detail", ""), "t": s.get("t")} for s in rep.get("steps", [])],
            "interruptions": rep.get("interruptions", []),
            "choke_points": chokes[:12],
            "sources": [{**{k: o["source"].get(k) for k in ("webpage_url", "title", "uploader", "upload_date")},
                         "folder": o["_dir"]} for o in g],
        })
    lines.sort(key=lambda x: (-x["support"], -len(x["steps"])))

    # cards across all videos: what the archetype's lines rely on
    used = count([s["code"] for s in c.get("steps", []) if s.get("code")] for c in combos)
    first = count([[c["steps"][0]["code"]] for c in combos if c["steps"] and c["steps"][0].get("code")])
    boards = count(c["_board"] for c in combos)
    n = len(combos)
    return {
        "version": LIBRARY_VERSION,
        "archetype": archetype,
        "generated": datetime.date.today().isoformat(),
        "deck_file": deck_file,
        "videos": n,
        "key_cards": [{**card(k), "share": round(v / n, 2)} for k, v in sorted(used.items(), key=lambda x: -x[1])][:25],
        "starters": [{**card(k), "share": round(v / n, 2)} for k, v in sorted(first.items(), key=lambda x: -x[1])],
        "end_board_cards": [{**card(k), "share": round(v / n, 2)} for k, v in sorted(boards.items(), key=lambda x: -x[1])],
        "lines": lines,
    }
