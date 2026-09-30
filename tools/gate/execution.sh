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
  shift 3
  local log="$WORK/$flag.log" report="$WORK/$flag.json"
  if bash "$TOOLS/run_tests.sh" --target "$target" --jobs "$(nproc)" \
       --json "$report" "$@" >"$log" 2>&1 && \
     python3 "$GATE/check_execution.py" --manifest "$TESTS/manifest.tsv" \
       --report "$report" --target "$target" >>"$log" 2>&1; then
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

hdr "E. 自研降级器契约验证"
if [ "$CAN_IR" = 1 ] && [ -x "$TOOLS/toy_backend" ]; then
  run_execution TOY_OK x86 "自研降级器" --toy-backend
else
  c_skip "toy backend 或 LLVM 发射尚未实现（S11b 必需）"
fi
return 0
