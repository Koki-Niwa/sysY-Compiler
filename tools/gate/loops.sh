# Sourced by tools/verify.sh; shared state/contract: gate/README.md.
hdr "C7. 循环规范化自校验（若 --normalize 已实现 —— S05b 起）"
# ─────────────────────────────────────────────────────────────────────────────
# 这是第一个"变换"而不是降级：判据只有一个——**行为不变**。
# 关卡守四件可机械判定的事：
#   ① 冻结契约：不带开关时与 S05 样例对逐字节相同、往返同构（含"不触发用例零改动"）；
#   ② 不变式 I1–I3 **本关才真正有意义**，且必须能被 3 份人为违反反证；
#   ③ ★ 独立实现的第二份规范化器逐字节一致；
#   ④ ★ 受限执行器：变换前后两份 IR 用同一输入跑出**相同轨迹**——这是唯一能抓
#      边界 off-by-one 的判据（设计文档的调度等价性模型检查自己标了"必要条件，
#      不是充分条件"，抓不到 IV 替换错与边界差一）。
CAN_NORM=0
if [ -x "$COMPILER" ] && [ -f "$TOOLS/selftest/check_loopnorm.py" ]; then
  if "$COMPILER" --normalize --emit=structured-ir "$TESTS/final_arm/functional/00_main.sy" \
        -o "$WORK/norm_probe.txt" >/dev/null 2>&1 && grep -q '^(Module' "$WORK/norm_probe.txt" 2>/dev/null; then
    CAN_NORM=1
  fi
fi
if [ "$CAN_NORM" = "0" ]; then
  c_skip "循环规范化跳过：--normalize 尚未实现（S05b 的交付物）"
else
  # ①②③零改动：一次性跑完全量（含两种模式的往返与"不触发用例零改动"）
  NLOG="$WORK/loopnorm.log"
  if python3 "$TOOLS/selftest/check_loopnorm.py" --compiler "$COMPILER" \
        --jobs "$(nproc)" --probe --variants >"$NLOG" 2>&1; then
    c_ok "循环规范化：不变式 + 往返 + 零改动 + 反证"
    grep -E '成功率|零改动|反例|变体' "$NLOG" | head -4 | sed 's/^/      /'
  else
    c_bad "循环规范化检查失败（不变式 / 往返 / 零改动 / 反证）"
    grep -E '✘|违反|失败' "$NLOG" | head -8 | sed 's/^/      /'
  fi
  # ③ 独立实现的第二份规范化器
  if [ -f "$TOOLS/selftest/independent_loopnorm_check.py" ]; then
    LLOG="$WORK/iloopnorm.log"
    if [ "$QUICK" = 1 ]; then
      c_skip "轨 D（独立规范化器）--quick 档跳过 —— **交付前请跑全量**"
    elif python3 "$TOOLS/selftest/independent_loopnorm_check.py" --compiler "$COMPILER" \
          --jobs "$(nproc)" >"$LLOG" 2>&1; then
      c_ok "独立规范化器：与 C++ 输出逐字节一致"
    else
      c_bad "独立规范化器不一致（C++ 侧或独立侧有真 bug）"
      grep -E '不一致|差异|✘' "$LLOG" | head -8 | sed 's/^/      /'
    fi
  else
    c_skip "independent_loopnorm_check.py 未实现（S05b 的交付物）"
  fi

  # ④ 行为等价（唯一能抓 off-by-one 的一条）
  if [ -f "$TOOLS/selftest/loopnorm_exec.py" ]; then
    ELOG="$WORK/loopnorm_exec.log"
    if [ "$QUICK" = 1 ]; then
      c_skip "轨 E（行为等价执行器）--quick 档跳过 —— **交付前请跑全量**"
    elif python3 "$TOOLS/selftest/loopnorm_exec.py" --compiler "$COMPILER" \
          --jobs "$(nproc)" >"$ELOG" 2>&1; then
      c_ok "行为等价：变换前后两份 IR 轨迹相同"
      grep -E '轨迹相同|跳过' "$ELOG" | tail -2 | sed 's/^/      /'
    else
      c_bad "行为等价检查失败（变换改了行为——先假定是规范化错了）"
      grep -E '不同|差异|✘' "$ELOG" | head -8 | sed 's/^/      /'
    fi
  else
    c_skip "loopnorm_exec.py 未实现（S05b 的交付物）"
  fi

  # 用例集
  if [ -f "$TOOLS/selftest/run_loopnorm_cases.py" ]; then
    KLOG="$WORK/loopnorm_cases.log"
    if python3 "$TOOLS/selftest/run_loopnorm_cases.py" --compiler "$COMPILER" >"$KLOG" 2>&1; then
      SUM=$(grep -E '通过|总计' "$KLOG" | tail -1)
      c_ok "循环规范化用例集：${SUM:-全过}"
    else
      c_bad "循环规范化用例集有失败"
      grep -E '✘|失败|FAIL' "$KLOG" | head -8 | sed 's/^/      /'
    fi
  else
    c_skip "run_loopnorm_cases.py 未实现（S05b 的交付物）"
  fi
fi
# 两份自洽判据可能共享误解；C8 的可观察结果还须由 GCC 锚点验证。

return 0
