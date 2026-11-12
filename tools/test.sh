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
build/ygoe-cli profile decks/elfnote_koller_wien_2026-11.ydk > /dev/null
build/ygoe-cli side decks/elfnote_koller_wien_2026-11.ydk decks/angelechy_luna_mardelplata_2026-11.ydk | grep -q "Going second"
echo "== JSON API"
for call in analyze profile side; do
  build/ygoe-cli json "$call" "examples/request_$call.json" > "$OUT/$call.json"
done

echo "== asset pack + JNI"
python3 tools/make_pack.py third_party/CardScripts data "$OUT/pack" > /dev/null
mkdir -p "$OUT/classes"
javac -d "$OUT/classes" tests/jni/com/ygoengine/*.java
java -Djava.library.path=build-jni -cp "$OUT/classes" com.ygoengine.JniSmoke "$OUT/pack" examples | grep -v '^progress'
echo "all desktop checks passed"
