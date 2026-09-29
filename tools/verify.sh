#!/usr/bin/env bash
# ============================================================================
# verify.sh —— 会话交付质量关卡（开发者运行，验收独立复核）
#
# 设计意图：
#   开发 agent 的报告是"自述"，本脚本是"事实核查"。两者独立，才能发现虚报与回归。
#   判据可读；通过反例测试与独立复核保证检查有效。
#
# 用法：
#   bash compiler/tools/verify.sh              # 自动探测当前能力，跑到能跑的程度
#   bash compiler/tools/verify.sh --stage S11  # 指定阶段，启用该阶段的硬门禁
#
# 退出码：0 = 全部通过；1 = 有失败项
# ============================================================================
set -uo pipefail

TOOLS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$TOOLS/../.." && pwd)"
GATE="$TOOLS/gate"
COMPILER="$ROOT/compiler/build/compiler"
TESTS="$ROOT/tests"
RUNTIME="$ROOT/runtime"
LLVM_BIN="${LLVM_BIN:-/usr/lib/llvm-18/bin}"
WORK="${TMPDIR:-/tmp}/verify_$$"
STAGE=""
QUICK=0        # --quick：日常档，跳过五条耗时交叉验证轨（**交付前必须跑全量**）
FAIL=0
PASS=0
SKIP=0

mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

while [ $# -gt 0 ]; do
  case "$1" in
    --stage) [ $# -ge 2 ] || { echo "--stage requires a value" >&2; exit 2; }; STAGE="$2"; shift 2 ;;
    --quick) QUICK=1; shift ;;
    *) echo "未知参数: $1"; exit 2 ;;
  esac
done

c_ok()   { printf '  \033[32m✔\033[0m %s\n' "$1"; PASS=$((PASS+1)); }
c_bad()  { printf '  \033[31m✘\033[0m %s\n' "$1"; FAIL=$((FAIL+1)); }
c_skip() { printf '  \033[33m−\033[0m %s\n' "$1"; SKIP=$((SKIP+1)); }
hdr()    { printf '\n\033[1m═══ %s ═══\033[0m\n' "$1"; }

echo "════════════════════════════════════════════════════════════════"
echo " 会话交付质量关卡    $(date '+%Y-%m-%d %H:%M:%S')   阶段=${STAGE:-自动}$([ "$QUICK" = 1 ] && echo '   【--quick 日常档】')"
echo "════════════════════════════════════════════════════════════════"

# ─────────────────────────────────────────────────────────────────────────────
hdr "A. 构建（零警告是硬要求）"
# ─────────────────────────────────────────────────────────────────────────────

# 每组在同一 shell 中更新 CAN_* / PASS / FAIL / SKIP；顺序是显式契约。
source "$GATE/stages.sh" || exit 2
validate_stage "$STAGE" || exit 2
CAN_IR=0; IR_VALID=0; CAN_SEMA=0; CAN_INITPLAN=0; CAN_SIR=0; CAN_NORM=0; CAN_FLAT=0
E2E_X86=0; E2E_AARCH64=0; E2E_RISCV64=0; E2E_NO_STRUCT=0; TOY_OK=0
for group in build architecture compliance llvm frontend initializers structured loops flat execution; do
  if ! source "$GATE/$group.sh"; then
    c_bad "关卡模块加载/执行失败: $group"
    exit 1
  fi
done
hdr "F. 阶段硬门禁"
check_stage "$STAGE"
echo
printf ' 结果：%d 通过  %d 失败  %d 跳过\n' "$PASS" "$FAIL" "$SKIP"
if [ "$FAIL" != 0 ]; then
  echo "判定：FAIL — 未通过"
  exit 1
fi
if [ "$QUICK" = 1 ]; then
  echo "判定：QUICK/SMOKE PASS — NOT FULL ACCEPTANCE（快速反馈，非阶段验收）"
else
  echo "判定：PASS — 本次完整关卡通过；能力边界与跳过项见上文"
fi
