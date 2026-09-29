#!/usr/bin/env python3
"""CLI 能力契约：未实现不是成功，诊断不能覆盖既有输出。

以后实现 S07 时允许真实 LLVM 定义；空文件/纯注释仍不能冒充成功。
结构化往返覆盖全部可调用运行时签名，补充目录依赖重构的行为验证。
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

MARKER = 'error[E-UNSUPPORTED-LLVM]'


def check(compiler):
    with tempfile.TemporaryDirectory(prefix='sysy_cli_contract_') as directory:
        work = Path(directory)
        source = work / 'case.sy'
        source.write_text('int main(){return 42;}\n')
        output = work / 'case.ll'
        for options in ([], ['--emit=llvm-ir']):
            for existing in (False, True):
                output.unlink(missing_ok=True)
                if existing:
                    output.write_bytes(b'existing artifact\n')
                result = subprocess.run([compiler, str(source), '-o', str(output), *options],
                                        capture_output=True, text=True, timeout=20)
                if result.returncode == 2 and MARKER in result.stderr:
                    assert not result.stdout, 'unsupported emitter wrote stdout'
                    if existing:
                        assert output.read_bytes() == b'existing artifact\n', 'old output lost'
                    else:
                        assert not output.exists(), 'unsupported emitter created output'
                else:
                    assert result.returncode == 0, result.stderr
                    assert output.exists(), 'successful emitter did not create output'
                    assert any(line.startswith('define ') for line in output.read_text().splitlines()), \
                        'successful emitter produced no LLVM function'
        for kind in ('ast', 'sema', 'structured-ir', 'flat-ir'):
            result = subprocess.run([compiler, str(source), '--emit=' + kind, '-o', str(output)],
                                    capture_output=True, text=True, timeout=20)
            assert result.returncode == 0 and output.stat().st_size > 0, (kind, result.stderr)
        source.write_text('''int main(){
  int a[2]; float b[2]; int x; float y;
  starttime(); x=getint(); x=x+getch(); x=x+getarray(a);
  y=getfloat(); x=x+getfarray(b); putint(x); putch(x);
  putarray(2,a); putfloat(y); putfarray(2,b); stoptime(); return x;
}\n''')
        roundtrip = work / 'roundtrip.sir'
        for command in ([compiler, str(source), '--emit=structured-ir', '-o', str(output)],
                        [compiler, str(output), '--from-structured', '--emit=structured-ir',
                         '-o', str(roundtrip)]):
            result = subprocess.run(command, capture_output=True, text=True, timeout=20)
            assert result.returncode == 0 and not result.stderr, result.stderr
        assert output.read_bytes() == roundtrip.read_bytes(), 'runtime calls changed on readback'
    print('CLI capability and runtime-call roundtrip checks passed')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', default=str(Path(__file__).resolve().parents[2] / 'build/compiler'))
    check(str(Path(parser.parse_args().compiler).resolve()))
