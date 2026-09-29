#!/usr/bin/env python3
"""按 manifest 身份验收运行器 JSON；rc=0 和汇总桶计数都不能单独证明通过。"""
import argparse
from collections import Counter
import json
from pathlib import Path

PASSED = {'PASS', 'PASS(FORMAT_C1)', 'PASS(FORMAT_C2/C3)'}


def manifest_cases(path):
    expected = {}
    for line in Path(path).read_text().splitlines():
        if not line or line.startswith('#'):
            continue
        cells = line.split('\t')
        if len(cells) != 7:
            raise ValueError('manifest 必须为 7 列')
        key = '/'.join(cells[:3])
        if key in expected:
            raise ValueError('manifest 身份重复: ' + key)
        if cells[6] not in ('-', 'tensor'):
            raise ValueError('未审定的跳过类别: ' + cells[6])
        expected[key] = cells[6] == 'tensor'
    return expected


def validate(report, expected, target, runner_rc=0, required=490, excluded=50):
    problems = []
    if runner_rc != 0:
        problems.append('运行器退出非零')
    if not expected or sum(not skip for skip in expected.values()) != required or \
            sum(expected.values()) != excluded:
        problems.append('manifest 范围与本阶段所需数量不符')
    if not isinstance(report, dict):
        return problems + ['JSON 根不是对象']
    if report.get('target') != target:
        problems.append('执行 target 不符')
    results = report.get('results')
    if not isinstance(results, list):
        return problems + ['缺少逐文件 results']
    ids, counts = Counter(), Counter()
    for row in results:
        if not isinstance(row, dict) or not isinstance(row.get('key'), str):
            problems.append('非法结果记录')
            continue
        key, status = row['key'], row.get('status')
        ids[key] += 1
        if not isinstance(status, str):
            problems.append('非法状态: ' + key)
            continue
        counts[status] += 1
        if key not in expected:
            problems.append('未知身份: ' + key)
        elif expected[key]:
            if status != 'SKIP' or row.get('skip_reason') != 'tensor':
                problems.append('tensor 排除记录不符: ' + key)
        elif status not in PASSED:
            problems.append('必需用例未通过: ' + key + ' (' + status + ')')
    for key in expected:
        if ids[key] != 1:
            problems.append('缺失/重复身份: ' + key)
    # 同时拒绝未知身份重复，避免把额外数据误当成正常执行证据。
    if len(results) != len(expected):
        problems.append('总身份数与 manifest 不符')
    totals = {
        'total': len(results), 'pass': sum(counts[s] for s in PASSED),
        'fail': sum(counts[s] for s in ('COMPILE_FAIL', 'BUILD_FAIL', 'WRONG_OUTPUT')),
        'timeout': counts['TIMEOUT'], 'skip': counts['SKIP'],
        'format_diff': counts['PASS(FORMAT_C1)'] + counts['PASS(FORMAT_C2/C3)'],
    }
    summary = report.get('summary')
    if not isinstance(summary, dict) or any(type(summary.get(k)) is not int or
                                           summary[k] != v for k, v in totals.items()):
        problems.append('汇总与逐文件证据不一致')
    if totals['pass'] != required or totals['skip'] != excluded:
        problems.append('未完成全部必需用例或跳过数量不符')
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', required=True)
    parser.add_argument('--report', required=True)
    parser.add_argument('--target', required=True, choices=['x86', 'aarch64', 'riscv64'])
    args = parser.parse_args()
    try:
        expected = manifest_cases(args.manifest)
        report = json.loads(Path(args.report).read_text())
        problems = validate(report, expected, args.target)
    except (OSError, ValueError, TypeError) as error:
        print('FAIL: 执行证据无法读取: ' + str(error))
        return 1
    for problem in problems[:10]:
        print('FAIL: ' + problem)
    if not problems:
        print('执行身份通过: 490 必需通过 / 50 清单内 tensor 排除')
    return int(bool(problems))


if __name__ == '__main__':
    raise SystemExit(main())
