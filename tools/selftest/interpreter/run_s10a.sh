#!/usr/bin/env bash
# S10a standalone gate. It builds only frontend + interpreter sources and
# never invokes IR, LLVM, target, or external runtime execution.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
CXX="${CXX:-clang++}"
WORK="${WORK:-$(mktemp -d /tmp/sysy_s10a_XXXXXX)}"
KEEP=0
REPORT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --work)
      [ $# -ge 2 ] || { echo "--work requires a path" >&2; exit 2; }
      WORK="$2"; shift 2 ;;
    --report)
      [ $# -ge 2 ] || { echo "--report requires a path" >&2; exit 2; }
      REPORT="$2"; shift 2 ;;
    --keep) KEEP=1; shift ;;
    -h|--help) sed -n '2,8p' "${BASH_SOURCE[0]}" | sed 's/^# //'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
# A manifest must keep the artifacts whose hashes it records.  The stage gate
# may pass only --report, so report mode implies --keep.
[ -n "$REPORT" ] && KEEP=1
mkdir -p "$WORK"
SRC="$ROOT/compiler/src"
OUT="$WORK/s10a"
BUILD_LOG="$WORK/build.log"
RUN_LOG="$WORK/run.out"
ERR_LOG="$WORK/run.err"

write_report() {
  [ -z "$REPORT" ] && return 0
  python3 "$HERE/s10a_report.py" --state "$1" --report "$REPORT" \
    --root "$ROOT" --work "$WORK" --reason "${2:-}"
}

SOURCES=(
  "$HERE/s10a_driver.cpp" "$HERE/s10a_cases.cpp" "$HERE/s10a_generator.cpp"
  "$HERE/s10a_oracle.cpp"
  "$SRC/interpreter/Interpreter.cpp"
  "$SRC/interpreter/InterpreterIO.cpp"
  "$SRC/interpreter/InterpreterRuntime.cpp"
  "$SRC/support/SourceFile.cpp" "$SRC/support/Diagnostic.cpp"
  "$SRC/frontend/Lexer.cpp" "$SRC/frontend/Parser.cpp" "$SRC/frontend/ParserStmtExpr.cpp"
  "$SRC/frontend/ConstEval.cpp" "$SRC/frontend/RuntimeLib.cpp"
  "$SRC/frontend/Sema.cpp" "$SRC/frontend/SemaStmt.cpp" "$SRC/frontend/SemaExpr.cpp"
  "$SRC/frontend/SemaDecl.cpp"
)

echo "S10a work: $WORK"
write_report RUNNING || exit 1
"$CXX" --std=c++17 -Wall -Wextra -Wpedantic -Werror -O1 -I"$SRC" \
  "${SOURCES[@]}" -o "$OUT" >"$BUILD_LOG" 2>&1
if [ "$?" -ne 0 ]; then
  sed -n '1,240p' "$BUILD_LOG" >&2
  write_report FAIL build || exit 1
  echo "S10a BUILD FAIL"
  exit 1
fi
"$OUT" >"$RUN_LOG" 2>"$ERR_LOG"
rc=$?
cat "$RUN_LOG"
if [ -s "$ERR_LOG" ]; then sed -n '1,120p' "$ERR_LOG" >&2; fi
if [ "$rc" -eq 0 ]; then
  write_report PASS || exit 1
  echo "S10a isolated PASS"
else
  write_report FAIL run || exit 1
  echo "S10a isolated FAIL"
fi
if [ "$KEEP" -eq 0 ] && [ "$rc" -eq 0 ]; then rm -rf "$WORK"; else echo "S10a artifacts: $WORK"; fi
exit "$rc"
