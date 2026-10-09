create_verify_workspace() {
  local tmp_base="${TMPDIR:-/tmp}"
  VERIFY_WORK="$(mktemp -d "$tmp_base/sysy_verify.XXXXXXXX")" || {
    echo "无法创建 verify 临时目录" >&2
    return 1
  }
}
