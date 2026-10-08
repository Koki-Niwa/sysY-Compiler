# Sourced by tools/verify.sh; requires a fresh full behavior run on promoted flat IR.
MEM2REG_OK=0
if [ -n "${STAGE:-}" ]; then
  declare -F stage_requires >/dev/null || return 2
  stage_requirement=0
  stage_requires "$STAGE" S09
  stage_requirement=$?
  if [ "$stage_requirement" -gt 1 ]; then
    return "$stage_requirement"
  elif [ "$stage_requirement" = 1 ]; then
    hdr "C8b. mem2reg 行为对照"
    c_skip "阶段 $STAGE 不要求 S09 mem2reg"
    return 0
  fi
fi

hdr "C8b. mem2reg 行为对照（S09 起）"
if [ "$QUICK" = 1 ]; then
  c_skip "S09 全语料提升后行为对照被 --quick 跳过"
  return 0
fi
if [ ! -x "$COMPILER" ] || [ ! -f "$TOOLS/selftest/flat_exec.py" ]; then
  c_bad "S09 mem2reg 编译器或行为对照工具缺失"
  return 0
fi

S09_FIXTURE="$TOOLS/selftest/s09/promotable.flat"
S09_SHIM="$TOOLS/selftest/s09/noop_mem2reg.sh"
S09_HARNESS="$TOOLS/selftest/s09/check_mem2reg.py"
S09_STRUCTURE_LOG="$WORK/mem2reg_structure.log"
if python3 "$S09_HARNESS" --compiler "$COMPILER" --shim "$S09_SHIM" \
      --source "$S09_FIXTURE" --work "$WORK/s09" >"$S09_STRUCTURE_LOG" 2>&1; then
  c_ok "S09：结构化提升证据通过（baseline memory form -> promoted SSA/direct form）"
  grep -E '^S09_(PROMOTED|BASELINE|PROMOTED_MEMORY)|^S09 negative' \
    "$S09_STRUCTURE_LOG" | sed 's/^/      /'
else
  c_bad "S09 结构化提升证据失败，或伪造统计量负控未被拒绝"
  tail -20 "$S09_STRUCTURE_LOG" | sed 's/^/      /'
  return 0
fi

MEM2REG_TEST_LOG="$WORK/mem2reg_runner_tests.log"
if PYTHONPATH="$ROOT/compiler${PYTHONPATH:+:$PYTHONPATH}" \
     python3 -m unittest tools.selftest.test_flat_exec_gate >"$MEM2REG_TEST_LOG" 2>&1; then
  c_ok "S09：提升后轨迹 runner 负控与 --mem2reg CLI 接线测试通过"
else
  c_bad "S09 提升后轨迹 runner 负控/CLI 接线测试失败"
  tail -16 "$MEM2REG_TEST_LOG" | sed 's/^/      /'
  return 0
fi

MEM2REG_LOG="$WORK/mem2reg.log"
if python3 "$TOOLS/selftest/flat_exec.py" --compiler "$COMPILER" \
      --dir "$TESTS" --jobs "$(nproc)" --min-files 400 --mem2reg \
      >"$MEM2REG_LOG" 2>&1 && \
   grep -Eq '^参与执行的文件数: 540$' "$MEM2REG_LOG" && \
   grep -Eq '^可观察轨迹不同:       0$' "$MEM2REG_LOG" && \
   grep -Eq '^== 结果：全过 ==$' "$MEM2REG_LOG"; then
  MEM2REG_OK=1
  c_ok "S09：提升后 flat IR 与结构化 IR 可观察行为一致"
  grep -E '参与执行|完整轨迹相同|循环计数口径不同|可观察轨迹不同|^跳过:' "$MEM2REG_LOG" | sed 's/^/      /'
else
  c_bad "S09 提升后 flat IR 行为对照失败或覆盖不足"
  grep -E '参与执行|完整轨迹|循环计数|可观察轨迹|^跳过:|结果|✘|Traceback|Error' \
    "$MEM2REG_LOG" | tail -12 | sed 's/^/      /'
fi
return 0
