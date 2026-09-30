# Sourced by tools/verify.sh; shared state/contract: gate/README.md.
S08_OK=0
if [ -n "${STAGE:-}" ]; then
  declare -F stage_requires >/dev/null || return 2
  stage_requires "$STAGE" S08
  stage_requirement=$?
  if [ "$stage_requirement" -gt 1 ]; then
    return "$stage_requirement"
  elif [ "$stage_requirement" = 1 ]; then
    hdr "C8a. FlatVerifier API 契约矩阵"
    c_skip "阶段 $STAGE 不要求 S08 API 矩阵"
    return 0
  fi
fi

hdr "C8a. FlatVerifier API 契约矩阵（S08 起）"
RUNNER="$TOOLS/selftest/s08/run.sh"
if [ ! -f "$RUNNER" ]; then
  c_bad "S08 FlatVerifier API 矩阵运行器缺失"
  return 0
fi
S08_LOG="$WORK/s08_matrix.log"
if bash "$RUNNER" >"$S08_LOG" 2>&1 && \
   grep -Eq '^S08 FlatVerifier API matrix PASS: invalid=[1-9][0-9]* positive=[1-9][0-9]* legacy=8$' "$S08_LOG"; then
  S08_OK=1
  c_ok "S08：公共 FlatVerifier API 矩阵（至少 12 个坏例、6 个合法例、原 8 例映射）"
  grep '^S08 FlatVerifier API matrix PASS:' "$S08_LOG" | sed 's/^/      /'
else
  c_bad "S08 FlatVerifier API 矩阵失败或验收计数不足"
  tail -24 "$S08_LOG" | sed 's/^/      /'
fi
return 0
