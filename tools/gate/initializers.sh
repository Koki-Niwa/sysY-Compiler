# Sourced by tools/verify.sh; shared state/contract: gate/README.md.
hdr "C5. 初始化计划自校验（若 --emit=initplan 已实现 —— S04 起）"
# ─────────────────────────────────────────────────────────────────────────────
# 这一关的产物没有可对齐的历史基线，所以关卡必须**自己**守住三件事：
#   结构（拼写/形态）、策略（零不物化、`a[4096]={1}` 动作数 ≤3）、规模（转储总量）。
# 语义正确性（填对没填对）由开发方的 check_initplan.py 负责——它独立实现一遍
# 初始化语义并模拟计划；关卡不重复那件事。
CAN_INITPLAN=0
if [ -x "$COMPILER" ] && [ -f "$TOOLS/selftest/check_initplan_format.py" ]; then
  FLOG="$WORK/initfmt.log"
  python3 "$TOOLS/selftest/check_initplan_format.py" --compiler "$COMPILER" \
      --root "$ROOT" --jobs "$(nproc)" >"$FLOG" 2>&1
  case $? in
    0) CAN_INITPLAN=1; c_ok "初始化计划：结构 / 策略 / 规模 $(grep -c . "$FLOG" >/dev/null && echo 合规)"
       grep -E '范围内用例|转储总量|单文件最大' "$FLOG" | sed 's/^/      /' ;;
    2) c_skip "初始化计划跳过：--emit=initplan 尚未实现" ;;
    *) c_bad "初始化计划违反格式 / 策略 / 规模预算"
       grep -E '✘|判定' "$FLOG" | head -8 | sed 's/^/      /' ;;
  esac
fi
  # ①b ★ 规范样例对：与仓库里那份**真实运行产出**的样例逐字节相同。
  #     §4.3 的示例原先是我手写的，与实际输出有三处排版差异（`:data` 一行一对、
  #     `StoreExpr` 折叠、`MemcpyConst` 收尾括号）—— 文字会漂，样例对不会。
  #     这与 C4 组里 `--emit=sema` 的做法完全一致。
  EX2="$TOOLS/selftest/initplan_example"
  if [ -f "$EX2/example.sy" ] && [ -f "$EX2/example.emit-initplan.txt" ]; then
    if "$COMPILER" --emit=initplan "$EX2/example.sy" -o "$WORK/ex2.txt" >/dev/null 2>&1 \
       && cmp -s "$WORK/ex2.txt" "$EX2/example.emit-initplan.txt"; then
      c_ok "初始化计划：与规范样例逐字节相同"
    else
      c_bad "初始化计划与规范样例不一致（违反 §4.3 的格式契约）"
      diff "$EX2/example.emit-initplan.txt" "$WORK/ex2.txt" 2>/dev/null | head -8 | sed 's/^/      /'
    fi
  fi
# 语义侧：开发方的独立检查器（自己实现初始化语义 + 模拟计划 + 与源码含义比对）
if [ "$CAN_INITPLAN" = "1" ]; then
  if [ -f "$TOOLS/selftest/check_initplan.py" ]; then
    ILOG="$WORK/initplan.log"
    if python3 "$TOOLS/selftest/check_initplan.py" --compiler "$COMPILER" \
          --jobs 4 >"$ILOG" 2>&1; then  # 大数组语料限制并发峰值内存
      c_ok "初始化语义：独立检查器模拟计划并与源码含义逐元素一致"
    else
      c_bad "初始化语义检查失败"
      grep -E '✘|违反|不一致' "$ILOG" | head -8 | sed 's/^/      /'
    fi
    # ★ 反证：人为改坏的转储必须逐条报红。与 C4 组同一条规矩——
    #   "检查器全过"只有在证明过它抓得住错之后才有意义（S01 的教训）。
    if python3 "$TOOLS/selftest/check_initplan.py" --compiler "$COMPILER" --probe \
          >"$WORK/initplan_probe.log" 2>&1; then
      c_ok "初始化检查器反证：人为改坏的转储被逐条报红"
    else
      c_bad "初始化检查器抓不住人为改坏 —— 它的\"全过\"不可信"
      grep -E '✘|没抓到|判定' "$WORK/initplan_probe.log" | head -6 | sed 's/^/      /'
    fi
  else
    c_skip "check_initplan.py 未实现（S04 的交付物）"
  fi
  if [ -f "$TOOLS/selftest/run_init_cases.py" ]; then
    CLOG="$WORK/initcases.log"
    if python3 "$TOOLS/selftest/run_init_cases.py" --compiler "$COMPILER" >"$CLOG" 2>&1; then
      SUM=$(grep -E '通过|总计' "$CLOG" | tail -1)
      c_ok "初始化金样例：${SUM:-全过}"
    else
      c_bad "初始化金样例有失败"
      grep -E '✘|失败|FAIL' "$CLOG" | head -8 | sed 's/^/      /'
    fi
  else
    c_skip "run_init_cases.py 未实现（S04 的交付物）"
  fi
fi

# ── 验证工具必须**能从源码运行** ──
#    S05 的独立代码生成器检查器曾经把 11 个方法只留在 `.pyc` 字节码里，靠 marshal
#    在运行时补回来。后果有三条，每一条都足以否掉那份验证：
#      ① **读不到** —— 我无法审计它到底比了什么；
#      ② **复现不了** —— 换 Python 版本 marshal 格式就变（它自己还写了按版本号找
#         `__pycache__` 的兜底，说明作者知道这一点）；
#      ③ **留不住** —— `.pyc` 没被 gitignore 覆盖，但也不该进源码仓；一次干净 clone
#         就再也跑不出那个"490/490"。
#    ⇒ 判据很简单：验证工具目录下**不许有 `.pyc`**，也不许 import marshal 去补代码。
if [ -d "$TOOLS/selftest" ]; then
  # ⚠️ 只查**故意放进去的** .pyc：`__pycache__/` 里的是解释器自动生成的缓存
  #    （且已被 gitignore 覆盖），它不影响可复现性。真正的信号是
  #    "工具目录下躺着一个源码之外的 .pyc，且代码用 marshal 去读它"。
  PYC=$(find "$TOOLS/selftest" -name '*.pyc' -not -path '*/__pycache__/*' 2>/dev/null | head -5)
  #  ⚠️ 判据必须查**真实用法**，不能查词：有脚本在**文档字符串**里写着"不生成
  #     .pyc/marshal"（那是声明，不是使用）。这已是同一类误报的第三次
  #     （前两次：`.ll` 注释里的 target-features、源码注释里的 tests/ 路径）。
  #     危险用法只有两种：`import marshal` 与 `marshal.load/loads(...)`。
  #     **判据假红会把矛头指向正确的代码**（TESTING-GUIDE §11.5）。
  MARSHAL=$(grep -lE '^[[:space:]]*import[[:space:]]+marshal|marshal[[:space:]]*\.[[:space:]]*(loads|load)[[:space:]]*\(' \
              "$TOOLS"/selftest/*.py 2>/dev/null | head -3)
  if [ -z "$PYC" ] && [ -z "$MARSHAL" ]; then
    c_ok "验证工具可从源码运行（无 .pyc、无 marshal 补代码）"
  else
    c_bad "验证工具依赖编译产物，无法从源码复现"
    [ -n "$PYC" ] && echo "$PYC" | sed 's/^/      .pyc: /'
    [ -n "$MARSHAL" ] && echo "$MARSHAL" | sed 's/^/      marshal: /'
  fi
fi

# ─────────────────────────────────────────────────────────────────────────────

return 0
