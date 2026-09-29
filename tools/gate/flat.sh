# Sourced by tools/verify.sh; shared state/contract: gate/README.md.
hdr "C8. 平面 IR 自校验（若 --emit=flat-ir 已实现 —— S06 起）"
CAN_FLAT=0
if [ -x "$COMPILER" ] && [ -f "$TOOLS/selftest/check_flat.py" ]; then
  if "$COMPILER" --emit=flat-ir "$TESTS/final_arm/functional/00_main.sy" \
        -o "$WORK/flat_probe.txt" >/dev/null 2>&1 && grep -qE '^\s*(define|@|%)' "$WORK/flat_probe.txt" 2>/dev/null; then
    CAN_FLAT=1
  fi
fi
if [ "$CAN_FLAT" = "0" ]; then
  c_skip "平面 IR 跳过：--emit=flat-ir 尚未实现（S06 的交付物）"
else
  EX6="$ROOT/compiler/tests/flat/example"
  if [ -f "$EX6/example.sy" ] && [ -f "$EX6/example.emit-flat.txt" ] && "$COMPILER" --emit=flat-ir "$EX6/example.sy" -o "$WORK/ex6.txt" >/dev/null 2>&1 && cmp -s "$WORK/ex6.txt" "$EX6/example.emit-flat.txt"; then
    c_ok "平面 IR：与真实生成的样例对逐字节相同"
  else
    c_bad "平面 IR 样例对缺失或不一致（违反格式契约）"
  fi
  # check_flat.py 的常规模式跑完整语料与坏 IR 反证；--bad 只跑反证。
  FLOG="$WORK/flat.log"
  FLAT_RC=0
  python3 "$TOOLS/selftest/check_flat.py" --compiler "$COMPILER" \
      --root "$ROOT" --jobs "$(nproc)" >"$FLOG" 2>&1 || FLAT_RC=$?
  FLAT_TOTAL=$(sed -n 's/^=== 轨 A\/B\/C：\([0-9][0-9]*\) 个文件.*/\1/p' "$FLOG" | tail -1)
  FLAT_OK=$(sed -n 's/^  ok[[:space:]]*\([0-9][0-9]*\)[[:space:]]*$/\1/p' "$FLOG" | tail -1)
  FLAT_FRONTEND=$(sed -n 's/^  frontend[[:space:]]*\([0-9][0-9]*\)[[:space:]]*$/\1/p' "$FLOG" | tail -1)
  FLAT_OK=${FLAT_OK:-0}
  FLAT_FRONTEND=${FLAT_FRONTEND:-0}
  # 清单的 tensor 项应由前端拒绝；check_flat.py 只列前 8 个失败文件，
  # 此处核对各桶计数，不能逐文件核对 frontend 桶的成员。
  FLAT_EXPECTED=""
  if [ -f "$TESTS/manifest.tsv" ]; then
    FLAT_EXPECTED=$(awk -F '\t' '!/^#/ && NF == 7 {
      if ($7 == "-") ok++;
      else if ($7 == "tensor") frontend++;
      else other++;
    } END {print ok+0, frontend+0, other+0}' "$TESTS/manifest.tsv")
  fi
  read -r FLAT_EXPECT_OK FLAT_EXPECT_FRONTEND FLAT_EXPECT_OTHER <<< "$FLAT_EXPECTED"
  FLAT_EXPECT_OK=${FLAT_EXPECT_OK:-0}
  FLAT_EXPECT_FRONTEND=${FLAT_EXPECT_FRONTEND:-0}
  FLAT_EXPECT_OTHER=${FLAT_EXPECT_OTHER:-0}
  FLAT_BAD_COUNTS=$(sed -n 's/^=== 轨 B 反证（手工坏 IR）：\([0-9][0-9]*\)\/\([0-9][0-9]*\) 报红$/\1 \2/p' "$FLOG" | tail -1)
  FLAT_BAD_OK=${FLAT_BAD_COUNTS%% *}
  FLAT_BAD_TOTAL=${FLAT_BAD_COUNTS#* }
  FLAT_CORPUS_PASS=0
  if [ -n "$FLAT_TOTAL" ] && [ "$FLAT_EXPECT_OK" -gt 0 ] && \
     [ "$FLAT_EXPECT_OTHER" -eq 0 ] && \
     [ "$FLAT_TOTAL" -eq "$((FLAT_EXPECT_OK + FLAT_EXPECT_FRONTEND))" ] && \
     [ "$FLAT_OK" -eq "$FLAT_EXPECT_OK" ] && \
     [ "$FLAT_FRONTEND" -eq "$FLAT_EXPECT_FRONTEND" ]; then
    FLAT_CORPUS_PASS=1
    c_ok "平面 IR 轨 A/B/C：$FLAT_OK 个通过、$FLAT_FRONTEND 个前端拒绝（与清单计数一致）"
    grep -E 'ok |往返|基本块|指令数|φ 数|反证' "$FLOG" | head -5 | sed 's/^/      /'
  else
    c_bad "平面 IR 轨 A/B/C 失败（往返 / 不变式 / 覆盖性）"
    echo "      实际: total=${FLAT_TOTAL:-缺失} ok=$FLAT_OK frontend=$FLAT_FRONTEND；清单: ok=$FLAT_EXPECT_OK tensor=$FLAT_EXPECT_FRONTEND other=$FLAT_EXPECT_OTHER"
    grep -E 'verify|flatgen|crash|frontend|parse|invariant|roundtrip|mismatch|Traceback|Error' "$FLOG" | head -8 | sed 's/^/      /'
  fi
  FLAT_BAD_PASS=0
  if [ -n "$FLAT_BAD_OK" ] && [ -n "$FLAT_BAD_TOTAL" ] && \
     [ "$FLAT_BAD_TOTAL" -eq 8 ] && [ "$FLAT_BAD_OK" = "$FLAT_BAD_TOTAL" ]; then
    FLAT_BAD_PASS=1
    c_ok "平面 IR 轨 B：$FLAT_BAD_OK/$FLAT_BAD_TOTAL 份坏 IR 反证报红"
  else
    c_bad "平面 IR 轨 B 坏 IR 反证失败"
    grep -E '轨 B 反证|✗|Traceback|Error' "$FLOG" | tail -8 | sed 's/^/      /'
  fi
  # check_flat.py 当前把预期的 frontend 桶也计入退出码 1。
  if [ "$FLAT_RC" -ne 0 ] && \
     { [ "$FLAT_RC" -ne 1 ] || [ "$FLAT_CORPUS_PASS" -ne 1 ] || \
       [ "$FLAT_BAD_PASS" -ne 1 ] || [ "$FLAT_FRONTEND" -eq 0 ]; }; then
    c_bad "平面 IR 检查器异常退出（rc=$FLAT_RC）"
  fi
  if [ "$QUICK" = 1 ]; then
    c_skip "轨 D（独立展平器）--quick 档跳过 —— **交付前请跑全量**"
  elif [ -f "$TOOLS/selftest/independent_flatten_check.py" ]; then
    ILOG="$WORK/iflat.log"
    if python3 "$TOOLS/selftest/independent_flatten_check.py" --compiler "$COMPILER" \
          --dir "$ROOT/tests" --jobs "$(nproc)" >"$ILOG" 2>&1; then
      c_ok "独立展平器：与 C++ 输出逐字节一致"
    else
      c_bad "独立展平器不一致（C++ 侧或独立侧有真 bug）"
      grep -E '有差异|不一致|✘' "$ILOG" | head -8 | sed 's/^/      /'
    fi
  else
    c_skip "independent_flatten_check.py 未实现（S06 的交付物）"
  fi
  if [ "$QUICK" = 1 ]; then
    c_skip "轨 E（两种形状行为等价）--quick 档跳过 —— **交付前请跑全量**"
  elif [ -f "$TOOLS/selftest/flat_exec.py" ]; then
    ELOG="$WORK/flat_exec.log"
    if python3 "$TOOLS/selftest/flat_exec.py" --compiler "$COMPILER" \
          --dir "$ROOT/tests" --jobs "$(nproc)" --min-files 400 >"$ELOG" 2>&1; then
      c_ok "行为等价：结构化与平面两种形状可观察轨迹相同"
      grep -E '完整轨迹相同|循环计数口径不同|可观察轨迹不同|跳过:' "$ELOG" | tail -4 | sed 's/^/      /'
    else
      c_bad "行为等价检查失败（变换改了行为——先证明判据自己对，再查产品）"
      grep -E '不同|差异|✘' "$ELOG" | head -8 | sed 's/^/      /'
    fi
  else
    c_skip "flat_exec.py 未实现（S06 的交付物）"
  fi
  # 跨层结构对应：仅有可达正常继续或 Continue 路径的循环需要回边；
  # 全部可达路径均 Break/Return 的循环可没有回边。
  # 检查器按函数统计，不能将绿色结果视作逐循环的一一对应证明。
  if [ -f "$TOOLS/selftest/loop_backedge_census.py" ]; then
    BLOG="$WORK/backedge.log"
    BACKEDGE_RC=0
    python3 "$TOOLS/selftest/loop_backedge_census.py" --compiler "$COMPILER" \
        --dir "$ROOT/tests" --jobs "$(nproc)" --expect-files "$FLAT_EXPECT_OK" \
        >"$BLOG" 2>&1 || BACKEDGE_RC=$?
    KNOWN="$TOOLS/selftest/backedge_known.txt"
    FOUND=$(grep -oE '^  · [^ ]+' "$BLOG" | sed 's/^  · //' | sort -u)
    if [ "$BACKEDGE_RC" -gt 1 ]; then
      c_bad "跨层结构对应普查工具错误（rc=$BACKEDGE_RC）"
      tail -8 "$BLOG" | sed 's/^/      /'
    elif [ "$BACKEDGE_RC" = 1 ] && [ -z "$FOUND" ]; then
      c_bad "跨层结构对应普查报告缺失回边，但未列出文件"
      tail -8 "$BLOG" | sed 's/^/      /'
    elif [ "$BACKEDGE_RC" = 0 ] && [ -n "$FOUND" ]; then
      c_bad "跨层结构对应普查结果与退出码不一致"
      tail -8 "$BLOG" | sed 's/^/      /'
    elif [ -z "$FOUND" ]; then
      c_ok "跨层结构对应：所需回边数量达标（按函数统计，零缺失）"
    else
      NEW=$(comm -23 <(echo "$FOUND") <(grep -v '^#' "$KNOWN" 2>/dev/null | grep -v '^$' | sort -u) 2>/dev/null)
      if [ -z "$NEW" ]; then
        c_ok "跨层结构对应：缺失 $(echo "$FOUND" | wc -l) 个文件，全部是已知未修缺陷（棘轮内）"
      else
        c_bad "出现新的所需回边缺失（存在可达继续路径，但平面回边不足）"
        echo "$NEW" | head -5 | sed 's/^/      NEW: /'
      fi
    fi
  fi
  if [ -f "$TOOLS/selftest/run_flat_cases.py" ]; then
    LLOG="$WORK/flatcases.log"
    if python3 "$TOOLS/selftest/run_flat_cases.py" --compiler "$COMPILER" >"$LLOG" 2>&1; then
      SUM=$(grep -E '通过|总计' "$LLOG" | tail -1)
      c_ok "平面 IR 用例集：${SUM:-全过}"
    else
      c_bad "平面 IR 用例集有失败"
      grep -E '✘|失败|FAIL' "$LLOG" | head -8 | sed 's/^/      /'
    fi
  else
    c_skip "run_flat_cases.py 未实现（S06 的交付物）"
  fi
fi


return 0
