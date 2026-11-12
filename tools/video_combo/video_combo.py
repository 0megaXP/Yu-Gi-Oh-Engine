#!/usr/bin/env python3
r"""Turn a Yu-Gi-Oh! combo video into a combo the engine can check.

  extract  a YouTube link or a local video -> combo.json, combo.md and a state file
  compare  the combo's end board vs. the board the engine found from the same hand
  harvest  search YouTube for an archetype's combo videos, extract each, build its library
  library  merge extracted combos into data/combos/<archetype>.json (harvest does this too)

Examples:
  python3 tools/video_combo/video_combo.py extract "https://youtu.be/..." \
      --deck decks/angelechy_luna_mardelplata_2026-11.ydk --out combos/angelechy_otk
  build/ygoe-cli analyze combos/angelechy_otk/state.txt --json --out combos/angelechy_otk/engine.json
  python3 tools/video_combo/video_combo.py compare combos/angelechy_otk
  python3 tools/video_combo/video_combo.py harvest Angelechy \
      --deck decks/angelechy_luna_mardelplata_2026-11.ydk --count 5 --since 2026-10-01
"""
import argparse
import glob
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from cards import ROOT, CardDB, deck_names, load_ydk  # noqa: E402
import frames as fr  # noqa: E402

# Seen in most combo videos as "what if they have X" branches.
HAND_TRAPS = [
    "Ash Blossom & Joyous Spring", "Maxx \"C\"", "Effect Veiler", "Infinite Impermanence",
    "Ghost Ogre & Snow Rabbit", "Ghost Belle & Haunted Mansion", "Droll & Lock Bird", "Nibiru, the Primal Being",
    "Called by the Grave", "Crossout Designator", "Mulcharmy Fuwalos", "Mulcharmy Purulia", "Mulcharmy Meowls",
    "Dimension Shifter", "Ghost Mourner & Moonlit Chill", "Triple Tactics Talent", "Forbidden Droplet",
]


def _rel(path, start):
    return os.path.relpath(os.path.abspath(path), os.path.abspath(start))


def cmd_extract(a):
    db = CardDB()
    out = os.path.abspath(a.out)
    work = os.path.join(out, "work")
    os.makedirs(work, exist_ok=True)

    prefer, names = [], []
    for path in filter(None, [a.deck, a.opponent]):
        deck = load_ydk(path)
        prefer += deck["main"] + deck["extra"] + deck["side"]
        names += deck_names(db, deck)
    for n in HAND_TRAPS:
        code, name, conf = db.resolve(n)
        if code and name not in names:
            names.append(name)
            prefer.append(code)

    # 1. video
    info, captions = {}, ""
    if a.frames_from:
        frames = fr.from_dir(a.frames_from)
        info_path = os.path.join(os.path.dirname(os.path.abspath(a.frames_from)), "info.json")
        if os.path.exists(info_path):
            info = json.load(open(info_path, encoding="utf-8"))
    else:
        if os.path.exists(a.video):
            video = a.video
            info = {"title": os.path.basename(video)}
        else:
            print(f"Downloading {a.video}")
            video, info, captions = fr.download(a.video, work)
            print(f"  {info.get('title')} ({info.get('duration')} s)" + ("  + captions" if captions else ""))
        if a.captions:
            captions = fr.parse_vtt(open(a.captions, encoding="utf-8").read())
        print("Cutting frames")
        frames = fr.extract(video, work, fps=a.fps, threshold=a.threshold, max_frames=a.max_frames,
                            start=a.start, end=a.end)
    if captions:
        open(os.path.join(work, "captions.txt"), "w", encoding="utf-8").write(captions)
    print(f"  {len(frames)} frames kept in {_rel(os.path.join(work, 'frames'), os.getcwd())}")
    if a.frames_only:
        print("Stopped after frames (--frames-only).")
        return

    # 2. read frames, 3. write the combo
    import vision
    print(f"Reading frames with {a.model}")
    batches = vision.read_frames(frames, names, info, captions, work, model=a.model, batch=a.batch)
    print("Writing the combo")
    combo = vision.write_combo(batches, names, info, captions, model=a.model)

    # 4. names -> passcodes
    unresolved = []

    def card(text):
        code, name, conf = db.resolve(text, prefer)
        if code is None:
            unresolved.append(text)
        return {"code": code, "name": name, "read_as": text if name != text else None, "match": conf}

    result = {
        "source": {k: info.get(k) for k in ("webpage_url", "title", "uploader", "upload_date")},
        "deck_file": _rel(a.deck, ROOT) if a.deck else None,
        "deck": combo.get("deck"),
        "going": combo.get("going", "unknown"),
        "starting_hand": [card(c) for c in combo.get("starting_hand", [])],
        "steps": [{**s, **{"code": card(s.get("card", ""))["code"] if s.get("card") else None}}
                  for s in combo.get("steps", [])],
        "end_board": [{**card(c["card"]), "where": c.get("where", ""), "note": c.get("note", "")}
                      for c in combo.get("end_board", [])],
        "interruptions": combo.get("interruptions", []),
        "choke_points": combo.get("choke_points", []),
        "uncertain": combo.get("uncertain", []),
        "unresolved_names": sorted(set(unresolved)),
        "model": a.model,
    }
    for c in result["starting_hand"] + result["end_board"]:
        if not c["read_as"]:
            c.pop("read_as")
    json.dump(result, open(os.path.join(out, "combo.json"), "w", encoding="utf-8"), indent=1, ensure_ascii=False)
    open(os.path.join(out, "combo.md"), "w", encoding="utf-8").write(markdown(result))
    if a.deck:
        open(os.path.join(out, "state.txt"), "w", encoding="utf-8").write(state_file(result, a, out))

    print(f"\nDone: {_rel(out, os.getcwd())}/combo.json, combo.md" + (", state.txt" if a.deck else ""))
    hand = [c["name"] for c in result["starting_hand"]]
    print(f"  hand: {'; '.join(hand) or '(not found)'}")
    print(f"  {len(result['steps'])} steps, end board of {len(result['end_board'])} cards")
    if result["unresolved_names"]:
        print(f"  could not match: {'; '.join(result['unresolved_names'])}")
    return result


def markdown(r):
    src = r["source"]
    o = [f"# {src.get('title') or 'Combo'}", ""]
    if src.get("webpage_url"):
        o.append(f"Source: [{src.get('uploader') or 'video'}]({src['webpage_url']})  ")
    o += [f"Deck: {r.get('deck') or '?'} · going {r.get('going')}", "", "## Starting hand", ""]
    o += [f"- {c['name']}" + (f"  _(read as \"{c['read_as']}\")_" if c.get("read_as") else "")
          for c in r["starting_hand"]] or ["- (not shown)"]
    o += ["", "## Steps", ""]
    for i, s in enumerate(r["steps"], 1):
        t = f"`{int(s['t']) // 60}:{int(s['t']) % 60:02d}` " if s.get("t") is not None else ""
        o.append(f"{i}. {t}**{s.get('action', '').replace('_', ' ')}** {s.get('card', '')} — {s.get('detail', '')}")
    o += ["", "## End board", ""]
    o += [f"- {c['name']} ({c['where']}{', ' + c['note'] if c.get('note') else ''})" for c in r["end_board"]]
    for key, title in (("interruptions", "Interruptions"), ("choke_points", "Choke points"),
                       ("uncertain", "Uncertain"), ("unresolved_names", "Names not matched to a card")):
        if r.get(key):
            o += ["", f"## {title}", ""] + [f"- {x}" for x in r[key]]
    return "\n".join(o) + "\n"


def state_file(r, a, out):
    """A docs/state-format.md file with the video's opening hand, so the engine can search the same spot."""
    hand = [c["name"] for c in r["starting_hand"] if c["code"]]
    me = 0 if r.get("going") != "second" else 1
    opp_deck = a.opponent or a.deck
    lines = [
        f"# From: {r['source'].get('title') or 'video'}",
        f"# {r['source'].get('webpage_url') or ''}",
        "# The engine starts from the video's opening hand; compare its end board with",
        "# combo.json using: video_combo.py compare " + _rel(out, ROOT),
    ]
    if not a.opponent:
        lines.append("# No --opponent deck was given, so the opponent plays the same list (goldfish).")
    if me == 1:
        lines.append("# The video goes second: put the opponent's board on side 0 by hand before analysing.")
    lines += [f"deck{me}: {_rel(a.deck, out)}", f"deck{1 - me}: {_rel(opp_deck, out)}", f"me: {me}",
              "mode: practice", f"hand{me}: " + "; ".join(hand), f"hand{1 - me}: proxy 5", "seed: 1"]
    return "\n".join(lines) + "\n"


def cmd_compare(a):
    combo = json.load(open(a.combo if a.combo.endswith(".json") else os.path.join(a.combo, "combo.json"),
                           encoding="utf-8"))
    engine_path = a.engine or os.path.join(os.path.dirname(os.path.abspath(
        a.combo if a.combo.endswith(".json") else os.path.join(a.combo, "combo.json"))), "engine.json")
    engine = json.load(open(engine_path, encoding="utf-8"))
    if "error" in engine:
        raise SystemExit("engine: " + engine["error"])
    def on_field(where):
        w = (where or "").lower()
        return not any(x in w for x in ("hand", "gy", "grave", "banish", "deck"))

    # field cards only: hands are compared separately, and the GY rarely shows fully on video
    want = [c for c in combo["end_board"] if c.get("code") and on_field(c.get("where"))]
    want_codes = [c["code"] for c in want]
    print(f"Video end board ({len(want)} cards): " + "; ".join(c["name"] for c in want))
    best = None
    for i, line in enumerate(engine.get("lines", []), 1):
        got = [c["code"] for c in line.get("end_board", []) if on_field(c.get("where"))]
        left = list(got)
        hit = []
        for c in want_codes:
            if c in left:
                left.remove(c)
                hit.append(c)
        miss = [c for c in want_codes if c not in hit]
        share = len(hit) / len(want_codes) if want_codes else 0
        print(f"\nEngine line #{i}  (score {line.get('combined', 0):.2f}): {share:.0%} of the video's board")
        names = {c["code"]: c["name"] for c in line.get("end_board", [])}
        if miss:
            print("  missing: " + "; ".join(next(w["name"] for w in want if w["code"] == c) for c in miss))
        if left:
            print("  extra:   " + "; ".join(names.get(c, str(c)) for c in left))
        if best is None or share > best[0]:
            best = (share, i)
    if best:
        verdict = "found the video's board" if best[0] == 1 else "did not fully reach the video's board"
        print(f"\nBest match: line #{best[1]} ({best[0]:.0%}). The engine {verdict}.")
        if best[0] < 1:
            print("Try more --iterations, or check combo.md for steps the video may have mislabelled.")


def _slug(text):
    return re.sub(r"[^a-z0-9]+", "_", text.lower()).strip("_") or "archetype"


def cmd_harvest(a):
    import harvest
    slug = _slug(a.archetype)
    root = os.path.abspath(a.out or os.path.join(ROOT, "combos", slug))
    print(f"Searching YouTube for {a.archetype} combo videos" + (f" since {a.since}" if a.since else ""))
    videos = harvest.search(a.archetype, count=a.count, since=a.since, query=a.query)
    if not videos:
        raise SystemExit("No matching videos found. Try --query, an older --since, or a larger --count.")
    for v in videos:
        print(f"  {v['upload_date'] or '?'}  {v['title']}  ({v['uploader']})")
    if a.list_only:
        return
    parser = build_parser()
    for i, v in enumerate(videos, 1):
        folder = os.path.join(root, v["id"])
        if os.path.exists(os.path.join(folder, "combo.json")) and not a.redo:
            print(f"\n[{i}/{len(videos)}] already extracted: {v['title']}")
            continue
        print(f"\n[{i}/{len(videos)}] {v['title']}")
        args = ["extract", v["url"], "--out", folder, "--model", a.model]
        if a.deck:
            args += ["--deck", a.deck]
        if a.opponent:
            args += ["--opponent", a.opponent]
        try:
            cmd_extract(parser.parse_args(args))
        except (Exception, SystemExit) as err:  # one bad video must not stop the batch
            print(f"  failed: {str(err).splitlines()[0][:120] if str(err) else type(err).__name__}")
        finally:
            if not a.keep_video:  # frames stay for re-runs; the video itself is large
                for f in glob.glob(os.path.join(folder, "work", "video.*")):
                    if not f.endswith((".vtt", ".json")):
                        os.remove(f)
    a.combos = root
    a.name = a.archetype
    cmd_library(a)


def cmd_library(a):
    import harvest
    db = CardDB()
    slug = _slug(a.name)
    out = a.library or os.path.join(ROOT, "data", "combos", slug + ".json")
    lib = harvest.build_library(a.combos, a.name, db, deck_file=_rel(a.deck, ROOT) if a.deck else None)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    json.dump(lib, open(out, "w", encoding="utf-8"), indent=1, ensure_ascii=False)
    print(f"\nLibrary: {_rel(out, os.getcwd())}  ({lib['videos']} videos, {len(lib['lines'])} distinct lines)")
    for i, line in enumerate(lib["lines"][:5], 1):
        board = "; ".join(c["name"] for c in line["end_board_core"]) or "?"
        print(f"  #{i} seen in {line['support']} video(s), {len(line['steps'])} steps -> {board}")
    if lib["starters"]:
        print("  starters: " + "; ".join(f"{c['name']} ({c['share']:.0%})" for c in lib["starters"][:5]))


def build_parser():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    e = sub.add_parser("extract", help="video -> combo.json / combo.md / state.txt")
    e.add_argument("video", nargs="?", help="YouTube (or any yt-dlp) link, or a local video file")
    e.add_argument("--out", required=True, help="output folder, e.g. combos/<name>")
    e.add_argument("--deck", help=".ydk of the deck in the video (strongly recommended)")
    e.add_argument("--opponent", help=".ydk of the opponent, if the video shows one")
    e.add_argument("--model", default=os.environ.get("VIDEO_COMBO_MODEL", "claude-sonnet-5-5"))
    e.add_argument("--fps", type=float, default=1.0, help="frames sampled per second (default 1)")
    e.add_argument("--threshold", type=int, default=10, help="how different a frame must be to keep it (0-256)")
    e.add_argument("--max-frames", type=int, default=400)
    e.add_argument("--batch", type=int, default=6, help="frames per model call")
    e.add_argument("--start", type=float, help="skip to this second (intros, deck profiles)")
    e.add_argument("--end", type=float, help="stop at this second")
    e.add_argument("--captions", help="a .vtt file to use instead of YouTube's")
    e.add_argument("--frames-only", action="store_true", help="cut frames and stop (no API calls)")
    e.add_argument("--frames-from", help="reuse a frames folder from an earlier run")
    e.set_defaults(func=cmd_extract)

    c = sub.add_parser("compare", help="combo.json end board vs. engine analyze --json output")
    c.add_argument("combo", help="combo folder or combo.json")
    c.add_argument("--engine", help="engine JSON (default: engine.json next to combo.json)")
    c.set_defaults(func=cmd_compare)

    h = sub.add_parser("harvest", help="find an archetype's combo videos, extract them, build the library")
    h.add_argument("archetype", help='e.g. "Angelechy"; video titles must contain it')
    h.add_argument("--deck", help=".ydk of a current list for the archetype (strongly recommended)")
    h.add_argument("--opponent", help=".ydk of an opponent, for state files")
    h.add_argument("--count", type=int, default=5, help="videos to extract (default 5)")
    h.add_argument("--since", help="skip videos uploaded before YYYY-MM-DD (e.g. the last banlist)")
    h.add_argument("--query", help='YouTube search text (default: "<archetype> combo")')
    h.add_argument("--out", help="folder for the per-video results (default combos/<archetype>)")
    h.add_argument("--library", help="library file (default data/combos/<archetype>.json)")
    h.add_argument("--model", default=os.environ.get("VIDEO_COMBO_MODEL", "claude-sonnet-5-5"))
    h.add_argument("--list-only", action="store_true", help="show the videos it would use, then stop")
    h.add_argument("--redo", action="store_true", help="extract again even if a video was done before")
    h.add_argument("--keep-video", action="store_true", help="keep the downloaded videos")
    h.set_defaults(func=cmd_harvest)

    lb = sub.add_parser("library", help="merge combos/<archetype>/*/combo.json into a library file")
    lb.add_argument("combos", help="folder holding one sub-folder per video")
    lb.add_argument("--name", required=True, help="archetype name")
    lb.add_argument("--deck", help=".ydk the combos were read with")
    lb.add_argument("--library", help="output (default data/combos/<name>.json)")
    lb.set_defaults(func=cmd_library)
    return p


def main():
    p = build_parser()
    a = p.parse_args()
    if a.cmd == "extract" and not (a.video or a.frames_from):
        p.error("give a video link/file or --frames-from")
    a.func(a)


if __name__ == "__main__":
    main()
