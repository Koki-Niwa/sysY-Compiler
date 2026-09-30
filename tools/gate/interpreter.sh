# Sourced by tools/verify.sh; shared state/contract: gate/README.md.
INTERP_OK=0
if [ -n "${STAGE:-}" ]; then
  declare -F stage_requires >/dev/null || return 2
  stage_requires "$STAGE" S10a
  stage_requirement=$?
  if [ "$stage_requirement" -gt 1 ]; then
    return "$stage_requirement"
  elif [ "$stage_requirement" = 1 ]; then
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
S10A_REPORT_PATH=""
S10A_ARGS=()
if [ -n "${STAGE:-}" ]; then
  S10A_REPORT_PATH="${S10A_REPORT:-$ROOT/docs/handoff/reports/S10A-LATEST.json}"
  S10A_ARGS=(--report "$S10A_REPORT_PATH")
fi
if bash "$RUNNER" "${S10A_ARGS[@]}" >"$ILOG" 2>&1 &&
   { [ -z "$S10A_REPORT_PATH" ] ||
     { grep -Fq '"state": "PASS"' "$S10A_REPORT_PATH" &&
       grep -Fq '"source_dirty": false' "$S10A_REPORT_PATH"; }; }; then
  INTERP_OK=1
  c_ok "S10a：AST 解释器固定/生成用例、重放、限制与负控通过"
  grep -E '^S10a fixed=|^S10a isolated PASS' "$ILOG" | tail -2 | sed 's/^/      /'
  if [ -n "$S10A_REPORT_PATH" ]; then
    echo "      revision-bound manifest: $S10A_REPORT_PATH"
  fi
else
  c_bad "S10a AST 解释器自测或 revision-bound manifest 失败"
  tail -20 "$ILOG" | sed 's/^/      /'
fi
return 0
