#!/usr/bin/env bash
# S08 isolated public FlatVerifier API matrix.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
SRC="$ROOT/compiler/src"
CXX="${CXX:-clang++}"
WORK="${WORK:-$(mktemp -d "${TMPDIR:-/tmp}/sysy_s08_XXXXXX")}"
KEEP=0
while [ $# -gt 0 ]; do
  case "$1" in
    --work) WORK="$2"; shift 2 ;;
    --keep) KEEP=1; shift ;;
    -h|--help) sed -n '2,4p' "${BASH_SOURCE[0]}" | sed 's/^# //'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
mkdir -p "$WORK"
OUT="$WORK/s08_flat_verifier_matrix"
BUILD_LOG="$WORK/build.log"
RUN_LOG="$WORK/run.out"
SOURCES=("$HERE/../s08_flat_verifier_matrix.cpp" "$SRC"/ir/*.cpp
         "$SRC/support/SourceFile.cpp" "$SRC/support/Diagnostic.cpp")

echo "S08 work: $WORK"
python3 "$HERE/validate_legacy_map.py" "$HERE/../s08_flat_verifier_matrix.cpp" \
  "$ROOT/compiler/tools/selftest/check_flat.py" || exit 1
"$CXX" --std=c++17 -Wall -Wextra -Wpedantic -Werror -O1 -I"$SRC" \
  "${SOURCES[@]}" -o "$OUT" >"$BUILD_LOG" 2>&1
if [ "$?" -ne 0 ]; then
  sed -n '1,240p' "$BUILD_LOG" >&2
  echo "S08 BUILD FAIL"
  exit 1
fi
if ! "$OUT" >"$RUN_LOG" 2>&1; then
  cat "$RUN_LOG"
  echo "S08 matrix FAIL"
  exit 1
fi
cat "$RUN_LOG"
if [ "$KEEP" -eq 0 ]; then rm -rf "$WORK"; else echo "S08 artifacts: $WORK"; fi
