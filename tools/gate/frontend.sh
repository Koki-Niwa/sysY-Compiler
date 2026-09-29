# Sourced by tools/verify.sh; shared state/contract: gate/README.md.
# ─────────────────────────────────────────────────────────────────────────────
hdr "C2. 词法自校验（若 check_lexer.py 已实现 —— S01 起）"
# ─────────────────────────────────────────────────────────────────────────────
if [ -f "$TOOLS/selftest/check_lexer.py" ] && [ -x "$COMPILER" ]; then
  LOG="$WORK/lex.log"
  if python3 "$TOOLS/selftest/check_lexer.py" --compiler "$COMPILER" \
        --jobs "$(nproc)" >"$LOG" 2>&1; then
    SUM=$(grep -E '不变式成立|范围内验证' "$LOG" | tr -s ' ' | tr '\n' ' ')
    c_ok "词法自校验：不变式成立 ${SUM:-通过}"
  else
    c_bad "词法自校验失败（词法器有 bug —— 拼接对不上原文）"
    grep -E '失败|首处差异|✘' "$LOG" | head -8 | sed 's/^/      /'
  fi
  # 附带：540 个文件在 --emit=tokens 下不崩（退出码 0 或 1 均可，但不能是信号/超时）
  CRASH=0
  for f in $(find "$ROOT/tests" -name '*.sy' 2>/dev/null); do
    # 超时护栏：词法器一旦死循环（正是"Invalid 必须前进"那条规则在防的失效模式），
    # 关卡会永久挂住而不是报红。rc=124 表示超时。
    timeout 20 "$COMPILER" "$f" --emit=tokens -o /dev/null >/dev/null 2>&1
    rc=$?
    [ "$rc" -gt 1 ] && CRASH=$((CRASH+1))
  done
  [ "$CRASH" = "0" ] && c_ok "540 个 .sy 在 --emit=tokens 下无一崩溃" \
                     || c_bad "$CRASH 个文件在 --emit=tokens 下异常退出（>1 或信号）"
else
  c_skip "check_lexer.py 未实现（S01 的交付物）"
fi

# ─────────────────────────────────────────────────────────────────────────────
hdr "C3. 语法自校验（若 check_parser.py 已实现 —— S02 起）"
# ─────────────────────────────────────────────────────────────────────────────
if [ -f "$TOOLS/selftest/check_parser.py" ] && [ -x "$COMPILER" ]; then
  LOG="$WORK/parse.log"
  if python3 "$TOOLS/selftest/check_parser.py" --compiler "$COMPILER" \
        --jobs "$(nproc)" >"$LOG" 2>&1; then
    SUM=$(grep -E '范围内往返成功|范围外已报诊断' "$LOG" | tr -s ' ' | tr '\n' ' ')
    c_ok "语法自校验：AST 往返逐字节相同 ${SUM:-通过}"
  else
    c_bad "语法自校验失败（打印器/读取器不互逆，或解析器丢信息）"
    grep -E '失败|不一致|✘|首处' "$LOG" | head -8 | sed 's/^/      /'
  fi
  # 独立实现交叉验证（若存在）：与 C++ 完全无关的第二份 parser 必须产出同一份 AST
  if [ -f "$TOOLS/selftest/independent_parser_check.py" ]; then
    ILOG="$WORK/iparse.log"
    if python3 "$TOOLS/selftest/independent_parser_check.py" --compiler "$COMPILER" \
          --jobs "$(nproc)" >"$ILOG" 2>&1; then
      c_ok "独立 parser 交叉验证：与 C++ 输出逐字节一致"
    else
      c_bad "独立 parser 交叉验证不一致（C++ 侧或独立侧有真 bug）"
      grep -E '不一致|mismatch|✘' "$ILOG" | head -8 | sed 's/^/      /'
    fi
  fi
  # ★ 冻结基线的指纹比对：`--emit=ast` 是对外契约，新阶段只许做加法。
  #   基线由编排方在阶段开始前生成并进版本库 —— 开发 agent 无法"改完再生成基线"。
  if [ -f "$TOOLS/selftest/baseline_ast_sha256.txt" ]; then
    BLOG="$WORK/astbase.log"
    if python3 "$TOOLS/selftest/check_ast_baseline.py" --compiler "$COMPILER" \
          --jobs "$(nproc)" >"$BLOG" 2>&1; then
      c_ok "AST 冻结基线：490 个文件逐字节不变"
    else
      c_bad "AST 输出偏离冻结基线（新阶段只能做加法，不许改已有格式）"
      grep -E '✘|逐字节相同' "$BLOG" | head -8 | sed 's/^/      /'
    fi
  fi
else
  c_skip "check_parser.py 未实现（S02 的交付物）"
fi

# ─────────────────────────────────────────────────────────────────────────────
hdr "C4. 语义自校验（若 --emit=sema 已实现 —— S03 起）"
# ─────────────────────────────────────────────────────────────────────────────
# ⚠️ 探测判据不能只看"退出码 0"：现阶段 `--emit=sema` 会被当成未知取值而**回落到默认转储**，
#    于是探测会误判成"已实现"，接着拿纯 AST 去比注解样例、报出假红。
#    真正的能力信号是**输出里有没有 `(RuntimeLib` 头**（S03 格式契约的第一行）。
CAN_SEMA=0
if [ -x "$COMPILER" ] && [ -f "$TESTS/final_arm/functional/00_main.sy" ]; then
  if "$COMPILER" --emit=sema "$TESTS/final_arm/functional/00_main.sy" -o "$WORK/sema_probe.txt" \
        >/dev/null 2>&1 && grep -q '^(RuntimeLib' "$WORK/sema_probe.txt" 2>/dev/null; then
    CAN_SEMA=1
  fi
fi
if [ "$CAN_SEMA" = "0" ]; then
  c_skip "语义转储跳过：--emit=sema 尚未实现（S03 的交付物）"
else
  # ① 规范样例对：转储必须与仓库里那份"已验证过能还原"的样例逐字节相同。
  #    这是 §五 格式契约的可执行版本 —— 纯文字描述产生过歧义（记号该放前面还是后面、
  #    下标换不换行），样例把歧义消掉了。
  EX="$TOOLS/selftest/sema_dump_example"
  if [ -f "$EX/example.sy" ] && [ -f "$EX/example.emit-sema.txt" ]; then
    if "$COMPILER" --emit=sema "$EX/example.sy" -o "$WORK/ex.sema" >/dev/null 2>&1 \
       && cmp -s "$WORK/ex.sema" "$EX/example.emit-sema.txt"; then
      c_ok "语义转储：与规范样例逐字节相同"
    else
      c_bad "语义转储与规范样例不一致（违反 §五 的格式契约）"
      diff "$EX/example.emit-sema.txt" "$WORK/ex.sema" 2>/dev/null | head -8 | sed 's/^/      /'
    fi
  fi
  # ② 独立不变式检查器（轨 A 零误报 + 去注解还原 + 16 条类型不变式，一次扫描）
  if [ -f "$TOOLS/selftest/check_sema.py" ]; then
    SLOG="$WORK/sema.log"
    if python3 "$TOOLS/selftest/check_sema.py" --compiler "$COMPILER" \
          --jobs "$(nproc)" >"$SLOG" 2>&1; then
      c_ok "语义自校验：零误报 + 去注解还原 + 类型不变式"
    else
      c_bad "语义自校验失败（零误报 / 还原 / 类型不变式 三者之一）"
      grep -E '✘|违反|不一致|误报' "$SLOG" | head -8 | sed 's/^/      /'
    fi
    # ②b ★ 证明检查器不是"永远说 OK"：四处**定向人为破坏**必须逐条报红。
    #     这是 S01 留下的教训 —— 自校验脚本必须先证明它抓得住错，
    #     否则"490 个文件全过"可能只是因为它什么都没查。
    if [ -f "$TOOLS/selftest/check_sema_probe.py" ]; then
      if python3 "$TOOLS/selftest/check_sema_probe.py" >"$WORK/semaprobe.log" 2>&1; then
        c_ok "语义检查器反证：四处人为破坏被逐条报红（不是永远说 OK）"
      else
        c_bad "语义检查器抓不住人为破坏 —— 它的\"全过\"不可信"
        grep -E '✘|没抓到|判定' "$WORK/semaprobe.log" | head -6 | sed 's/^/      /'
      fi
    fi
  else
    c_skip "check_sema.py 未实现（S03 的交付物）"
  fi
  # ③ 最小对照用例集
  if [ -f "$TOOLS/selftest/run_sema_cases.py" ]; then
    CLOG="$WORK/semacases.log"
    if python3 "$TOOLS/selftest/run_sema_cases.py" --compiler "$COMPILER" >"$CLOG" 2>&1; then
      SUM=$(grep -E '通过|总计' "$CLOG" | tail -1)
      c_ok "最小对照用例集：${SUM:-全过}"
    else
      c_bad "最小对照用例集有失败"
      grep -E '✘|失败|FAIL' "$CLOG" | head -8 | sed 's/^/      /'
    fi
  else
    c_skip "run_sema_cases.py 未实现（S03 的交付物）"
  fi
fi

# ─────────────────────────────────────────────────────────────────────────────

return 0
