# 完整执行：进程成功且逐文件身份和覆盖范围均通过，才设置证据位。
if [ -n "${STAGE:-}" ]; then
  declare -F stage_requires >/dev/null || return 2
  stage_requires "$STAGE" S11
  stage_requirement=$?
  if [ "$stage_requirement" -gt 1 ]; then
    return "$stage_requirement"
  elif [ "$stage_requirement" = 1 ]; then
    hdr "D. 全量回归（运行 + manifest 身份验证）"
    c_skip "阶段 $STAGE 不要求 S11 目标执行证据"
    return 0
  fi
fi

run_execution() {
  local flag="$1" target="$2" label="$3"
  local execution_timeout
  case "$flag" in
    E2E_X86|E2E_AARCH64|E2E_RISCV64|E2E_NO_STRUCT) ;;
    *) c_bad "未知 execution evidence flag: $flag"; return 2 ;;
  esac
  case "$target" in
    x86) execution_timeout=60 ;;
    aarch64|riscv64) execution_timeout=300 ;;
    *) c_bad "未知 execution target: $target"; return 2 ;;
  esac
  shift 3
  local log="$WORK/$flag.log" report="$WORK/$flag.json"
  if printf 'target=%s correctness_timeout=%ss\n' "$target" "$execution_timeout" >"$log" && \
     bash "$TOOLS/run_tests.sh" --target "$target" --timeout "$execution_timeout" --jobs "$(nproc)" \
       --json "$report" "$@" >>"$log" 2>&1 && \
     python3 "$GATE/check_execution.py" --manifest "$TESTS/manifest.tsv" \
       --report "$report" --target "$target" --compiler "$COMPILER" "$@" >>"$log" 2>&1; then
    printf -v "$flag" '%s' 1
    c_ok "$label：490 必需用例通过，50 清单内排除；逐文件证据完整"
  else
    c_bad "$label：执行失败或证据不完整"
    tail -10 "$log" | sed 's/^/      /'
  fi
}

hdr "D. 全量回归（运行 + manifest 身份验证）"
CAN_E2E="$CAN_IR"
if [ "$CAN_IR" = 0 ]; then
  c_skip "端到端尚不可用：LLVM 发射未实现"
elif [ ! -x "$TOOLS/run_tests.sh" ]; then
  c_bad "缺少全量运行器 run_tests.sh"
else
  for target in x86 aarch64 riscv64; do
    case "$target" in
      x86) flag=E2E_X86 ;;
      aarch64) flag=E2E_AARCH64
        command -v qemu-aarch64 >/dev/null || { c_skip "aarch64：无 qemu"; continue; } ;;
      riscv64) flag=E2E_RISCV64
        command -v qemu-riscv64 >/dev/null || { c_skip "riscv64：无 qemu"; continue; } ;;
    esac
    run_execution "$flag" "$target" "回归 $target"
  done
  run_execution E2E_NO_STRUCT x86 "回归 --no-structured" --no-structured
fi

run_toy_execution() {
  # Isolated from run_execution: accepting a toy CLI flag cannot set TOY_OK.
  local toy_bin="$ROOT/compiler/build/toy_backend"
  local log="$WORK/TOY_OK.log" report="$WORK/TOY_OK.json"
  if [ ! -x "$toy_bin" ]; then
    c_bad "toy backend binary missing: $toy_bin"
    return 1
  fi
  if printf 'target=x86 provider=self-built-toy correctness_timeout=600s\n' >"$log" &&
     bash "$TOOLS/run_tests.sh" --target x86 --toy-backend \
       --toy-backend-bin "$toy_bin" --timeout 600 --jobs "$(nproc)" \
       --json "$report" >>"$log" 2>&1 &&
     python3 "$GATE/check_execution.py" --manifest "$TESTS/manifest.tsv" \
       --report "$report" --target x86 --compiler "$COMPILER" \
       --toy-backend --toy-backend-bin "$toy_bin" >>"$log" 2>&1; then
    TOY_OK=1
    c_ok "S11b toy lowering: 490 required PASS / 50 tensor SKIP; provider identity verified"
  else
    c_bad "S11b toy execution failed or provider evidence incomplete"
    tail -10 "$log" | sed 's/^/      /'
    return 1
  fi
}
hdr "E. 自研降级器契约验证"
if [ -n "${STAGE:-}" ] && stage_requires "$STAGE" S11b; then
  run_toy_execution || :
else
  c_skip "S11b toy backend not required at stage ${STAGE:-unspecified}"
fi
return 0
