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
# ── 单元断言 ──
#    ⚠️ 关卡一直**没有**跑单元测试：改一条语义规则时，`run.sh` 里两处断言先红了，
#    而关卡照样 20 通过 —— 因为没人调它。凡是"标准要求、关卡不查"的东西，
#    迟早会漂移（这条与 C5 的成因是同一个）。
if [ -x "$TOOLS/selftest/unit/run.sh" ]; then
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
if [ -x "$TOOLS/selftest/check_mem_budget.py" ] && [ -x "$COMPILER" ]; then
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
hdr "C4. 语义自校验（若 --emit=sema 已实现 —— S03 起）"
# ─────────────────────────────────────────────────────────────────────────────
# ⚠️ 探测判据不能只看"退出码 0"：现阶段 `--emit=sema` 会被当成未知取值而**回落到默认转储**，
#    于是探测会误判成"已实现"，接着拿纯 AST 去比注解样例、报出假红。
#    真正的能力信号是**输出里有没有 `(RuntimeLib` 头**（S03 格式契约的第一行）。
CAN_SEMA=0
if [ -x "$COMPILER" ] && [ -f "$TESTS/final_arm/functional/00_main.sy" ]; then
  if "$COMPILER" --emit=sema "$TESTS/final_arm/functional/00_main.sy" -o "$WORK/sema_probe.txt" \
        >/dev/null 2>&1 && grep -q '^(RuntimeLib' "$WORK/sema_probe.txt" 2>/dev/null; then
    CAN_SEMA=1
  fi
fi
if [ "$CAN_SEMA" = "0" ]; then
  c_skip "语义转储跳过：--emit=sema 尚未实现（S03 的交付物）"
else
  # ① 规范样例对：转储必须与仓库里那份"已验证过能还原"的样例逐字节相同。
  #    这是 §五 格式契约的可执行版本 —— 纯文字描述产生过歧义（记号该放前面还是后面、
  #    下标换不换行），样例把歧义消掉了。
  EX="$TOOLS/selftest/sema_dump_example"
  if [ -f "$EX/example.sy" ] && [ -f "$EX/example.emit-sema.txt" ]; then
    if "$COMPILER" --emit=sema "$EX/example.sy" -o "$WORK/ex.sema" >/dev/null 2>&1 \
       && cmp -s "$WORK/ex.sema" "$EX/example.emit-sema.txt"; then
      c_ok "语义转储：与规范样例逐字节相同"
    else
      c_bad "语义转储与规范样例不一致（违反 §五 的格式契约）"
      diff "$EX/example.emit-sema.txt" "$WORK/ex.sema" 2>/dev/null | head -8 | sed 's/^/      /'
    fi
  fi
  # ② 独立不变式检查器（轨 A 零误报 + 去注解还原 + 16 条类型不变式，一次扫描）
  if [ -x "$TOOLS/selftest/check_sema.py" ]; then
    SLOG="$WORK/sema.log"
    if python3 "$TOOLS/selftest/check_sema.py" --compiler "$COMPILER" \
          --jobs "$(nproc)" >"$SLOG" 2>&1; then
      c_ok "语义自校验：零误报 + 去注解还原 + 类型不变式"
    else
      c_bad "语义自校验失败（零误报 / 还原 / 类型不变式 三者之一）"
      grep -E '✘|违反|不一致|误报' "$SLOG" | head -8 | sed 's/^/      /'
    fi
    # ②b ★ 证明检查器不是"永远说 OK"：四处**定向人为破坏**必须逐条报红。
    #     这是 S01 留下的教训 —— 自校验脚本必须先证明它抓得住错，
    #     否则"490 个文件全过"可能只是因为它什么都没查。
    if [ -x "$TOOLS/selftest/check_sema_probe.py" ]; then
      if python3 "$TOOLS/selftest/check_sema_probe.py" >"$WORK/semaprobe.log" 2>&1; then
        c_ok "语义检查器反证：四处人为破坏被逐条报红（不是永远说 OK）"
      else
        c_bad "语义检查器抓不住人为破坏 —— 它的\"全过\"不可信"
        grep -E '✘|没抓到|判定' "$WORK/semaprobe.log" | head -6 | sed 's/^/      /'
      fi
    fi
  else
    c_skip "check_sema.py 未实现（S03 的交付物）"
  fi
  # ③ 最小对照用例集
  if [ -x "$TOOLS/selftest/run_sema_cases.py" ]; then
    CLOG="$WORK/semacases.log"
    if python3 "$TOOLS/selftest/run_sema_cases.py" --compiler "$COMPILER" >"$CLOG" 2>&1; then
      SUM=$(grep -E '通过|总计' "$CLOG" | tail -1)
      c_ok "最小对照用例集：${SUM:-全过}"
    else
      c_bad "最小对照用例集有失败"
      grep -E '✘|失败|FAIL' "$CLOG" | head -8 | sed 's/^/      /'
    fi
  else
    c_skip "run_sema_cases.py 未实现（S03 的交付物）"
  fi
fi

# ─────────────────────────────────────────────────────────────────────────────
hdr "C5. 初始化计划自校验（若 --emit=initplan 已实现 —— S04 起）"
# ─────────────────────────────────────────────────────────────────────────────
# 这一关的产物没有可对齐的历史基线，所以关卡必须**自己**守住三件事：
#   结构（拼写/形态）、策略（零不物化、`a[4096]={1}` 动作数 ≤3）、规模（转储总量）。
# 语义正确性（填对没填对）由开发方的 check_initplan.py 负责——它独立实现一遍
# 初始化语义并模拟计划；关卡不重复那件事。
CAN_INITPLAN=0
if [ -x "$COMPILER" ] && [ -x "$TOOLS/selftest/check_initplan_format.py" ]; then
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
  if [ -x "$TOOLS/selftest/check_initplan.py" ]; then
    ILOG="$WORK/initplan.log"
    if python3 "$TOOLS/selftest/check_initplan.py" --compiler "$COMPILER" \
          --jobs "$(nproc)" >"$ILOG" 2>&1; then
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
  if [ -x "$TOOLS/selftest/run_init_cases.py" ]; then
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

# ─────────────────────────────────────────────────────────────────────────────
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
  if [ -x "$TOOLS/selftest/check_structured.py" ]; then
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
  if [ -x "$TOOLS/selftest/independent_irgen_check.py" ]; then
    ILOG="$WORK/iirgen.log"
    if python3 "$TOOLS/selftest/independent_irgen_check.py" --compiler "$COMPILER" \
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
  if [ -x "$TOOLS/selftest/run_structured_cases.py" ]; then
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
# ★ 能力门控有个洞：C4/C5 在能力不具备时会**跳过**，于是"阶段要求的能力还没做"
#   反而让关卡全绿通过。S03/S04 起，各阶段**要求**的能力一旦缺失就是硬失败。
NEED=""
case "$STAGE" in
  S03) NEED="sema" ;;
  S04) NEED="sema initplan" ;;
  S05|S0[6-9]|S1[0-9]|S2[0-8]) NEED="sema initplan structured-ir" ;;
esac
if [ -n "$NEED" ]; then
  case "$NEED" in *sema*)     [ "$CAN_SEMA" = "1" ]     || c_bad "本阶段要求 --emit=sema，但它没实现/没通过";; esac
  case "$NEED" in *initplan*) [ "$CAN_INITPLAN" = "1" ] || c_bad "本阶段要求 --emit=initplan，但它没实现/没通过";; esac
  case "$NEED" in *structured-ir*) [ "$CAN_SIR" = "1" ] || c_bad "本阶段要求 --emit=structured-ir，但它没实现/没通过";; esac
fi

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
