#!/usr/bin/env bash
# ============================================================================
# selftest/run.sh —— ★★ 自检链路（P00 prompt §4.7，本阶段的核心交付物）
#
# 此时编译器还产不出 .ll，但 "编译 → 清 IR → llvm-as → llc(三种 target) →
# 链接 → 运行 → 比对 .out" 这条链路【必须现在就验证】，否则后面 31 个会话
# 都建在未验证的地基上。
#
# 做法：链路接受任意 .ll 作为输入，不依赖编译器 ——
#   clang -S -emit-llvm -O0 -fno-addrsig probe.c -o probe.ll
#   # 把它当作 compiler 的产物，走 §4.4 的完整链路（三种 target）
#
# 通过标准：输入 21 → stdout "42\n"、退出码 7，按 C0 规则完整输出 "42\n7\n"。
#
# 用法: bash compiler/tools/selftest/run.sh [--target x86|aarch64|riscv64|all]
#                                          [--timeout 60] [--keep]
# ============================================================================
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
TOOLS="$(cd "$HERE/.." && pwd)"
LLVM_BIN="${LLVM_BIN:-/usr/lib/llvm-18/bin}"

TARGETS="all"; TIMEOUT=60; KEEP=0; WORK=""
while [ $# -gt 0 ]; do
  case "$1" in
    --target)  TARGETS="$2"; shift 2 ;;
    --timeout) TIMEOUT="$2"; shift 2 ;;
    --llvm-bin) LLVM_BIN="$2"; shift 2 ;;
    --work)    WORK="$2"; shift 2 ;;
    --keep)    KEEP=1; shift ;;
    -h|--help) sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "未知参数: $1" >&2; exit 2 ;;
  esac
done

# 工具检查（缺一个就明确报出来，不要等到半路失败）
need() { command -v "$1" >/dev/null || { echo "[selftest] 缺少 $1" >&2; exit 2; }; }
need clang
[ -x "$LLVM_BIN/llvm-as" ] || { echo "[selftest] 缺少 $LLVM_BIN/llvm-as" >&2; exit 2; }
[ -x "$LLVM_BIN/llc" ]     || { echo "[selftest] 缺少 $LLVM_BIN/llc" >&2; exit 2; }
if [ "$TARGETS" = "all" ] || [ "$TARGETS" = "aarch64" ]; then
  need aarch64-linux-gnu-gcc; need qemu-aarch64
fi
if [ "$TARGETS" = "all" ] || [ "$TARGETS" = "riscv64" ]; then
  need riscv64-linux-gnu-gcc; need qemu-riscv64
fi

[ -n "$WORK" ] || WORK="$(mktemp -d /tmp/sysy_selftest_XXXXXX)"
mkdir -p "$WORK"
echo "════════════════════════════════════════════════════════════"
echo " 自检链路 selftest    $(date '+%Y-%m-%d %H:%M:%S')"
echo " 工作目录: $WORK"
echo "════════════════════════════════════════════════════════════"

# ── 步骤 0：造一个"假"的编译器输出（raw.ll → clean.ll）─────────────────────
# -fno-addrsig 不能忘：本机 binutils 2.42 不认 .addrsig（clang 18 默认发射）
clang -S -emit-llvm -O0 -fno-addrsig "$HERE/probe.c" -o "$WORK/raw.ll" || {
  echo "[selftest] clang 生成 probe.ll 失败" >&2; exit 1; }

# 第 1 步：得到"干净"的 IR —— 无 triple/datalayout、无 target-cpu/features
#        （自检时用 sed 清；S07 之后我们的编译器本来就不发射这些）
sed -E 's/ "target-cpu"="[^"]*"//; s/ "target-features"="[^"]*"//; s/ "tune-cpu"="[^"]*"//' \
    "$WORK/raw.ll" | grep -v '^target triple\|^target datalayout' > "$WORK/case.ll"

echo
echo "── 准备：raw.ll 与 clean.ll 的差异（这就是"目标无关 IR"契约的机械含义）──"
grep -E '^target |target-cpu|target-features' "$WORK/raw.ll" | sed 's/^/  raw: /' | head -4
echo "  clean.ll 里 target/triple/features 出现次数: $(grep -cE '^target |target-cpu|target-features' "$WORK/case.ll" || true)"

# ── 步骤 1：准备用例清单（复用 run_tests.sh 的 ②→⑤ 链路，跳过 ①）──────────
printf '21\n' > "$WORK/probe.in"          # 输入 21 → 期望 stdout "42\n"、退出码 7
printf '42\n7\n' > "$WORK/probe.out"      # C0 规则：stdout + main 返回值各占一行
CASES="$WORK/cases.tsv"
# 注意：printf 的格式串里必须是真正的 TAB（"\t"），写成 "\\t" 会输出字面量
printf 'selftest\tchain\tprobe\t%s\t%s\t%s\t-\n' \
       "$WORK/case.ll" "$WORK/probe.in" "$WORK/probe.out" > "$CASES"
echo "  用例清单（7 列 TAB 分隔）:"
sed 's/\t/ | /g; s/^/    /' "$CASES"

# ── 步骤 2：对每种 target 走完整链路 ────────────────────────────────────────
if [ "$TARGETS" = "all" ]; then TARGET_LIST="x86 aarch64 riscv64"; else TARGET_LIST="$TARGETS"; fi

FAIL=0
for T in $TARGET_LIST; do
  LOG="$WORK/run_$T.log"
  echo
  echo "════════ target: $T ════════"
  python3 "$TOOLS/_run_tests.py" --no-compile-step --compiler /nonexistent/compiler \
      --manifest "$CASES" --root "$ROOT" --target "$T" --jobs 1 \
      --timeout "$TIMEOUT" --work "$WORK/work_$T" --verbose > "$LOG" 2>&1
  rc=$?
  # 打印每个用例的判定 + 真实产物行数（行数随 clang 版本/选项变化，仅供参考，不是判据）
  grep -E '^  \[' "$LOG" | sed 's/^/  /' || true
  S_LINES=$( [ -f "$WORK/work_$T/0000/case.s" ] && grep -c '' "$WORK/work_$T/0000/case.s" || echo 0 )
  echo "  case.s 行数: $S_LINES"

  # 真实运行一次，把 stdout 与退出码原样贴出来（报告要求"真实命令与输出"）
  case "$T" in
    x86)     RUNNER="" ;;
    aarch64) RUNNER="qemu-aarch64" ;;
    riscv64) RUNNER="qemu-riscv64" ;;
  esac
  echo "  运行: $RUNNER ./prog < probe.in   (cwd=$WORK/work_$T/0000)"
  # 用子 shell + 立即取 $? 才能拿到【程序本身】的退出码（被后续命令覆盖是常见坑）
  ( cd "$WORK/work_$T/0000" && $RUNNER ./prog < "$WORK/probe.in" > "$WORK/got_$T.bin" 2>"$WORK/got_$T.err" )
  GOT_CODE=$?
  echo "  stdout: [$(cat "$WORK/got_$T.bin")]   退出码: $GOT_CODE"
  echo "  期望(C0): $(printf '42\n7\n' | od -c | head -2 | tr -s ' ' | tr '\n' ' ')"
  echo "  实际    : $(od -c "$WORK/got_$T.bin" | head -2 | tr -s ' ' | tr '\n' ' ')  退出码=$GOT_CODE"

  if [ "$rc" = 0 ] && [ "$GOT_CODE" = 7 ]; then
    echo "  ✔ $T 自检通过（stdout=42、退出码=7、按 C0 比对命中）"
  else
    echo "  ✘ $T 自检失败（完整日志 $LOG）"
    sed 's/^/    /' "$LOG" | tail -25
    FAIL=1
  fi
done

echo
echo "════════════════════════════════════════════════════════════"
if [ "$FAIL" = 0 ]; then
  echo " 自检结果：✔ 三种 target 全部通过（x86 / aarch64 / riscv64）"
else
  echo " 自检结果：✘ 有 target 失败 —— 【不要进入 S01】"
fi
echo "════════════════════════════════════════════════════════════"

if [ "$KEEP" = 0 ] && [ "$FAIL" = 0 ]; then rm -rf "$WORK"; else echo "保留工作目录: $WORK"; fi
exit "$FAIL"
