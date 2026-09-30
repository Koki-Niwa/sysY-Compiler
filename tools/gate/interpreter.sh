# Sourced by tools/verify.sh; shared state/contract: gate/README.md.
INTERP_OK=0
if [ -n "${STAGE:-}" ] && declare -F stage_index >/dev/null; then
  requested_stage=$(stage_index "$STAGE") || return 2
  interpreter_stage=$(stage_index S10a) || return 2
  if [ "$requested_stage" -lt "$interpreter_stage" ]; then
    hdr "E2. AST 参考解释器"
    c_skip "阶段 $STAGE 不要求 S10a 解释器证据"
    return 0
  fi
fi

hdr "E2. AST 参考解释器"
RUNNER="$TOOLS/selftest/interpreter/run_s10a.sh"
if [ ! -x "$RUNNER" ]; then
  c_skip "S10a 隔离运行器缺失"
  return 0
fi
ILOG="$WORK/s10a.log"
if bash "$RUNNER" >"$ILOG" 2>&1; then
  INTERP_OK=1
  c_ok "S10a：AST 解释器固定/生成用例、重放、限制与负控通过"
  grep -E '^S10a fixed=|^S10a isolated PASS' "$ILOG" | tail -2 | sed 's/^/      /'
else
  c_bad "S10a AST 解释器自测失败"
  tail -20 "$ILOG" | sed 's/^/      /'
fi
return 0
