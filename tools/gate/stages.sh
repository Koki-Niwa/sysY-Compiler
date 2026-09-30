# 阶段顺序与必需证据。可单独 source 做反例测试，不执行编译/语料。
STAGES=(S00 S01 S02 S03 S04 S05a S05 S05b S06 S07 S08 S09 S10a S11 S10b S11b
        S12 S13 S14 S15 S16 S17 S18 S19 S20 S21 S22 S23 S24 S25 S26 S27 S28)
stage_index() {
  local index stage="$1"
  # Keep the former S10 spelling as an alias for the AST interpreter milestone.
  [ "$stage" = S10 ] && stage=S10a
  for index in "${!STAGES[@]}"; do
    if [ "${STAGES[$index]}" = "$stage" ]; then echo "$index"; return 0; fi
  done
  return 1
}
validate_stage() {
  [ -z "$1" ] && return 0
  stage_index "$1" >/dev/null || { echo "未知阶段: $1" >&2; return 2; }
}
check_stage() {
  local stage="$1" actual threshold flag label minimum
  [ -z "$stage" ] && { echo "未指定阶段，仅报告执行过的检查"; return 0; }
  actual=$(stage_index "$stage") || return 2
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
    minimum=$(stage_index "$threshold")
    if [ "$actual" -ge "$minimum" ] && [ "${!flag:-0}" != 1 ]; then
      c_bad "阶段 $stage 缺少必需能力/证据: $label ($flag)"
    fi
  done
  return 0
}
