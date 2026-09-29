# Sourced by tools/verify.sh; shared state/contract: gate/README.md.
hdr "B. 合规自查（对应铁律 2/3/4 —— 违反会被取消资格）"
# ─────────────────────────────────────────────────────────────────────────────
if [ -d "$ROOT/compiler/src" ]; then
  EXT=$(grep -rnE '\b(system|popen|execv?p?|fork)\s*\(' "$ROOT/compiler/src" 2>/dev/null | grep -v '^\s*//' || true)
  [ -z "$EXT" ] && c_ok "编译器本体没有调用外部程序" || { c_bad "发现外部进程调用"; echo "$EXT" | head -3 | sed 's/^/      /'; }

  # RuntimeLib owns names; the checker permits only the documented putf
  # variadic ABI sites in the LLVM emitter, with exact per-line allowlisting.
  if python3 "$TOOLS/selftest/check_special_cases.py" --root "$ROOT" >"$WORK/special-cases.log" 2>&1; then
    c_ok "没有识别函数名/用例名的特判（putf 仅限变参 ABI 边界）"
  else
    c_bad "疑似特判"
    tail -6 "$WORK/special-cases.log" | sed 's/^/      /'
  fi

  # ⚠️ 只检查【代码】，不检查【注释】。
  #    这条规则的目的是"代码里不许读用例路径 / 拿用例名做特判"；注释做不到这件事。
  #    而源码注释里写清楚"这个数字实测出自哪个用例"是**正常的工程记录**
  #    （例如"真实语料 tests/.../86_long_code2.sy 的打印树深是 4007"）。
  #    做法：剥掉行尾 `//` 注释，丢掉以 `*` 或 `/*` 开头的块注释续行，再匹配。
  #    （与前面 target-features 的处理同一个道理：关卡自己也会误报，误报要修关卡，不是改代码。）
  CASE=$(grep -rnE '\.(sy|in|out)"|tests/' "$ROOT/compiler/src" 2>/dev/null \
         | sed -E 's@//.*$@@' \
         | grep -vE ':[0-9]+:[[:space:]]*(/\*|\*)' \
         | grep -E '\.(sy|in|out)"|tests/' || true)
  [ -z "$CASE" ] && c_ok "没有引用测试用例路径" || { c_bad "引用了测试用例"; echo "$CASE" | head -3 | sed 's/^/      /'; }
else
  c_skip "compiler/src 不存在"
fi


return 0
