# Sourced by tools/verify.sh; shared state/contract: gate/README.md.
hdr "C6. 结构化 IR 自校验（若 --emit=structured-ir 已实现 —— S05 起）"
# ─────────────────────────────────────────────────────────────────────────────
# 这一关的产物没有"对着规范判对错"的直接判据：同一段源码有无数种正确的 IR 形态。
# 所以关卡自己守三件可机械判定的事，语义等价交给开发方的两条独立实现去比：
#   ① 样例对逐字节（格式契约）；② 结构/覆盖性检查器；③ ★ 独立实现的第二份 IRGen
#   逐字节一致——这是唯一能抓"系统性理解错了"的一条，前两条对它全盲。
CAN_SIR=0
if [ -x "$COMPILER" ] && [ -f "$TESTS/final_arm/functional/00_main.sy" ]; then
  if "$COMPILER" --emit=structured-ir "$TESTS/final_arm/functional/00_main.sy" \
        -o "$WORK/sir_probe.txt" >/dev/null 2>&1 && grep -q '^(Module' "$WORK/sir_probe.txt" 2>/dev/null; then
    CAN_SIR=1
  fi
fi
if [ "$CAN_SIR" = "0" ]; then
  c_skip "结构化 IR 跳过：--emit=structured-ir 尚未实现（S05 的交付物）"
else
  # ① 样例对：与仓库里那份**真实运行产出**的样例逐字节相同（前两关各栽过一次，
  #    都是靠这一条抓出来的——文字描述会漂，样例对不会）。
  # ⚠️ 样例对在 **compiler/tests/** 下（源码仓里），不在语言测试语料 `tests/` 下。
  #    第一版写成了 $TESTS（语料目录），于是关卡报"找不到样例对"——**误报**。
  EX3="$ROOT/compiler/tests/structured/example"
  if [ -f "$EX3/example.sy" ] && [ -f "$EX3/example.emit-structured.txt" ]; then
    if "$COMPILER" --emit=structured-ir "$EX3/example.sy" -o "$WORK/ex3.txt" >/dev/null 2>&1 \
       && cmp -s "$WORK/ex3.txt" "$EX3/example.emit-structured.txt"; then
      c_ok "结构化 IR：与样例对逐字节相同"
    else
      c_bad "结构化 IR 与样例对不一致（违反格式契约）"
      diff "$EX3/example.emit-structured.txt" "$WORK/ex3.txt" 2>/dev/null | head -8 | sed 's/^/      /'
    fi
    # 往返：dump → 读回 → 再 dump，必须逐字节相同（轨 A）
    if "$COMPILER" --from-structured --emit=structured-ir "$EX3/example.emit-structured.txt" \
          -o "$WORK/ex3b.txt" >/dev/null 2>&1 && cmp -s "$WORK/ex3.txt" "$WORK/ex3b.txt"; then
      c_ok "结构化 IR：dump→读回→再 dump 逐字节相同"
    else
      c_bad "结构化 IR 往返不同构（打印器与读取器不互逆）"
    fi
  else
    c_bad "找不到样例对 $EX3（S05 的交付物：真实运行产出的 dump + README）"
  fi
  # ② 结构 / 覆盖性 / 指令集封闭
  if [ -f "$TOOLS/selftest/check_structured.py" ]; then
    SLOG="$WORK/sir.log"
    if python3 "$TOOLS/selftest/check_structured.py" --compiler "$COMPILER" \
          --dir "$ROOT/tests" --jobs "$(nproc)" >"$SLOG" 2>&1; then
      c_ok "结构化 IR：不变式 + 覆盖性 + 指令集封闭"
      grep -E 'Op 数|覆盖|指令集' "$SLOG" | head -3 | sed 's/^/      /'
    else
      c_bad "结构化 IR 结构检查失败"
      grep -E '✘|违反|失败' "$SLOG" | head -8 | sed 's/^/      /'
    fi
  else
    c_skip "check_structured.py 未实现（S05 的交付物）"
  fi
  # ③ ★★ 独立实现的第二份 IRGen（唯一能抓"理解错了"的一条）
  if [ -f "$TOOLS/selftest/independent_irgen_check.py" ]; then
    ILOG="$WORK/iirgen.log"
    if [ "$QUICK" = 1 ]; then
      c_skip "轨 D（独立 IRGen）--quick 档跳过 —— **交付前请跑全量**"
    elif python3 "$TOOLS/selftest/independent_irgen_check.py" --compiler "$COMPILER" \
          --dir "$ROOT/tests" --jobs "$(nproc)" >"$ILOG" 2>&1; then
      c_ok "独立 IRGen 交叉验证：与 C++ 输出逐字节一致"
    else
      c_bad "独立 IRGen 交叉验证不一致（C++ 侧或独立侧有真 bug）"
      grep -E '不一致|mismatch|✘' "$ILOG" | head -8 | sed 's/^/      /'
    fi
  else
    c_skip "independent_irgen_check.py 未实现（S05 的交付物）"
  fi
  # ④ 金样例与最小对照
  if [ -f "$TOOLS/selftest/run_structured_cases.py" ]; then
    CLOG="$WORK/sircases.log"
    if python3 "$TOOLS/selftest/run_structured_cases.py" --compiler "$COMPILER" \
          >"$CLOG" 2>&1; then
      SUM=$(grep -E '通过|总计' "$CLOG" | tail -1)
      c_ok "结构化用例集：${SUM:-全过}"
    else
      c_bad "结构化用例集有失败"
      grep -E '✘|失败|FAIL' "$CLOG" | head -8 | sed 's/^/      /'
    fi
  else
    c_skip "run_structured_cases.py 未实现（S05 的交付物）"
  fi
fi

# ─────────────────────────────────────────────────────────────────────────────

return 0
