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
build/ygoe-cli library examples/decks/angelechy_luna_mardelplata_2026-11.ydk --hands 1 --iterations 60 \
  --out "$OUT/libs/angelechy_luna_mardelplata_2026-11.json" > "$OUT/lib.txt"
YGOE_FOLLOW_JSONL=1 build/ygoe-cli followtest examples/decks/angelechy_luna_mardelplata_2026-11.ydk examples/decks/elfnote_koller_wien_2026-11.ydk \
  --seed 1 --turns 4 | python3 -c "import json,sys; [print(l) for r in map(json.loads, sys.stdin) for l in r['jsonl'].splitlines() if '\"kind\":\"start\"' in l]" > "$OUT/starts.jsonl"
build/ygoe-cli selfplay examples/decks/angelechy_luna_mardelplata_2026-11.ydk examples/decks/elfnote_koller_wien_2026-11.ydk \
  --games 2 --iterations 40 --turns 3 --net none --libraries "$OUT/libs" --bo3 1 --explore 0.5 \
  --starts "$OUT/starts.jsonl" --start-share 0.5 --policy-out "$OUT/sp.pjsonl" --out "$OUT/sp.jsonl" \
  --ne-explore 0.2 --ne-out "$OUT/sp.nejsonl" --games-out "$OUT/sp.gjsonl" > "$OUT/sp.txt"
python3 tools/ml/nonengine.py values "$OUT/sp.gjsonl" --out "$OUT/ne_values.json" > /dev/null
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
