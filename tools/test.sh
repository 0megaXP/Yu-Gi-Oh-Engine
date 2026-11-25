#!/usr/bin/env bash
# Desktop checks: CLI on the examples, JSON API, and the JNI bridge through Java.
# Needs tools/bootstrap.sh to have run. Usage: tools/test.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="$ROOT/build-test"
mkdir -p "$OUT"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j
cmake -S . -B build-jni -DCMAKE_BUILD_TYPE=Release -DYGOE_BUILD_JNI=ON -DYGOE_BUILD_CLI=OFF >/dev/null
cmake --build build-jni -j

echo "== CLI analyze (practice)"
build/ygoe-cli analyze examples/angelechy_first_practice.txt --iterations 300 --verify-iterations 80 --lines 1 > "$OUT/practice.txt"
grep -q "^#1 " "$OUT/practice.txt"
echo "== CLI analyze (real)"
build/ygoe-cli analyze examples/angelechy_first_real.txt --iterations 300 --no-verify --lines 1 --samples 4 > "$OUT/real.txt"
grep -q "^#1 " "$OUT/real.txt"
echo "== CLI profile / side"
build/ygoe-cli profile examples/decks/elfnote_koller_wien_2026-11.ydk > /dev/null
build/ygoe-cli side examples/decks/elfnote_koller_wien_2026-11.ydk examples/decks/angelechy_luna_mardelplata_2026-11.ydk | grep -q "Going second"
echo "== bridges (reach.h): Elfnote joins its Power Patron cards"
build/ygoe-cli bridges examples/decks/elfnote_koller_wien_2026-11.ydk --reach data/reach.tsv | grep -q '"bridges":\[{'
echo "== triggers (trigger.h): Artmage Diactorus floats when destroyed"
build/ygoe-cli triggers 27184601 | grep -q "destroyed from the Monster Zone"
echo "== learning in the engine: self-play positions, then the C++ trainer (nettrain.h)"
build/ygoe-cli selfplay examples/decks/angelechy_luna_mardelplata_2026-11.ydk examples/decks/elfnote_koller_wien_2026-11.ydk \
  --games 3 --iterations 20 --turns 4 --seed 2 --out "$OUT/learn-pos.jsonl" > /dev/null
build/ygoe-cli train-net "$OUT/learn-pos.jsonl" --out "$OUT/learn-net.json" --epochs 5 | grep -q '"net_logloss"'
build/ygoe-cli netcheck "$OUT/learn-net.json" "$OUT/learn-pos.jsonl" | head -1 | grep -qE '^-?[0-9]'
echo "== simulator replays: a saved duel is rebuilt with the same log, and targets are shown"
build/ygoe-cli duel examples/decks/angelechy_luna_mardelplata_2026-11.ydk examples/decks/elfnote_koller_wien_2026-11.ydk \
  --bot0 easy --bot1 easy --turns 4 --seed 11 --save "$OUT/sim.json" | grep -v "^  >\|bot decisions\|^$" > "$OUT/sim.txt"
build/ygoe-cli simreplay "$OUT/sim.json" | grep -v "^  >\|^after\|  chain:\|  next:\|^$" | diff -q - "$OUT/sim.txt" > /dev/null
grep -q " targets " "$OUT/sim.txt"
echo "== zones: Elfnote Regina goes to the center; a Pendulum deck keeps its Pendulum Zones free"
# (examples/sim_elfnote_mirror.json: an app bot-vs-bot duel where both went wrong)
build/ygoe-cli simreplay examples/sim_elfnote_mirror.json --at 27 --options | grep -A1 "next: P1 place, minor" | grep -q "Monster Zone, column 3"
build/ygoe-cli simreplay examples/sim_elfnote_mirror.json --at 192 --options | grep -A1 "next: P1 place" | tail -1 | grep -qv "column 1\|column 5"
echo "== battle: on an open field the bot attacks instead of ending the Battle Phase"
build/ygoe-cli simreplay examples/sim_elfnote_mirror.json --at 304 --bot normal --iterations 60 | grep -q "battle: Attack with"
echo "== negates wait for an activation: Infinite Impermanence in an open Battle Phase window"
YGOE_BOT_DEBUG=1 build/ygoe-cli simreplay examples/sim_elfnote_mirror.json --at 848 --bot normal --iterations 60 2> "$OUT/imperm.txt" | grep -q "chain: Don't respond"
grep -q "Infinite Impermanence: no activation to answer" "$OUT/imperm.txt"
echo "== turn planner: no Battle Phase with no monsters while combo cards are left"
build/ygoe-cli simreplay examples/sim_elfnote_mirror.json --at 197 --bot normal --iterations 150 > "$OUT/plan197.txt"
grep "idle:" "$OUT/plan197.txt" | grep -vq "Go to Battle Phase"
echo "== once per turn: the rules core's count of what is left reaches the board"
YGOE_BOT_DEBUG=1 build/ygoe-cli duel examples/decks/angelechy_luna_mardelplata_2026-11.ydk examples/decks/elfnote_koller_wien_2026-11.ydk \
  --turns 1 --seed 3 > /dev/null 2> "$OUT/opt.txt"
python3 -c "
import re,sys
used=[m for m in re.findall(r'once per turn: (\d+)/(\d+) left', open(sys.argv[1], errors='replace').read()) if int(m[0]) < int(m[1])]
sys.exit(0 if used else 'no card showed a once-per-turn effect used')" "$OUT/opt.txt"
echo "== JSON API"
for call in analyze profile side; do
  build/ygoe-cli json "$call" "examples/request_$call.json" > "$OUT/$call.json"
done

echo "== video lines (one playable line, one that isn't)"
build/ygoe-cli json lines examples/request_lines.json > "$OUT/lines.json"
grep -q '"verified":1,' "$OUT/lines.json"

echo "== benchmark (replay lines, small budget: the checker must still follow them)"
build/ygoe-cli bench bench/suite.json --only lines --iterations 150 --out "$OUT/bench.json" > "$OUT/bench.txt"
python3 -c "import json,sys; t=json.load(open(sys.argv[1]))['totals']; sys.exit(0 if t['lines.followed'] >= 0.85 else 'replay lines followed: %.2f' % t['lines.followed'])" "$OUT/bench.json"
echo "== learning: combo pieces, self-play (best of 3, starts), training, policy, arena (tiny)"
rm -f "$OUT/sp.jsonl" "$OUT/sp.pjsonl" "$OUT/sp.nejsonl" "$OUT/sp.gjsonl"
mkdir -p "$OUT/libs"
rm -f "$OUT/pool.json"
build/ygoe-cli library examples/decks/angelechy_luna_mardelplata_2026-11.ydk --hands 1 --iterations 60 \
  --pool "$OUT/pool.json" --new-out "$OUT/new.json" --out "$OUT/libs/angelechy_luna_mardelplata_2026-11.json" > "$OUT/lib.txt"
# the shared pool: a run's pieces go in, and the next run's library starts from them
build/ygoe-cli pool-merge "$OUT/pool.json" "$OUT/new.json" --out "$OUT/pool.json" | grep -q "shared pool: [1-9]"
build/ygoe-cli library examples/decks/angelechy_luna_mardelplata_2026-11.ydk --hands 1 --iterations 60 --seed 2 \
  --pool "$OUT/pool.json" --out "$OUT/lib2.json" | grep -q "and [1-9][0-9]* the shared pool has"
YGOE_FOLLOW_JSONL=1 build/ygoe-cli followtest examples/decks/angelechy_luna_mardelplata_2026-11.ydk examples/decks/elfnote_koller_wien_2026-11.ydk \
  --seed 1 --turns 4 > "$OUT/follow.json"
python3 -c "import json,sys; [print(l) for r in map(json.loads, open(sys.argv[1])) for l in r['jsonl'].splitlines() if '\"kind\":\"start\"' in l]" "$OUT/follow.json" > "$OUT/starts.jsonl"
# the upload holds every position the follower counted (and the follower version the app checks)
python3 -c "
import json,sys
r=json.loads(open(sys.argv[1]).readline())
n=sum(1 for l in r['jsonl'].splitlines() if '\"f\":' in l)
sys.exit(0 if r['positions'] > 0 and n == r['positions'] and r.get('follow_version', 0) >= 2 else f'positions: {n} written, {r[\"positions\"]} counted')" "$OUT/follow.json"
build/ygoe-cli selfplay examples/decks/angelechy_luna_mardelplata_2026-11.ydk examples/decks/elfnote_koller_wien_2026-11.ydk \
  --games 2 --iterations 40 --turns 3 --net none --libraries "$OUT/libs" --bo3 1 --explore 0.5 \
  --starts "$OUT/starts.jsonl" --start-share 0.5 --policy-out "$OUT/sp.pjsonl" --out "$OUT/sp.jsonl" \
  --ne-explore 0.2 --ne-out "$OUT/sp.nejsonl" --games-out "$OUT/sp.gjsonl" > "$OUT/sp.txt"
python3 tools/ml/nonengine.py values "$OUT/sp.gjsonl" --out "$OUT/ne_values.json" > /dev/null
# the pipeline following the app's uploaded games (a real 2-turn YGO Omega game, followed to the end)
build/ygoe-cli followall examples/follow_input.jsonl --out "$OUT/fa.jsonl" --summary "$OUT/fa_sum.jsonl" > /dev/null
python3 -c "
import json,sys
s=json.loads(open(sys.argv[1]).readline()); n=sum(1 for l in open(sys.argv[2]) if '\"f\":' in l)
sys.exit(0 if s['complete'] and n > 0 else f'followall: complete {s[\"complete\"]}, {n} positions')" "$OUT/fa_sum.jsonl" "$OUT/fa.jsonl"
mkdir -p "$OUT/followed" && cp "$OUT/fa.jsonl" "$OUT/fa_sum.jsonl" "$OUT/followed/"
python3 tools/ml/replays.py merge "$OUT/none.jsonl" "$OUT/followed" "$OUT/merged.jsonl" > /dev/null
python3 tools/ml/decklib.py decks --starts "$OUT/starts.jsonl" --out "$OUT/deck_library.json" > /dev/null
# hidden information: every guess of the other player's cards keeps what the bot sees (guessed decks from data/)
YGOE_HIDDEN_DEBUG=1 build/ygoe-cli selfplay examples/decks/angelechy_luna_mardelplata_2026-11.ydk examples/decks/elfnote_koller_wien_2026-11.ydk \
  --games 1 --iterations 30 --turns 3 --net none --seed 4 --out "$OUT/hidden.jsonl" > /dev/null 2> "$OUT/hidden.txt"
grep -q "hidden: turn 2" "$OUT/hidden.txt" && ! grep -v " 0 bad" "$OUT/hidden.txt" | grep -q "^hidden" || { echo "hidden information failed"; exit 1; }
build/ygoe-cli nonengine examples/decks/angelechy_luna_mardelplata_2026-11.ydk examples/decks/elfnote_koller_wien_2026-11.ydk | grep -q "hand trap"
python3 tools/ml/train.py "$OUT/sp.jsonl" "$OUT/starts.jsonl" --epochs 2 --out "$OUT/net.json" --report "$OUT/net_report.json" > /dev/null
build/ygoe-cli policy-train "$OUT/sp.pjsonl" --epochs 1 --out "$OUT/policy.json" --report "$OUT/policy_report.json" > /dev/null
build/ygoe-cli arena examples/decks/angelechy_luna_mardelplata_2026-11.ydk examples/decks/elfnote_koller_wien_2026-11.ydk \
  --games 2 --iterations 40 --turns 2 --net "$OUT/net.json" --vs none --policy "$OUT/policy.json" --vs-policy none \
  --libraries "$OUT/libs" --out "$OUT/arena.json" > "$OUT/arena.txt"
python3 -c "import json,sys; r=json.load(open(sys.argv[1])); sys.exit(0 if r['games']==2 and len(r['pairs'])==1 else 'arena failed')" "$OUT/arena.json"
mkdir -p "$OUT/arena"
cp "$OUT/arena.json" "$OUT/arena/arena-1.json"
python3 tools/ml/arena.py "$OUT/arena" --pending "$OUT/none.json" --out-pending "$OUT/pending.json" --run 1 > /dev/null
python3 -c "import json,sys; v=json.load(open(sys.argv[1])); sys.exit(0 if 'first' in v else 'no first/second rates')" "$OUT/ne_values.json"
echo "== asset pack + JNI"
python3 tools/make_pack.py third_party/CardScripts data "$OUT/pack" > /dev/null
mkdir -p "$OUT/classes"
javac -d "$OUT/classes" tests/jni/com/ygoengine/*.java
java -Djava.library.path=build-jni -cp "$OUT/classes" com.ygoengine.JniSmoke "$OUT/pack" examples | grep -v '^progress'
echo "all desktop checks passed"
