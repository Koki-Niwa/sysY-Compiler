"""Flat IR roundtrip and GCC anchored behavior checks for run_flat_cases."""

import os
import subprocess
import tempfile

import arena
from flat_exec import FlatExec
from flat_mod import FlatMod


SEED = 12345
INPUT_COUNT = 64


def _diagnostic(result):
    lines = result.stderr.decode('utf-8', 'replace').strip().splitlines()
    return lines[0] if lines else ''


def check(compiler, source):
    """Return (failures, flat dump size) for one source file."""
    failures = []
    with tempfile.TemporaryDirectory(prefix='flat_case_') as tmp:
        flat = os.path.join(tmp, 'case.flat')
        roundtrip = os.path.join(tmp, 'roundtrip.flat')
        emitted = subprocess.run([compiler, source, '--emit=flat-ir', '-o', flat],
                                 capture_output=True)
        if emitted.returncode or emitted.stderr.strip():
            return (['flat 编译 rc=%d %s' % (emitted.returncode,
                                          _diagnostic(emitted))], 0)
        if not os.path.isfile(flat):
            return (['flat 编译器没产出文件'], 0)
        with open(flat, 'rb') as stream:
            data = stream.read()
        if not data.strip():
            return (['flat 产物为空'], 0)

        reread = subprocess.run([compiler, '--from-flat', flat, '--emit=flat-ir',
                                 '-o', roundtrip], capture_output=True)
        if reread.returncode or reread.stderr.strip():
            failures.append('flat 往返 rc=%d %s' % (reread.returncode,
                                                    _diagnostic(reread)))
        elif not os.path.isfile(roundtrip):
            failures.append('flat 往返没产出文件')
        else:
            with open(roundtrip, 'rb') as stream:
                if stream.read() != data:
                    failures.append('flat 往返与产物不同')

        # A stable dump can still execute incorrectly: use GCC as independent truth.
        try:
            gcc_rc, gcc_out = arena.gcc_truth(source, INPUT_COUNT, SEED, tmp)
            if gcc_rc < 0:
                failures.append('gcc 被信号 %d 终止' % -gcc_rc)
            else:
                trace = FlatExec(FlatMod(data.decode('utf-8')), SEED).run()
                flat_rc = int(trace['ret']) & 0xff
                flat_out = arena.stdout_of(trace)
                if flat_rc != gcc_rc:
                    failures.append('flat 返回值 %d 与 gcc %d 不同' %
                                    (flat_rc, gcc_rc))
                if flat_out != gcc_out:
                    failures.append('flat 输出 %r 与 gcc %r 不同' %
                                    (flat_out[:120], gcc_out[:120]))
        except Exception as error:  # Report interpreter and subprocess errors per case.
            failures.append('flat/gcc 行为检查失败：%s: %s' %
                            (type(error).__name__, error))
        return failures, len(data)
