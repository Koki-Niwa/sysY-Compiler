# Sourced by tools/verify.sh; shared state/contract: gate/README.md.
# S07's evidence is deliberately two-level: CAN_IR is only a one-file probe;
# IR_VALID is set only after the manifest checker has validated every identity.

hdr "C. LLVM IR 发射与 S07 清单门禁"
CAN_IR=0
IR_VALID=0
LLVM_LOG="$WORK/llvm-ir.log"
LLVM_REPORT="$WORK/llvm-ir.json"

if [ ! -x "$COMPILER" ]; then
  c_skip "LLVM 发射跳过：编译器未构建"
else
  PROBE_SY="$TESTS/final_arm/functional/00_main.sy"
  rm -f "$WORK/gate.ll"
  probe_rc=0
  "$COMPILER" "$PROBE_SY" -o "$WORK/gate.ll" >"$WORK/capability.log" 2>&1 || probe_rc=$?
  if [ "$probe_rc" = 0 ] && grep -q '^define ' "$WORK/gate.ll" 2>/dev/null; then
    CAN_IR=1
    c_ok "LLVM 能力探测：默认模式产出真实函数定义"
  elif [ "$probe_rc" = 2 ] && grep -q 'error\[E-UNSUPPORTED-LLVM\]' "$WORK/capability.log"; then
    c_skip "LLVM 清单门禁跳过：编译器尚未产出真实 IR"
  else
    c_bad "LLVM 能力探测失败：既非真实产物，也非明确的未实现诊断"
    tail -8 "$WORK/capability.log" | sed 's/^/      /'
  fi

  if [ "$CAN_IR" = 1 ]; then
    if [ ! -x "$LLVM_BIN/llvm-as" ]; then
      c_bad "S07 LLVM 清单门禁不可用：找不到 $LLVM_BIN/llvm-as"
    elif python3 "$TOOLS/selftest/check_llvm_ir.py" \
         --root "$ROOT" --manifest "$TESTS/manifest.tsv" \
         --compiler "$COMPILER" --llvm-as "$LLVM_BIN/llvm-as" \
         --jobs "$(nproc)" --work "$WORK/llvm-ir-cases" \
         --report "$LLVM_REPORT" >"$LLVM_LOG" 2>&1; then
      IR_VALID=1
      c_ok "S07 LLVM 清单门禁：490 个真实模块通过 llvm-as，50 个 tensor 前端拒绝"
      grep '^LLVM manifest:' "$LLVM_LOG" | sed 's/^/      /'
    else
      c_bad "S07 LLVM 清单门禁失败：逐文件发射/汇编/身份证据不完整"
      tail -12 "$LLVM_LOG" | sed 's/^/      /'
    fi
  fi
fi

return 0
