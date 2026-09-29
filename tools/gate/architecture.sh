hdr "A2. 架构边界、能力契约与判据反证"
for check in check_architecture check_cli_capabilities; do
  if python3 "$TOOLS/selftest/$check.py" >"$WORK/$check.log" 2>&1; then
    c_ok "$check：通过"
  else
    c_bad "$check：失败"
    tail -10 "$WORK/$check.log" | sed 's/^/      /'
  fi
done
for test in test_architecture_gate test_execution_gate test_stage_gate test_llvm_ir_gate test_special_cases; do
  if python3 "$TOOLS/selftest/$test.py" >"$WORK/$test.log" 2>&1; then
    c_ok "$test：正例与反例通过"
  else
    c_bad "$test：判据存在缺口"
    tail -10 "$WORK/$test.log" | sed 's/^/      /'
  fi
done
return 0
