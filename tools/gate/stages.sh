# 阶段顺序与必需证据。顺序只用于展示；先决条件由 STAGE_DEPS 显式定义。
STAGES=(S00 S01 S02 S03 S04 S05a S05 S05b S06 S07 S08 S09 S10a S11 S11b S10b
        S12 S13 S14 S15 S16 S17 S18 S19 S20 S21 S22 S23 S24 S25 S26 S27 S28)

# 旧的 S10 名称继续指向 AST 解释器会话。它不是一个额外的依赖节点。
stage_canonical() {
  [ "$1" = S10 ] && { printf '%s\n' S10a; return 0; }
  printf '%s\n' "$1"
}

declare -A STAGE_DEPS=(
  [S00]=""
  [S01]="S00"
  [S02]="S01"
  [S03]="S02"
  [S04]="S03"
  [S05a]="S04"
  [S05]="S04 S05a"
  [S05b]="S05"
  [S06]="S05b"
  [S07]="S06"
  [S08]="S07"
  [S09]="S08"
  # S10a is an independent interpreter track after its semantic inputs.
  [S10a]="S03 S04"
  [S11]="S07 S09"
  [S11b]="S11"
  [S10b]="S10a S11b"
  [S12]="S11"
  [S13]="S12"
  [S14]="S11"
  [S15]="S13"
  [S16]="S14"
  [S17]="S15"
  [S18]="S17"
  [S19]="S18"
  [S20]="S19"
  [S21]="S05 S13"
  [S22]="S21"
  [S23]="S16 S21"
  [S24]="S23"
  [S25]="S24"
  [S26]="S25"
  [S27]="S26"
  [S28]="S27"
)
declare -A STAGE_VISITING=() STAGE_SEEN=() STAGE_CLOSURE=()

stage_known() {
  local stage known
  stage="$1"
  [ "$stage" = S10 ] && stage=S10a
  for known in "${STAGES[@]}"; do
    [ "$known" = "$stage" ] && return 0
  done
  return 1
}

# Keep this legacy helper for display and diagnostics only. No gate decision may
# compare these numbers: the graph above is the sole source of prerequisites.
stage_index() {
  local index stage
  stage="$1"
  [ "$stage" = S10 ] && stage=S10a
  for index in "${!STAGES[@]}"; do
    if [ "${STAGES[$index]}" = "$stage" ]; then echo "$index"; return 0; fi
  done
  return 1
}

stage_dfs() {
  local stage="$1" dep
  if [ "${STAGE_VISITING[$stage]:-0}" = 1 ]; then
    echo "阶段依赖环: $stage" >&2
    return 3
  fi
  [ "${STAGE_SEEN[$stage]:-0}" = 1 ] && return 0
  stage_known "$stage" || { echo "未知阶段依赖: $stage" >&2; return 2; }
  STAGE_VISITING[$stage]=1
  for dep in ${STAGE_DEPS[$stage]-}; do
    stage_known "$dep" || { echo "未知阶段依赖: $stage -> $dep" >&2; return 2; }
    stage_dfs "$dep" || return $?
  done
  STAGE_VISITING[$stage]=0
  STAGE_SEEN[$stage]=1
  STAGE_CLOSURE[$stage]=1
  return 0
}

stage_closure() {
  local stage
  stage="$1"
  [ "$stage" = S10 ] && stage=S10a
  stage_known "$stage" || { echo "未知阶段: $1" >&2; return 2; }
  STAGE_VISITING=()
  STAGE_SEEN=()
  STAGE_CLOSURE=()
  stage_dfs "$stage"
}

validate_stage_graph() {
  local stage
  for stage in "${!STAGE_DEPS[@]}"; do
    stage_known "$stage" || { echo "未知阶段注册: $stage" >&2; return 2; }
  done
  for stage in "${STAGES[@]}"; do
    if [ "${STAGE_DEPS[$stage]+registered}" != registered ]; then
      echo "阶段缺少依赖注册: $stage" >&2
      return 2
    fi
    stage_closure "$stage" >/dev/null || return $?
  done
  return 0
}

# Return 0 when REQUIRED is the requested stage or one of its transitive
# prerequisites, 1 when it is independent, and 2+ for an invalid graph/query.
stage_requires() {
  local stage required
  stage="$1"
  required="$2"
  [ "$stage" = S10 ] && stage=S10a
  [ "$required" = S10 ] && required=S10a
  stage_known "$stage" || { echo "未知阶段: $1" >&2; return 2; }
  stage_known "$required" || { echo "未知阶段: $2" >&2; return 2; }
  # The closure walk validates every edge reachable from the requested stage.
  # Full graph validation remains at the gate boundary and in
  # validate_stage_graph(); repeating it for every capability query makes a
  # single check_stage call traverse the entire graph sixteen extra times.
  stage_closure "$stage" >/dev/null || return $?
  [ "${STAGE_CLOSURE[$required]:-0}" = 1 ]
}

validate_stage() {
  validate_stage_graph || return $?
  [ -z "$1" ] && return 0
  stage_known "$1" || { echo "未知阶段: $1" >&2; return 2; }
}

check_stage() {
  local stage="$1" requirement threshold flag label result
  validate_stage_graph || return $?
  [ -z "$stage" ] && { echo "未指定阶段，仅报告执行过的检查"; return 0; }
  stage_known "$stage" || { echo "未知阶段: $stage" >&2; return 2; }
  for requirement in \
    'S03 CAN_SEMA 语义转储' \
    'S04 CAN_INITPLAN 初始化计划' \
    'S05 CAN_SIR 结构化IR' \
    'S05b CAN_NORM 循环规范化' \
    'S06 CAN_FLAT 平面IR' \
    'S07 CAN_IR 真实LLVM发射' \
    'S07 IR_VALID llvm-as合法性验证' \
    'S08 S08_OK FlatVerifier 公共边界矩阵' \
    'S09 MEM2REG_OK mem2reg 可观察行为一致性' \
    'S10a INTERP_OK AST参考解释器完整自测' \
    'S11 E2E_X86 x86完整执行证据' \
    'S11 E2E_AARCH64 aarch64完整执行证据' \
    'S11 E2E_RISCV64 riscv64完整执行证据' \
    'S11 E2E_NO_STRUCT no-structured完整执行证据' \
    'S10b DIFF_OK 目标与AST解释器差分证据' \
    'S11b TOY_OK 自研降级器完整执行证据'; do
    read -r threshold flag label <<< "$requirement"
    stage_requires "$stage" "$threshold"
    result=$?
    if [ "$result" -gt 1 ]; then return "$result"; fi
    if [ "$result" = 0 ] && [ "${!flag:-0}" != 1 ]; then
      c_bad "阶段 $stage 缺少必需能力/证据: $label ($flag)"
    fi
  done
  return 0
}
