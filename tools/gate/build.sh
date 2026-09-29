# Sourced by tools/verify.sh; shared state/contract: gate/README.md.
if [ ! -f "$ROOT/compiler/CMakeLists.txt" ]; then
  c_skip "尚无 CMakeLists.txt —— 【还没开工】（S00 完成后本项应通过）"
else
  BUILD_LOG="$WORK/build.log"
  if cmake -S "$ROOT/compiler" -B "$ROOT/compiler/build" -G Ninja >"$BUILD_LOG" 2>&1 \
     && cmake --build "$ROOT/compiler/build" >>"$BUILD_LOG" 2>&1; then
    c_ok "cmake --build 退出码 0"
    WARN=$(grep -ciE '\bwarning:' "$BUILD_LOG" || true)
    if [ "$WARN" = "0" ]; then c_ok "零警告"
    else c_bad "有 $WARN 条编译警告（要求零警告）"; grep -iE '\bwarning:' "$BUILD_LOG" | head -5 | sed 's/^/      /'; fi
  else
    c_bad "构建失败"; tail -20 "$BUILD_LOG" | sed 's/^/      /'
  fi
fi

# ── 代码规模（SESSION-PLAN §C4）──
#    ⚠️ 这条标准以前只写在文档里、**从来没有被检查过**，于是它既没被遵守也没被拦下
#    （S02 收尾时 AstPrinter.cpp 1135 行、Parser.cpp 1010 行）。
#    "写了标准但不检查"比没有标准更糟：它教会后面每个会话无视这一节。
# ── 单元断言 ──
#    ⚠️ 关卡一直**没有**跑单元测试：改一条语义规则时，`run.sh` 里两处断言先红了，
#    而关卡照样 20 通过 —— 因为没人调它。凡是"标准要求、关卡不查"的东西，
#    迟早会漂移（这条与 C5 的成因是同一个）。
if [ -f "$TOOLS/selftest/unit/run.sh" ]; then
  ULOG="$WORK/unit.log"
  if bash "$TOOLS/selftest/unit/run.sh" >"$ULOG" 2>&1; then
    SUM=$(grep -oE '检查项 [0-9]+ 个' "$ULOG" | tr '\n' ' ')
    c_ok "单元断言：全部通过 ${SUM:-}"
  else
    c_bad "单元断言有失败"
    grep -E '✘|X ' "$ULOG" | head -8 | sed 's/^/      /'
  fi
fi

# ── 内存预算 ──
#    "编译器的内存开销不得随源码里的一个数字爆炸"。语料的**正确性**判据看不见这类缺陷
#    （490 个用例全绿、诊断全对），它会一直藏到现场赛的一个大数组上才 OOM。
#    判据是几个**压力文件**的实测峰值 RSS —— 见 check_mem_budget.py 里每一行的依据。
if [ -f "$TOOLS/selftest/check_mem_budget.py" ] && [ -x "$COMPILER" ]; then
  MLOG="$WORK/mem.log"
  if python3 "$TOOLS/selftest/check_mem_budget.py" --compiler "$COMPILER" \
        --root "$ROOT" >"$MLOG" 2>&1; then
    c_ok "内存预算：压力文件峰值 RSS 全部在预算内"
  else
    c_bad "有压力文件超出内存预算（内存开销随源码数字爆炸）"
    grep -E '✘|依据' "$MLOG" | head -6 | sed 's/^/      /'
  fi
fi

if [ -f "$TOOLS/selftest/check_line_budget.py" ]; then
  LLOG="$WORK/lines.log"
  if python3 "$TOOLS/selftest/check_line_budget.py" --root "$ROOT" >"$LLOG" 2>&1; then
    c_ok "行数预算（§C4）：产品代码 ≤ 600 行 / 工具脚本棘轮"
  else
    c_bad "违反 §C4 的行数预算（拆分，或由编排方明确改标准）"
    grep -E '✘' "$LLOG" | head -6 | sed 's/^/      /'
  fi
fi

# ─────────────────────────────────────────────────────────────────────────────

return 0
