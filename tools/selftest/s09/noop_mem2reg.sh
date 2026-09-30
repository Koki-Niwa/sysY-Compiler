#!/usr/bin/env bash
# Deliberately dishonest S09 CLI: it accepts --mem2reg, copies the input, and
# reports a forged promotion count. The focused harness must reject it because
# the IR has not changed structurally.
set -u

input=""
output=""
dump_stats=0
while [ "$#" -gt 0 ]; do
  case "$1" in
    --mem2reg) shift ;;
    --dump-flat-stats) dump_stats=1; shift ;;
    --emit=*) shift ;;
    --emit|--from-flat)
      shift
      [ "$#" -gt 0 ] && [ "$1" = "flat-ir" ] && shift
      ;;
    -o) [ "$#" -ge 2 ] || exit 2; output="$2"; shift 2 ;;
    -*) shift ;;
    *) input="$1"; shift ;;
  esac
done

[ -n "$input" ] && [ -n "$output" ] || exit 2
cp -- "$input" "$output" || exit 1
if [ "$dump_stats" = 1 ]; then
  printf '%s\n' 'mem2reg promoted 1 allocas generated-phis 0 eliminated-phis 0' >&2
fi
