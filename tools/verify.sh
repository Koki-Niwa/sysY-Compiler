#!/usr/bin/env bash
# ============================================================================
# verify.sh —— 会话交付质量关卡（由【用户】运行，不由开发 agent 运行）
#
# 设计意图：
#   开发 agent 的报告是"自述"，本脚本是"事实核查"。两者独立，才能发现虚报与回归。
#   agent 看不到本脚本的内容，所以它无法针对本脚本做应付。
#
# 用法：
#   bash compiler/tools/verify.sh              # 自动探测当前能力，跑到能跑的程度
#   bash compiler/tools/verify.sh --stage S11  # 指定阶段，启用该阶段的硬门禁
#
# 退出码：0 = 全部通过；1 = 有失败项
# ============================================================================
set -uo pipefail

ROOT="/home/koki1/try"
COMPILER="$ROOT/compiler/build/compiler"
TESTS="$ROOT/tests"
RUNTIME="$ROOT/runtime"
TOOLS="$ROOT/compiler/tools"
LLVM_BIN="/usr/lib/llvm-18/bin"
WORK="${TMPDIR:-/tmp}/verify_$$"
STAGE=""
FAIL=0
PASS=0
SKIP=0

mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

while [ $# -gt 0 ]; do
  case "$1" in
    --stage) STAGE="$2"; shift 2 ;;
    *) echo "未知参数: $1"; exit 2 ;;
  esac
done

c_ok()   { printf '  \033[32m✔\033[0m %s\n' "$1"; PASS=$((PASS+1)); }
c_bad()  { printf '  \033[31m✘\033[0m %s\n' "$1"; FAIL=$((FAIL+1)); }
c_skip() { printf '  \033[33m−\033[0m %s\n' "$1"; SKIP=$((SKIP+1)); }
hdr()    { printf '\n\033[1m═══ %s ═══\033[0m\n' "$1"; }

echo "════════════════════════════════════════════════════════════════"
echo " 会话交付质量关卡    $(date '+%Y-%m-%d %H:%M:%S')   阶段=${STAGE:-自动}"
echo "════════════════════════════════════════════════════════════════"

# ─────────────────────────────────────────────────────────────────────────────
hdr "A. 构建（零警告是硬要求）"
# ─────────────────────────────────────────────────────────────────────────────
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
hdr "B. 合规自查（对应铁律 2/3/4 —— 违反会被取消资格）"
# ─────────────────────────────────────────────────────────────────────────────
if [ -d "$ROOT/compiler/src" ]; then
  EXT=$(grep -rnE '\b(system|popen|execv?p?|fork)\s*\(' "$ROOT/compiler/src" 2>/dev/null | grep -v '^\s*//' || true)
  [ -z "$EXT" ] && c_ok "编译器本体没有调用外部程序" || { c_bad "发现外部进程调用"; echo "$EXT" | head -3 | sed 's/^/      /'; }

  # 识别函数名/用例名做特判（运行时库签名表允许出现这些名字，故排除表所在文件）
  HARD=$(grep -rniE '"(getint|getch|getfloat|getarray|getfarray|putint|putch|putfloat|putarray|putfarray|putf|matmul|transpose|fft|shuffle|crypto|huffman|sl1)"' \
           "$ROOT/compiler/src" 2>/dev/null | grep -viE 'RuntimeLib|runtime_lib|RuntimeLibrary|Signature' || true)
  [ -z "$HARD" ] && c_ok "没有识别函数名/用例名的特判" || { c_bad "疑似特判"; echo "$HARD" | head -3 | sed 's/^/      /'; }

  # ⚠️ 只检查【代码】，不检查【注释】。
  #    这条规则的目的是"代码里不许读用例路径 / 拿用例名做特判"；注释做不到这件事。
  #    而源码注释里写清楚"这个数字实测出自哪个用例"是**正常的工程记录**
  #    （例如"真实语料 tests/.../86_long_code2.sy 的打印树深是 4007"）。
  #    做法：剥掉行尾 `//` 注释，丢掉以 `*` 或 `/*` 开头的块注释续行，再匹配。
  #    （与前面 target-features 的处理同一个道理：关卡自己也会误报，误报要修关卡，不是改代码。）
  CASE=$(grep -rnE '\.(sy|in|out)"|tests/' "$ROOT/compiler/src" 2>/dev/null \
         | sed -E 's@//.*$@@' \
         | grep -vE ':[0-9]+:[[:space:]]*(/\*|\*)' \
         | grep -E '\.(sy|in|out)"|tests/' || true)
  [ -z "$CASE" ] && c_ok "没有引用测试用例路径" || { c_bad "引用了测试用例"; echo "$CASE" | head -3 | sed 's/^/      /'; }
else
  c_skip "compiler/src 不存在"
fi

# ─────────────────────────────────────────────────────────────────────────────
# ── 能力探测：能否产出【真实】IR（至少含一个函数定义）──
#    判据不写死阶段号，而是探测能力。IRGen 在 S05–S07 之间落地；落地之前，
#    C 组的 IR 检查全部是【空过】—— 对着一份纯注释的占位 .ll，llvm-as 会成功，
#    各种 grep 也一律干净，于是关卡给出 7 个假的 ✔（S02 实测踩到这个"假绿"）。
#    宁可显式跳过，也不要假绿：假绿会掩盖真问题，比红灯更危险。
CAN_IR=0
if [ -x "$COMPILER" ]; then
  PROBE_SY="$TESTS/final_arm/functional/00_main.sy"
  if [ -f "$PROBE_SY" ] && "$COMPILER" "$PROBE_SY" -o "$WORK/gate.ll" >/dev/null 2>&1; then
    grep -q '^define ' "$WORK/gate.ll" && CAN_IR=1
  fi
fi

# ─────────────────────────────────────────────────────────────────────────────
hdr "C. 基础功能（编译 → 跑 → 比对 .out）"
# ─────────────────────────────────────────────────────────────────────────────
if [ ! -x "$COMPILER" ]; then
  c_skip "编译器未构建，跳过功能验证"
elif [ "$CAN_IR" = "0" ]; then
  c_skip "C 组跳过：编译器尚未产出真实 IR（无 'define'）—— IRGen 在 S05–S07"
else
  # 取一个最简单的用例探路
  PROBE="$TESTS/final_arm/functional/00_main.sy"
  if [ ! -f "$PROBE" ]; then
    c_skip "找不到探路用例"
  else
    if "$COMPILER" "$PROBE" -o "$WORK/probe.ll" >"$WORK/c.log" 2>&1; then
      c_ok "编译器能产出 .ll"
      if [ -x "$LLVM_BIN/llvm-as" ]; then
        if "$LLVM_BIN/llvm-as" "$WORK/probe.ll" -o /dev/null 2>"$WORK/as.log"; then
          c_ok "llvm-as 通过（IR 合法）"
          # 指令集封闭性检查
          if [ -f "$ROOT/docs/handoff/iset.txt" ]; then
            ALLOWED=$(grep -v '^#' "$ROOT/docs/handoff/iset.txt" | tr ' ' '\n' | grep -v '^$' | sort -u)
            USED=$(grep -oE '^\s+%?[A-Za-z0-9_.]+\s*=\s*(tail\s+)?[a-z][a-z0-9.]*' "$WORK/probe.ll" \
                   | grep -oE '[a-z][a-z0-9.]*$' | sort -u)
            TERM=$(grep -oE '^\s+(ret|br|unreachable|call)\b' "$WORK/probe.ll" | awk '{print $1}' | sort -u)
            BAD=$(printf '%s\n%s\n' "$USED" "$TERM" | sort -u | grep -v '^$' | while read -r i; do
                    echo "$ALLOWED" | grep -qx "$i" || echo "$i"; done | tr '\n' ' ')
            [ -z "$BAD" ] && c_ok "指令集封闭（无声明外指令）" || c_bad "出现声明外指令: $BAD"
          fi
          # alloca 位置检查（不变量 2）
          if grep -q 'alloca' "$WORK/probe.ll"; then
            BADALLOC=$(awk '/^define/{fn=1;blk=0} /^[a-zA-Z_.][^:]*:$/{blk++} /alloca/{if(blk>1) print FILENAME": "$0}' "$WORK/probe.ll" | head -3)
            [ -z "$BADALLOC" ] && c_ok "alloca 全在入口块" || { c_bad "alloca 出现在非入口块"; echo "$BADALLOC" | sed 's/^/      /'; }
          fi
          # 禁止项
          # 注意：只检查【非注释行】（LLVM 注释以 ; 开头）——避免误报文档性注释
          NOWS=$(grep -v '^\s*;' "$WORK/probe.ll")
          echo "$NOWS" | grep -qE '^target (triple|datalayout)|"target-(cpu|features)"' \
            && c_bad "出现了 target 信息（契约要求目标无关）" || c_ok "目标无关（无 triple/datalayout/target-*）"
          echo "$NOWS" | grep -qE '\binbounds\b' && c_bad "出现了 inbounds（策略是默认不加）" || c_ok "无 inbounds"
          echo "$NOWS" | grep -qE '\b(nsw|nuw)\b'  && c_bad "出现 nsw/nuw（违反 D6）" || c_ok "无 nsw/nuw"
          echo "$NOWS" | grep -qE '\bfcmp\s+one\b' && c_bad "fcmp 用了 one（应为 une）" || c_ok "fcmp 谓词正确"
        else
          c_bad "llvm-as 失败（IR 非法）"; head -5 "$WORK/as.log" | sed 's/^/      /'
        fi
      else
        c_skip "llvm-as 不可用"
      fi
    else
      c_bad "编译器执行失败"; tail -10 "$WORK/c.log" | sed 's/^/      /'
    fi
  fi
fi

# ─────────────────────────────────────────────────────────────────────────────
hdr "C2. 词法自校验（若 check_lexer.py 已实现 —— S01 起）"
# ─────────────────────────────────────────────────────────────────────────────
if [ -x "$TOOLS/selftest/check_lexer.py" ] && [ -x "$COMPILER" ]; then
  LOG="$WORK/lex.log"
  if python3 "$TOOLS/selftest/check_lexer.py" --compiler "$COMPILER" \
        --jobs "$(nproc)" >"$LOG" 2>&1; then
    SUM=$(grep -E '不变式成立|范围内验证' "$LOG" | tr -s ' ' | tr '\n' ' ')
    c_ok "词法自校验：不变式成立 ${SUM:-通过}"
  else
    c_bad "词法自校验失败（词法器有 bug —— 拼接对不上原文）"
    grep -E '失败|首处差异|✘' "$LOG" | head -8 | sed 's/^/      /'
  fi
  # 附带：540 个文件在 --emit=tokens 下不崩（退出码 0 或 1 均可，但不能是信号/超时）
  CRASH=0
  for f in $(find "$ROOT/tests" -name '*.sy' 2>/dev/null); do
    # 超时护栏：词法器一旦死循环（正是"Invalid 必须前进"那条规则在防的失效模式），
    # 关卡会永久挂住而不是报红。rc=124 表示超时。
    timeout 20 "$COMPILER" "$f" --emit=tokens -o /dev/null >/dev/null 2>&1
    rc=$?
    [ "$rc" -gt 1 ] && CRASH=$((CRASH+1))
  done
  [ "$CRASH" = "0" ] && c_ok "540 个 .sy 在 --emit=tokens 下无一崩溃" \
                     || c_bad "$CRASH 个文件在 --emit=tokens 下异常退出（>1 或信号）"
else
  c_skip "check_lexer.py 未实现（S01 的交付物）"
fi

# ─────────────────────────────────────────────────────────────────────────────
hdr "C3. 语法自校验（若 check_parser.py 已实现 —— S02 起）"
# ─────────────────────────────────────────────────────────────────────────────
if [ -x "$TOOLS/selftest/check_parser.py" ] && [ -x "$COMPILER" ]; then
  LOG="$WORK/parse.log"
  if python3 "$TOOLS/selftest/check_parser.py" --compiler "$COMPILER" \
        --jobs "$(nproc)" >"$LOG" 2>&1; then
    SUM=$(grep -E '范围内往返成功|范围外已报诊断' "$LOG" | tr -s ' ' | tr '\n' ' ')
    c_ok "语法自校验：AST 往返逐字节相同 ${SUM:-通过}"
  else
    c_bad "语法自校验失败（打印器/读取器不互逆，或解析器丢信息）"
    grep -E '失败|不一致|✘|首处' "$LOG" | head -8 | sed 's/^/      /'
  fi
  # 独立实现交叉验证（若存在）：与 C++ 完全无关的第二份 parser 必须产出同一份 AST
  if [ -x "$TOOLS/selftest/independent_parser_check.py" ]; then
    ILOG="$WORK/iparse.log"
    if python3 "$TOOLS/selftest/independent_parser_check.py" --compiler "$COMPILER" \
          --jobs "$(nproc)" >"$ILOG" 2>&1; then
      c_ok "独立 parser 交叉验证：与 C++ 输出逐字节一致"
    else
      c_bad "独立 parser 交叉验证不一致（C++ 侧或独立侧有真 bug）"
      grep -E '不一致|mismatch|✘' "$ILOG" | head -8 | sed 's/^/      /'
    fi
  fi
  # ★ 冻结基线的指纹比对：`--emit=ast` 是对外契约，新阶段只许做加法。
  #   基线由编排方在阶段开始前生成并进版本库 —— 开发 agent 无法"改完再生成基线"。
  if [ -f "$TOOLS/selftest/baseline_ast_sha256.txt" ]; then
    BLOG="$WORK/astbase.log"
    if python3 "$TOOLS/selftest/check_ast_baseline.py" --compiler "$COMPILER" \
          --jobs "$(nproc)" >"$BLOG" 2>&1; then
      c_ok "AST 冻结基线：490 个文件逐字节不变"
    else
      c_bad "AST 输出偏离冻结基线（新阶段只能做加法，不许改已有格式）"
      grep -E '✘|逐字节相同' "$BLOG" | head -8 | sed 's/^/      /'
    fi
  fi
else
  c_skip "check_parser.py 未实现（S02 的交付物）"
fi

# ─────────────────────────────────────────────────────────────────────────────
hdr "D. 全量回归（若 run_tests.sh 已实现）"
# ─────────────────────────────────────────────────────────────────────────────
CAN_E2E="$CAN_IR"

if [ -x "$TOOLS/run_tests.sh" ] && [ "$CAN_E2E" = "0" ]; then
  c_skip "端到端回归跳过：编译器尚未产出真实 IR（无 'define'）—— IRGen 在 S05–S07"
elif [ -x "$TOOLS/run_tests.sh" ]; then
  for T in x86 aarch64 riscv64; do
    case "$T" in
      aarch64) command -v qemu-aarch64 >/dev/null || { c_skip "$T：无 qemu"; continue; } ;;
      riscv64) command -v qemu-riscv64 >/dev/null || { c_skip "$T：无 qemu"; continue; } ;;
    esac
    LOG="$WORK/rt_$T.log"
    if bash "$TOOLS/run_tests.sh" --target "$T" --jobs "$(nproc)" >"$LOG" 2>&1; then
      SUM=$(grep -E '^总计' "$LOG" | tail -1)
      c_ok "回归 $T：${SUM:-通过}"
    else
      SUM=$(grep -E '^总计' "$LOG" | tail -1)
      c_bad "回归 $T 有失败：${SUM:-见日志}"
      grep -A 15 '失败的用例' "$LOG" | head -12 | sed 's/^/      /'
    fi
  done
  # 结构化层开关一致性
  if bash "$TOOLS/run_tests.sh" --no-structured --target x86 --jobs "$(nproc)" >"$WORK/rt_ns.log" 2>&1; then
    c_ok "回归 --no-structured：通过"
  else
    c_bad "回归 --no-structured 有失败"
  fi
else
  c_skip "run_tests.sh 未实现（S00 的交付物之一）"
fi

# ─────────────────────────────────────────────────────────────────────────────
hdr "E. 契约验证（若玩具后端已实现 —— S11b）"
# ─────────────────────────────────────────────────────────────────────────────
if [ -x "$TOOLS/toy_backend" ] && [ "$CAN_E2E" = "1" ]; then
  if bash "$TOOLS/run_tests.sh" --toy-backend --target x86 --jobs "$(nproc)" >"$WORK/rt_toy.log" 2>&1; then
    c_ok "经【自研降级器】回归通过（契约真的够后端用）"
  else
    c_bad "自研降级器路径失败 —— 契约有缺口"
    grep -A 15 '失败的用例' "$WORK/rt_toy.log" | head -12 | sed 's/^/      /'
  fi
else
  c_skip "toy_backend 未实现（S11b 的交付物）"
fi

# ─────────────────────────────────────────────────────────────────────────────
hdr "F. 阶段硬门禁"
# ─────────────────────────────────────────────────────────────────────────────
case "$STAGE" in
  S11|S11b|S1[2-9]|S2[0-8])
    echo "  本阶段要求：所有回归 100% 通过（490 个用例输出与 .out 逐字节相同）"
    if [ "$FAIL" != "0" ]; then c_bad "有失败项 → 未达 S11 之后的硬门禁"; else c_ok "硬门禁满足"; fi
    ;;
  "") echo "  （未指定 --stage，跳过阶段门禁）" ;;
  *)  echo "  阶段 $STAGE 的门禁：人工对照 SESSION-PLAN.md 的验收列" ;;
esac

# ─────────────────────────────────────────────────────────────────────────────
echo
echo "════════════════════════════════════════════════════════════════"
printf " 结果：\033[32m%d 通过\033[0m  \033[31m%d 失败\033[0m  \033[33m%d 跳过\033[0m\n" "$PASS" "$FAIL" "$SKIP"
if [ "$FAIL" = "0" ]; then
  echo " 判定：✔ 可以进入下一个会话"
else
  echo " 判定：✘ 【不要进入下一个会话】—— 把上面的红项连同 agent 的报告一起贴给我"
fi
echo "════════════════════════════════════════════════════════════════"
exit $([ "$FAIL" = "0" ] && echo 0 || echo 1)
