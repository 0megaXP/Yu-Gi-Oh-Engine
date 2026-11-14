#!/usr/bin/env bash
# Fetches the pinned rules core, card scripts and card database into third_party/,
# then exports the card data the engine reads (data/cards.tsv).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TP="$ROOT/third_party"
mkdir -p "$TP"

# name | url | commit
DEPS=(
  "ygopro-core|https://github.com/edo9300/ygopro-core|c2e0b697e031d3560c077c9cd2cc76474969dedf"
  "ygopro-core/lua/src|https://github.com/lua/lua|6e22fedb74cf0c9b6656e9fce8b7331db847c605"
  "CardScripts|https://github.com/ProjectIgnis/CardScripts|9b2b77f222e8cb94a127232475f22355748cd12e"
  "BabelCDB|https://github.com/ProjectIgnis/BabelCDB|0f17a0ce1e8980374ef577e076e20d11d6057ede"
)

fetch() {
  local dir="$TP/$1" url="$2" commit="$3"
  if [ -d "$dir/.git" ] && [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null)" = "$commit" ]; then
    echo "ok    $1 @ ${commit:0:10}"; return
  fi
  rm -rf "$dir"; mkdir -p "$dir"
  git -C "$dir" init -q
  git -C "$dir" remote add origin "$url"
  # Shallow fetch of exactly the pinned commit; fall back to a full fetch if the host refuses.
  if ! git -C "$dir" fetch -q --depth 1 origin "$commit" 2>/dev/null; then
    git -C "$dir" fetch -q origin
  fi
  git -C "$dir" checkout -q "$commit"
  echo "fetch $1 @ ${commit:0:10}"
}

for d in "${DEPS[@]}"; do
  IFS='|' read -r name url commit <<<"$d"
  fetch "$name" "$url" "$commit"
done

# Our fixes to the fetched code: tools/patches/<dependency>/*.patch, applied in name order.
# Each is applied once (skipped when it is already in place).
for dir in "$ROOT"/tools/patches/*/; do
  [ -d "$dir" ] || continue
  dep="$(basename "$dir")"
  for patch in "$dir"*.patch; do
    [ -f "$patch" ] || continue
    if git -C "$TP/$dep" apply --reverse --check "$patch" 2>/dev/null; then
      echo "ok    patch $dep/$(basename "$patch")"
    else
      git -C "$TP/$dep" apply "$patch"
      echo "patch $dep/$(basename "$patch")"
    fi
  done
done

python3 "$ROOT/tools/export_cards.py" "$TP/BabelCDB" "$ROOT/data/passcode_map.tsv" "$ROOT/data/cards.tsv"
python3 "$ROOT/tools/analyze_scripts.py" "$ROOT/data/cards.tsv" "$TP/CardScripts" "$ROOT/data/effects.tsv"
echo "done. Build with: cmake -S $ROOT -B $ROOT/build -DCMAKE_BUILD_TYPE=Release && cmake --build $ROOT/build -j"
