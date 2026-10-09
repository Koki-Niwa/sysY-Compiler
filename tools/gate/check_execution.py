#!/usr/bin/env python3
"""按 manifest 身份验收运行器 JSON；rc=0 和汇总桶计数都不能单独证明通过。"""
import argparse
from collections import Counter
import json
import hashlib
import os
import subprocess
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


def validate(report, expected, target, runner_rc=0, required=490, excluded=50, revision=None, manifest_sha256=None, no_structured=False, expected_compiler=None, expected_manifest=None, toy_backend=False, toy_backend_bin=None):
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
    if toy_backend and target != "x86":
        problems.append("toy provider only supports x86")
    compiler = report.get('compiler')
    manifest = report.get('manifest')
    mode = report.get('mode')
    chain = report.get('toolchain')
    if report.get('schema_version') != 1:
        problems.append('schema_version 不符')
    if not isinstance(compiler, dict) or not isinstance(compiler.get('revision'), str) or (
            revision is not None and compiler.get('revision') != revision):
        problems.append('compiler revision 不符')
    if not isinstance(compiler, dict) or compiler.get('dirty') is not False:
        problems.append('compiler dirty')
    if not isinstance(compiler, dict) or not compiler.get('executable_sha256') or not compiler.get('executable'):
        problems.append('compiler identity 缺失')
    else:
        executable = compiler['executable']
        path = expected_compiler or executable
        if not os.path.isfile(path) or not os.access(path, os.X_OK):
            problems.append('expected compiler 不存在或不可执行')
        elif os.path.realpath(executable) != os.path.realpath(path):
            problems.append('compiler executable path 不符')
        else:
            digest = hashlib.sha256(Path(path).read_bytes()).hexdigest()
            if compiler['executable_sha256'] != digest:
                problems.append('compiler executable hash 不符')
    if not isinstance(manifest, dict) or not manifest.get('path') or not manifest.get('sha256') or (
            manifest_sha256 is not None and manifest.get('sha256') != manifest_sha256):
        problems.append('manifest sha256 不符')
    if expected_manifest is not None and (not isinstance(manifest, dict) or
            not manifest.get('path') or os.path.realpath(manifest['path']) != os.path.realpath(expected_manifest)):
        problems.append('manifest path 不符')
    if not isinstance(mode, dict) or mode.get('target') != target or (
            mode.get('no_structured') is not no_structured) or (
            mode.get('boundary') != ('from-flat' if no_structured else 'source')):
        problems.append('mode/boundary 不符')
    if not isinstance(mode, dict) or mode.get("lowering", "llvm-llc") != (
            "self-built-toy" if toy_backend else "llvm-llc"):
        problems.append("lowering mode/provider mismatch")
    if not isinstance(chain, dict):
        problems.append('toolchain identity 缺失')
    else:
        llvm = chain.get('llvm', {})
        linker = chain.get('linker', {})
        execution = chain.get('execution', {})
        config = chain.get('target_config', {})
        if not all(isinstance(v, dict) for v in (llvm, linker, execution, config)) or (
                not toy_backend and not all(isinstance(llvm.get(k), dict) and llvm[k].get('path') and
                        llvm[k].get('version') for k in ('llvm_as', 'llc'))) or (
                not all(linker.get(k) for k in ('path', 'version', 'dumpmachine'))) or (
                not all(execution.get(k) for k in ('kind', 'runner', 'runner_version'))) or (
                not all(config.get(k) for k in ('triple', 'probed'))):
            problems.append('toolchain identity 缺失')
        if target == 'riscv64' and isinstance(config, dict):
            if not all(config.get('probed', {}).get(k) for k in ('dumpmachine', 'march', 'mabi')):
                problems.append('RISC-V probe 不完整')
        if target != 'x86' and isinstance(execution, dict) and (
                execution.get('kind') != 'qemu' or execution.get('real_target_performance') is not False):
            problems.append('QEMU performance label 不符')
    if toy_backend:
        provider = chain.get("lowering_provider") if isinstance(chain, dict) else None
        if not isinstance(provider, dict) or provider.get("kind") != "self-built-toy":
            problems.append("self-built toy provider identity missing")
        else:
            requested = toy_backend_bin
            if not requested or not os.path.isfile(requested) or not os.access(requested, os.X_OK):
                problems.append("toy backend executable missing")
            else:
                real = os.path.realpath(requested)
                if provider.get("path") != real:
                    problems.append("toy backend path mismatch")
                elif provider.get("sha256") != hashlib.sha256(Path(real).read_bytes()).hexdigest():
                    problems.append("toy backend SHA256 mismatch")
                if not isinstance(provider.get("version"), str) or not provider["version"]:
                    problems.append("toy backend version missing")
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
        if toy_backend and not expected.get(key, False):
            commands = row.get("compile_invocations")
            if (not isinstance(commands, list) or not commands or
                    not all(isinstance(cmd, list) for cmd in commands) or
                    not any("--emit=llvm-ir" in cmd for cmd in commands) or
                    any("--toy-backend" in cmd for cmd in commands)):
                problems.append("toy mode compiler invocation invalid: " + key)
            invocation = row.get("lower_invocation")
            if (not isinstance(invocation, list) or len(invocation) < 4 or
                    not all(isinstance(x, str) for x in invocation) or
                    invocation[0] != os.path.realpath(toy_backend_bin or "") or
                    "-o" not in invocation or
                    any(os.path.basename(x) in ("llc","clang","gcc","clang-S","gcc-S")
                        for x in invocation) or
                    any(x == "-S" for x in invocation)):
                problems.append("toy lowering invocation is not self-built: " + key)
        if no_structured and not expected.get(key, False):
            commands = row.get('compile_invocations')
            if not isinstance(commands, list) or len(commands) != 2 or (
                    not all(isinstance(cmd, list) for cmd in commands)) or (
                    '--emit=flat-ir' not in commands[0]) or (
                    '--from-flat' not in commands[1]) or (
                    '--emit=llvm-ir' not in commands[1]):
                problems.append('缺少两个独立 Flat compiler invocations: ' + key)
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
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--report', required=True)
    parser.add_argument('--no-structured', action='store_true')
    parser.add_argument('--toy-backend', action='store_true')
    parser.add_argument('--toy-backend-bin', default=str(Path(__file__).resolve().parents[2] / 'build/toy_backend'))
    parser.add_argument('--target', required=True, choices=['x86', 'aarch64', 'riscv64'])
    args = parser.parse_args()
    try:
        expected = manifest_cases(args.manifest)
        report = json.loads(Path(args.report).read_text())
        revision = subprocess.check_output(['git', '-C', str(Path(__file__).resolve().parents[2]),
                                            'rev-parse', 'HEAD'], text=True).strip()
        digest = hashlib.sha256(Path(args.manifest).read_bytes()).hexdigest()
        problems = validate(report, expected, args.target, revision=revision,
                            manifest_sha256=digest, no_structured=args.no_structured,
                            expected_compiler=args.compiler, expected_manifest=args.manifest,
                            toy_backend=args.toy_backend, toy_backend_bin=args.toy_backend_bin)
    except (OSError, ValueError, TypeError, subprocess.CalledProcessError) as error:
        print('FAIL: 执行证据无法读取: ' + str(error))
        return 1
    for problem in problems[:10]:
        print('FAIL: ' + problem)
    if not problems:
        print('执行身份通过: 490 必需通过 / 50 清单内 tensor 排除')
    return int(bool(problems))


if __name__ == '__main__':
    raise SystemExit(main())
