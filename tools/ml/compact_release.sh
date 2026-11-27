#!/usr/bin/env bash
# Packs the ml-data release's many small uploads into archive files (a release holds at most
# 1000 assets; at 1000 every upload fails): replays-* (all) and inputs-*, local-pos-*,
# local-pol-* older than a day into one archive each (tools/ml/compact.py: read like the files
# they replace), and local-newpieces-* into one library file (the pool job merges and deletes it).
# Needs GH_TOKEN with contents: write; run from the repository root.
set -uo pipefail
REPO=${GITHUB_REPOSITORY:-0megaXP/Yu-Gi-Oh-Engine}
W=$(mktemp -d)
cutoff=$(date -u -d '1 day ago' +%Y-%m-%dT%H:%M:%SZ)
gh api --paginate "repos/$REPO/releases/tags/ml-data" --jq '.assets[] | "\(.id) \(.name) \(.created_at)"' > "$W/assets.txt"
echo "ml-data: $(wc -l < "$W/assets.txt") assets"

delete_ids() {  # asset ids on stdin
  while read -r id; do gh api -X DELETE "repos/$REPO/releases/assets/$id" > /dev/null || echo "could not delete $id"; done
}

# pack <prefix> <archive name> <mode> <age: old|all>
pack() {
  local prefix=$1 archive=$2 mode=$3 age=$4 d="$W/$1"
  mkdir -p "$d"
  # the files to pack: the prefix, not the archive itself, old enough
  awk -v p="$prefix" -v a="$archive" -v c="$cutoff" -v age="$age" \
    'index($2, p) == 1 && $2 != a && (age == "all" || $3 < c) {print $1, $2}' "$W/assets.txt" > "$W/$prefix.list"
  local n; n=$(wc -l < "$W/$prefix.list")
  if [ "$n" -lt 2 ]; then echo "$prefix: $n to pack, left as they are"; return; fi
  while read -r id name; do
    gh api -H "Accept: application/octet-stream" "repos/$REPO/releases/assets/$id" > "$d/$name" || { echo "could not download $name"; rm -f "$d/$name"; }
  done < "$W/$prefix.list"
  # the archive of before, packed in too (oldest)
  if grep -q " $archive " "$W/assets.txt"; then
    gh api -H "Accept: application/octet-stream" "repos/$REPO/releases/assets/$(awk -v a="$archive" '$2 == a {print $1}' "$W/assets.txt")" > "$d/$archive"
  fi
  case "$mode" in
    replays|inputs) python3 tools/ml/compact.py "$mode" "$d" "$W/$archive" || return ;;
    lines) python3 tools/ml/compact.py lines "$d" "${prefix}*.jsonl.gz" "$W/$archive" || return ;;
    pieces) python3 - "$d" "$W/$archive" <<'PY' || return
import glob, json, os, sys
d, out = sys.argv[1:3]
pieces, lines = [], 0
for f in sorted(glob.glob(os.path.join(d, "local-newpieces-*.json"))):
    try:
        v = json.load(open(f))
    except ValueError:
        continue
    if isinstance(v, dict):
        lines += int(v.get("lines", 0))
        pieces += v.get("pieces", [])
    else:
        pieces += v
json.dump({"lines": lines, "pieces": pieces}, open(out, "w"), separators=(",", ":"))
print(f"{len(pieces)} pieces packed into {os.path.basename(out)}")
PY
    ;;
  esac
  [ -s "$W/$archive" ] || { echo "$prefix: no archive written"; return; }
  # a free place for the archive if the release is full: one of the packed files goes first
  # (its content is in the archive and in this runner)
  local first; first=$(head -1 "$W/$prefix.list")
  if [ "$(gh api --paginate "repos/$REPO/releases/tags/ml-data" --jq '.assets | length' | awk '{s+=$1} END {print s}')" -ge 1000 ]; then
    echo "${first%% *}" | delete_ids
    sed -i 1d "$W/$prefix.list"
  fi
  if gh release upload ml-data "$W/$archive" --clobber -R "$REPO"; then
    awk '{print $1}' "$W/$prefix.list" | delete_ids
    echo "$prefix: $n files packed into $archive"
  else
    echo "$prefix: the archive could not be uploaded; the files stay"
    # (put back the one deleted for room, if any)
    [ -n "$first" ] && [ -f "$d/${first#* }" ] && gh release upload ml-data "$d/${first#* }" -R "$REPO" || true
  fi
}

pack replays- replays-0-archive.jsonl.gz replays all
pack local-newpieces- local-newpieces-0-archive.json pieces all
pack inputs- inputs-0-z-archive.jsonl.gz inputs old
pack local-pos- local-pos-0-archive.jsonl.gz lines old
pack local-pol- local-pol-0-archive.jsonl.gz lines old
echo "ml-data: $(gh api --paginate "repos/$REPO/releases/tags/ml-data" --jq '.assets | length' | awk '{s+=$1} END {print s}') assets"
