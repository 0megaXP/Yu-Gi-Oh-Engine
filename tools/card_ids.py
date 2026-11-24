#!/usr/bin/env python3
"""Card ids the engine doesn't know (YGO Omega's replays use them): what they are, from
YGOPRODeck, and the id the EDOPro scripts know that card by (ProjectIgnis BabelCDB).

usage: card_ids.py <BabelCDB dir> <id> [<id> ...]     (prints passcode_map.tsv rows, and notes)

An id can be an alternate artwork (YGOPRODeck lists each artwork's id under its card: the card's
own id is then the one to use) or a new card's official passcode (the scripts may still know it
by its pre-release id: found by the card's name in BabelCDB).
"""
import glob
import json
import sqlite3
import sys
import time
import urllib.request

API = "https://db.ygoprodeck.com/api/v7/cardinfo.php?id="


def babel(path):
    by_name = {}
    ids = set()
    for f in sorted(glob.glob(path + "/*.cdb")):
        con = sqlite3.connect(f)
        for i, name in con.execute("select d.id, t.name from datas d join texts t on d.id = t.id"):
            ids.add(i)
            by_name.setdefault(name.lower(), i)
    return by_name, ids


def main():
    path, ids = sys.argv[1], [int(x) for x in sys.argv[2:]]
    by_name, known = babel(path)
    for i in ids:
        try:
            req = urllib.request.Request(API + str(i), headers={"User-Agent": "Yu-Gi-Oh-Engine card ids"})
            data = json.loads(urllib.request.urlopen(req, timeout=30).read())["data"][0]
        except Exception as e:
            print(f"# {i}: not on YGOPRODeck ({e})")
            continue
        name, main = data["name"], data["id"]
        arts = [im["id"] for im in data.get("card_images", [])]
        script = main if main in known else by_name.get(name.lower())
        how = "alternate artwork of " + str(main) if i != main else "official passcode"
        if script is None:
            print(f"# {i}\t{name}: {how}; not in BabelCDB (no script yet)")
        elif script == i:
            print(f"# {i}\t{name}: already known to BabelCDB")
        else:
            print(f"{i}\t{script}\t{name}\tygoprodeck ({how}; artworks {arts})")
        time.sleep(0.1)


if __name__ == "__main__":
    main()
