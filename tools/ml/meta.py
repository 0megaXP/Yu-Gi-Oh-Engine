#!/usr/bin/env python3
"""The format's decks for learning: picked from the last weeks' tournament top cuts.

usage: meta.py --out-decks format/ --out-lists lists.json --report meta_report.json
               [--weeks 3] [--by-results 3] [--by-usage 5] [--cache pages.json]

Reads YGOPRODeck's "Tournament Meta Decks" (TCG top cuts: deck, tournament, placement, players,
date, the lists) for the last --weeks weeks, and groups them by archetype (YGOPRODeck's deck
name, e.g. "Ars Magna Elfnote").

  results: each top-cut list scores its placement (Winner 4, Runner-Up 3, Top 4 2, Top 8 1,
           lower 0.5), times the event's size (1 + log2(players / 32), at least 0.5), so a win at
           a 400-player Open counts more than at a 30-player regional
  usage:   how many top-cut lists the archetype had

The decks are the --by-results best archetypes by results, then the --by-usage most used of
the others. Each is written as one .ydk: its best scoring list (the most recent on a tie). All
the window's lists go to --out-lists, for the deck library the bots guess opponents from
(decklib.py). With fewer than 3 archetypes found (no network, the site changed) it writes
nothing and exits 1, so the pipeline keeps the decks it has.
"""
import argparse
import collections
import datetime
import json
import math
import os
import re
import sys
import time
import urllib.parse
import urllib.request

API = "https://ygoprodeck.com/api/decks/getDecks.php"
UA = "Yu-Gi-Oh-Engine learning pipeline (github.com/0megaXP/Yu-Gi-Oh-Engine)"
PLACE = [("winner", 4), ("1st", 4), ("runner", 3), ("2nd", 3), ("top 4", 2), ("top4", 2), ("top 8", 1), ("top8", 1)]


def fetch(offset, limit):
    q = urllib.parse.urlencode({"_sft_category": "Tournament Meta Decks", "offset": offset, "limit": limit})
    req = urllib.request.Request(API + "?" + q, headers={"User-Agent": UA, "Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as r:
        data = json.loads(r.read().decode("utf-8"))
    if isinstance(data, dict):  # {"data": [...]} or similar
        for k in ("data", "decks", "results"):
            if isinstance(data.get(k), list):
                return data[k]
        return []
    return data if isinstance(data, list) else []


def cards(v):
    if isinstance(v, list):
        items = v
    elif isinstance(v, str):
        try:
            items = json.loads(v)
            if not isinstance(items, list):
                items = [items]
        except ValueError:
            items = re.split(r"[,\s]+", v)
    else:
        return []
    out = []
    for x in items:
        s = str(x).strip().strip('"')
        if s.isdigit():
            out.append(int(s))
    return out


def when(d, today):
    for k in ("tournamentDate", "tournament_date", "event_date", "submit_date", "date"):
        v = d.get(k)
        if not v:
            continue
        v = str(v)
        m = re.match(r"(\d{4})-(\d{2})-(\d{2})", v)
        if m:
            return datetime.date(int(m[1]), int(m[2]), int(m[3]))
        m = re.match(r"(\d+|an?|one)\s+(day|week|month|hour|minute)s?\s+ago", v.strip().lower())
        if m:
            n = 1 if m[1] in ("a", "an", "one") else int(m[1])
            days = {"day": 1, "week": 7, "month": 30, "hour": 0, "minute": 0}[m[2]] * n
            return today - datetime.timedelta(days=days)
    return None


def placement_points(p):
    p = str(p or "").lower()
    for key, pts in PLACE:
        if key in p:
            return pts
    return 0.5


def players(d):
    for k in ("tournamentPlayerCount", "players", "player_count"):
        m = re.search(r"\d+", str(d.get(k) or ""))
        if m:
            return int(m.group())
    return 32


def tcg(d):
    text = " ".join(str(d.get(k) or "") for k in ("format", "deck_name", "tournamentName", "category")).lower()
    return not any(x in text for x in ("ocg", "master duel", "genesys", "goat", "edison", "speed duel", "rush"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-decks", required=True)
    ap.add_argument("--out-lists", required=True)
    ap.add_argument("--report", required=True)
    ap.add_argument("--weeks", type=int, default=3)
    ap.add_argument("--by-results", type=int, default=3)
    ap.add_argument("--by-usage", type=int, default=5)
    ap.add_argument("--cache", help="read the pages from this file instead of the site (tests)")
    a = ap.parse_args()
    today = datetime.date.today()
    since = today - datetime.timedelta(weeks=a.weeks)
    rows = []
    if a.cache:
        rows = json.load(open(a.cache))
    else:
        limit, offset = 50, 0
        for _ in range(40):
            try:
                page = fetch(offset, limit)
            except Exception as e:  # network, site change
                print(f"fetch failed at offset {offset}: {e}", file=sys.stderr)
                break
            if not page:
                break
            rows += page
            dates = [when(d, today) for d in page]
            if all(x is not None and x < since for x in dates):
                break  # past the window (newest first)
            offset += len(page)
            time.sleep(1.0)
    lists, seen = [], set()
    for d in rows:
        if not tcg(d):
            continue
        day = when(d, today)
        if day is None or day < since:
            continue
        main_deck = cards(d.get("main_deck"))
        if len(main_deck) < 40:
            continue
        key = d.get("deckNum") or (d.get("deck_name"), d.get("tournamentName"), d.get("tournamentPlayerName"))
        if str(key) in seen:
            continue
        seen.add(str(key))
        size = players(d)
        lists.append({
            "archetype": str(d.get("deck_name") or "?").strip(),
            "player": str(d.get("tournamentPlayerName") or "").strip(),
            "event": str(d.get("tournamentName") or "").strip(),
            "placement": str(d.get("tournamentPlacement") or "").strip(),
            "players": size,
            "date": day.isoformat(),
            "points": placement_points(d.get("tournamentPlacement")) * max(0.5, 1 + math.log2(size / 32)),
            "main": main_deck, "extra": cards(d.get("extra_deck")), "side": cards(d.get("side_deck")),
        })
    by = collections.defaultdict(list)
    for l in lists:
        by[l["archetype"]].append(l)
    results = sorted(by, key=lambda k: (-sum(l["points"] for l in by[k]), -len(by[k]), k))
    usage = sorted(by, key=lambda k: (-len(by[k]), -sum(l["points"] for l in by[k]), k))
    if len(by) < 3:
        print(f"only {len(by)} archetypes in {len(rows)} rows: keeping the current decks", file=sys.stderr)
        sys.exit(1)
    pick_r = results[:a.by_results]
    pick_u = [k for k in usage if k not in pick_r][:a.by_usage]
    os.makedirs(a.out_decks, exist_ok=True)
    chosen = []
    for k in pick_r + pick_u:
        best = max(by[k], key=lambda l: (l["points"], l["date"]))
        slug = re.sub(r"[^a-z0-9]+", "-", k.lower()).strip("-")
        who = re.sub(r"[^a-z0-9]+", "-", (best["player"].split() or ["player"])[-1].lower()).strip("-") or "player"
        name = f"{slug}_{who}_{best['date']}.ydk"
        with open(os.path.join(a.out_decks, name), "w", encoding="utf-8") as f:
            f.write(f"#created by Yu-Gi-Oh! Engine — {k}, {best['player']}, {best['event']} {best['placement']} "
                    f"({best['date']}), via YGOPRODeck\n#main\n")
            f.write("".join(f"{c}\n" for c in best["main"]))
            f.write("#extra\n" + "".join(f"{c}\n" for c in best["extra"]))
            f.write("!side\n" + "".join(f"{c}\n" for c in best["side"]))
        chosen.append({"archetype": k, "why": "results" if k in pick_r else "usage", "file": name,
                       "results": round(sum(l["points"] for l in by[k]), 2), "lists": len(by[k])})
    json.dump({"version": 1, "since": since.isoformat(), "lists": [
        {"name": f"{l['archetype']} ({l['player']}, {l['event']} {l['placement']})", "main": l["main"], "weight": 1}
        for l in lists]}, open(a.out_lists, "w"), separators=(",", ":"))
    rep = {"since": since.isoformat(), "rows": len(rows), "lists": len(lists), "archetypes": len(by), "decks": chosen,
           "by_results": [[k, round(sum(l["points"] for l in by[k]), 2)] for k in results[:8]],
           "by_usage": [[k, len(by[k])] for k in usage[:10]]}
    json.dump(rep, open(a.report, "w"), indent=1)
    print(json.dumps(rep, indent=1))


if __name__ == "__main__":
    main()
