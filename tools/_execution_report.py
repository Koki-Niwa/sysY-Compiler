"""S11 execution report formatter and revision-bound JSON writer."""
import json
import sys

PASS = "PASS"
FORMAT_WARN = "PASS(FORMAT_C1)"
FORMAT_NOTE = "PASS(FORMAT_C2/C3)"
COMPILE_FAIL = "COMPILE_FAIL"
BUILD_FAIL = "BUILD_FAIL"
WRONG_OUTPUT = "WRONG_OUTPUT"
TIMEOUT = "TIMEOUT"
SKIP = "SKIP"

def _emit(results, args, elapsed, work_root):
    order = {PASS: 0, FORMAT_WARN: 1, FORMAT_NOTE: 1, COMPILE_FAIL: 2,
             BUILD_FAIL: 3, WRONG_OUTPUT: 4, TIMEOUT: 5, SKIP: 6}
    results.sort(key=lambda r: (r["suite"], r["category"], r["name"]))

    groups = {}
    for r in results:
        g = groups.setdefault("%s/%s" % (r["suite"], r["category"]),
                              {"total": 0, "pass": 0, "fail": 0, "timeout": 0, "skip": 0})
        g["total"] += 1
        st = r["status"]
        if st == SKIP:
            g["skip"] += 1
        elif st == TIMEOUT:
            g["timeout"] += 1
        elif st in (PASS, FORMAT_WARN, FORMAT_NOTE):
            g["pass"] += 1
        else:
            g["fail"] += 1

    n_pass = sum(1 for r in results if r["status"] in (PASS, FORMAT_WARN, FORMAT_NOTE))
    n_fail = sum(1 for r in results if r["status"] in
                 (COMPILE_FAIL, BUILD_FAIL, WRONG_OUTPUT))
    n_timeout = sum(1 for r in results if r["status"] == TIMEOUT)
    n_skip = sum(1 for r in results if r["status"] == SKIP)
    n_fmt = sum(1 for r in results if r["status"] in (FORMAT_WARN, FORMAT_NOTE))

    out = sys.stdout
    out.write("\n=== 汇总 ===\n")
    out.write("%-28s %6s %6s %6s %6s %6s\n"
              % ("suite/category", "总数", "通过", "失败", "超时", "跳过"))
    for key in sorted(groups):
        g = groups[key]
        out.write("%-28s %6d %6d %6d %6d %6d\n"
                  % (key, g["total"], g["pass"], g["fail"], g["timeout"], g["skip"]))
    out.write("-" * 70 + "\n")
    total_all = len(results)
    tail_note = ("（跳过 %d 个 tensor/无 .out 用例）" % n_skip) if n_skip else ""
    out.write("总计: %d / %d 通过    (失败 %d，超时 %d，跳过 %d，耗时 %.1fs) %s\n"
              % (n_pass, total_all - n_skip, n_fail, n_timeout, n_skip, elapsed, tail_note))
    out.write("target=%s  工作目录=%s\n" % (args.target, work_root))
    if n_fmt:
        out.write("注: %d 个用例按 C1–C3 命中（.out 格式差异，非语义错误）: %s\n"
                  % (n_fmt, ", ".join(r["key"] for r in results
                                      if r["status"] in (FORMAT_WARN, FORMAT_NOTE))[:400]))

    bad = [r for r in results if r["status"] in
           (COMPILE_FAIL, BUILD_FAIL, WRONG_OUTPUT, TIMEOUT)]
    if bad:
        out.write("\n失败的用例:\n")
        for r in sorted(bad, key=lambda r: (order.get(r["status"], 9), r["key"])):
            line = "  %s  [%s]" % (r["key"], r["status"])
            if r["detail"]:
                first = r["detail"].splitlines()[0]
                line += "  " + first
            out.write(line + "\n")
            for extra in r["detail"].splitlines()[1:10]:
                out.write("      " + extra + "\n")
    else:
        out.write("\n失败的用例: 无\n")

    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump({**args.evidence, "target": args.target, "elapsed": elapsed,
                       "summary": {"total": total_all, "pass": n_pass, "fail": n_fail,
                                   "timeout": n_timeout, "skip": n_skip,
                                   "format_diff": n_fmt},
                       "results": results}, f, ensure_ascii=False, indent=1)
        out.write("\nJSON 明细: %s\n" % args.json)

    return 0 if (n_fail == 0 and n_timeout == 0) else 1
