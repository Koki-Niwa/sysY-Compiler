"""S11b self-built parser and actual x86 executable regression fixtures (no llc)."""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[2]
TOY = ROOT / 'build/toy_backend'
RUNTIME = ROOT.parent / 'runtime/sylib.c'


def ir(text):
    return textwrap.dedent(text).strip() + '\n'


class ToyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not TOY.is_file():
            raise unittest.SkipTest('build/toy_backend must be built')

    def lower(self, code, source=None, do_link=True, extra_c=None):
        with tempfile.TemporaryDirectory(prefix='toy_fixture_') as folder:
            wd = Path(folder)
            source_path = wd / 'test.ll'
            source_path.write_text(ir(code))
            args = [str(TOY), str(source_path), '-o', str(wd / 'case.s'),
                    '--report', str(wd / 'contract.json')]
            p = subprocess.run(args, capture_output=True, text=True, timeout=15)
            if source is None:
                self.assertEqual(0, p.returncode, p.stderr)
                report = json.loads((wd / 'contract.json').read_text())
                self.assertEqual('self-built-toy-x86', report['backend'])
                self.assertEqual(hashlib.sha256(source_path.read_bytes()).hexdigest(),
                                 report['input_sha256'])
            else:
                self.assertNotEqual(0, p.returncode)
                self.assertIn('line ', p.stderr)
                self.assertIn('source:', p.stderr)
                self.assertIn(source, p.stderr)
                return p.stderr
            asm = (wd / 'case.s').read_text()
            if not do_link:
                return asm
            cmd = ['clang', str(wd / 'case.s'), '-w', '-o', str(wd / 'a'), '-lm']
            if RUNTIME.is_file():
                cmd.insert(2, str(RUNTIME))
            if extra_c:
                (wd / 'extra.c').write_text(extra_c)
                cmd.insert(2, str(wd / 'extra.c'))
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=20)
            self.assertEqual(0, r.returncode, r.stderr)
            proc = subprocess.run([str(wd / 'a')], capture_output=True, timeout=20)
            return proc.returncode, proc.stdout, proc.stderr

    def assertExit(self, text, value):
        rc, out, err = self.lower(text)
        self.assertEqual(value, rc, (out, err))

    def test_integer_branch_and_select(self):
        self.assertExit("""
        define i32 @main() {
        bb0:
          %a = mul i32 6, 7
          %v = icmp sgt i32 %a, 40
          br i1 %v, label %bb1, label %bb2
        bb1:
          %x = select i1 true, i32 9, i32 1
          ret i32 %x
        bb2:
          ret i32 0
        }
        """, 9)

    def test_phi_merge_and_cycle(self):
        self.assertExit("""
        define i32 @main() {
        bb0:
          br label %bb1
        bb1:
          %a = phi i32 [ 1, %bb0 ], [ %b, %bb2 ]
          %b = phi i32 [ 2, %bb0 ], [ %a, %bb2 ]
          %n = phi i32 [ 0, %bb0 ], [ %next, %bb2 ]
          %cond = icmp slt i32 %n, 1
          br i1 %cond, label %bb2, label %bb3
        bb2:
          %next = add i32 %n, 1
          br label %bb1
        bb3:
          %res = sub i32 %a, %b
          ret i32 %res
        }
        """, 1)

    def test_recursion_and_internal_call(self):
        self.assertExit("""
        define i32 @factorial(i32 %n) {
        bb0:
          %base = icmp sle i32 %n, 1
          br i1 %base, label %bb1, label %bb2
        bb1:
          ret i32 1
        bb2:
          %prev = sub i32 %n, 1
          %call = call i32 @factorial(i32 %prev)
          %prod = mul i32 %n, %call
          ret i32 %prod
        }
        define i32 @main() {
        bb0:
          %result = call i32 @factorial(i32 5)
          ret i32 %result
        }
        """, 120)

    def test_eight_integer_arguments(self):
        a = ', '.join(f'i32 %a{i}' for i in range(8))
        call = ', '.join(f'i32 {i}' for i in range(1, 9))
        body = '\n'.join(f'  %t{i} = add i32 %t{i-1}, %a{i}' for i in range(1, 8))
        self.assertExit(f"""
        define i32 @sum({a}) {{
        bb0:
          %t0 = add i32 %a0, 0
        {body}
          ret i32 %t7
        }}
        define i32 @main() {{
        bb0:
          %result = call i32 @sum({call})
          ret i32 %result
        }}
        """, 36)

    def test_mixed_fp_integer_and_fp_register_overflow(self):
        self.assertExit("""
        define float @math(i32 %n, float %x, i32 %m, float %y) {
        bb0:
          %nf = sitofp i32 %n to float
          %a = fadd float %nf, %x
          %b = fadd float %a, %y
          ret float %b
        }
        define i32 @main() {
        bb0:
          %f = call float @math(i32 3, float 2.0, i32 0, float 4.0)
          %i = fptosi float %f to i32
          ret i32 %i
        }
        """, 9)
        p = ', '.join(f'float %f{i}' for i in range(9))
        args = ', '.join(f'float {i}.0' for i in range(9))
        self.assertExit(f"""
        define float @edge({p}) {{
        bb0:
          %a = fadd float %f0, %f8
          ret float %a
        }}
        define i32 @main() {{
        bb0:
          %a = call float @edge({args})
          %b = fptosi float %a to i32
          ret i32 %b
        }}
        """, 8)

    def test_globals_gep_and_sparse_bss(self):
        self.assertExit("""
        @arr = global [3 x i32] [i32 1, i32 3, i32 4], align 4
        @scalar = global i32 6, align 4
        define i32 @main() {
        bb0:
          %ptr = getelementptr i8, ptr @arr, i64 4
          %v = load i32, ptr %ptr, align 4
          %x = load i32, ptr @scalar, align 4
          %r = add i32 %v, %x
          ret i32 %r
        }
        """, 9)
        asm = self.lower("""
        @big = global [412500000 x i32] zeroinitializer, align 4
        define i32 @main() {
        bb0:
          ret i32 0
        }
        """, do_link=False)
        self.assertLess(len(asm), 2048)
        self.assertIn('.zero 1650000000', asm)

    def test_hex_float_nan_unordered_and_signed_zero(self):
        self.assertExit("""
        define i32 @main() {
        bb0:
          %n = fdiv float 0.000000e+00, 0.000000e+00
          %uneq = fcmp une float %n, 0x3FF0000000000000
          %eq = fcmp oeq float %n, %n
          %minus = fneg float 0.000000e+00
          %zero = fcmp oeq float %minus, 0.000000e+00
          %a = zext i1 %uneq to i32
          %b = zext i1 %eq to i32
          %c = zext i1 %zero to i32
          %r = add i32 %a, %c
          %z = sub i32 %r, %b
          ret i32 %z
        }
        """, 2)

    def test_memcpy_memset_and_float_bit_width(self):
        self.assertExit("""
        declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)
        declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)
        define i32 @main() {
        bb0:
          %a = alloca [4 x i32], align 4
          %b = alloca [4 x i32], align 4
          call void @llvm.memset.p0.i64(ptr %a, i8 0, i64 16, i1 false)
          %p = getelementptr i8, ptr %a, i64 4
          store i32 17, ptr %p, align 4
          call void @llvm.memcpy.p0.p0.i64(ptr %b, ptr %a, i64 16, i1 false)
          %q = getelementptr i8, ptr %b, i64 4
          %value = load i32, ptr %q, align 4
          ret i32 %value
        }
        """, 17)

    def test_min_division_overflow_guard(self):
        for opcode, expected in [('sdiv', -2147483648), ('srem', 0)]:
            with self.subTest(opcode=opcode):
                self.assertExit(f"""
                define i32 @main() {{
                bb0:
                  %r = {opcode} i32 -2147483648, -1
                  %b = icmp eq i32 %r, {expected}
                  %v = zext i1 %b to i32
                  ret i32 %v
                }}
                """, 1)

    def test_i64_arithmetic_shift_and_float_load_store(self):
        self.assertExit("""
        define i32 @main() {
        bb0:
          %a = alloca float, align 4
          store float 0x4008000000000000, ptr %a, align 4
          %f = load float, ptr %a, align 4
          %i = fptosi float %f to i32
          %w = sext i32 %i to i64
          %m = mul i64 %w, 2
          %shifted = shl i64 %m, 2
          %sh = ashr i64 %shifted, 1
          %n = trunc i64 %sh to i32
          ret i32 %n
        }
        """, 12)

    def test_variadic_fpext_abi(self):
        text = """
        declare i32 @variad(ptr, ...)
        define i32 @main() {
        bb0:
          %x = fpext float 0x400C000000000000 to double
          %v = call i32 @variad(ptr null, double %x)
          ret i32 %v
        }
        """
        extra = ('#include <stdarg.h>\n'
                 'int variad(void* p, ...) { (void)p; va_list a; va_start(a,p); '
                 'double x=va_arg(a,double); va_end(a); return x==3.5 ? 7:0; }\n')
        rc, out, err = self.lower(text, extra_c=extra)
        self.assertEqual(7, rc, (out, err))

    def test_quoted_symbol(self):
        self.assertExit(r"""
        define i32 @"s\20p"() {
        bb0:
          ret i32 7
        }
        define i32 @main() {
        bb0:
          %r = call i32 @"s\20p"()
          ret i32 %r
        }
        """, 7)

    def test_parser_fail_closed(self):
        negative = {
            'unknown_opcode': ('%x = fma float 1.0, 2.0', 'fma'),
            'inbounds': ('%x = getelementptr inbounds i8, ptr null, i64 0', 'inbounds'),
            'nsw': ('%x = add nsw i32 1, 2', 'nsw'),
            'fast': ('%x = fadd fast float 1.0, 2.0', 'fast'),
            'bad_type': ('%x = add i17 1, 2', 'i17'),
            'bad_gep_width': ('%x = getelementptr i8, ptr null, i32 1', 'i32'),
            'unknown_use': ('%x = add i32 %missing, 2', '%missing'),
        }
        for key, (bad, snippet) in negative.items():
            with self.subTest(key=key):
                self.lower(f"define i32 @main() {{\nbb0:\n  {bad}\n  ret i32 0\n}}",
                           source=snippet)
        self.lower("""define i32 @main() {
        bb0:
          %a = add i32 1, 2
          %a = sub i32 2, 1
          ret i32 %a
        }""", source='duplicate local')
        self.lower("""define i32 @main() {
        bb0:
          %a = add i32 1, 2
        }""", source='terminator')
        self.lower("""define i32 @main() {
        bb0:
          br label %bb1
        bb1:
          %p = phi i32 [ 1, %bb2 ]
          ret i32 %p
        bb2:
          ret i32 0
        }""", source='phi')
        self.lower("""define i32 @main() {
        bb0:
          %v = add i32 1, 2
          %p = phi i32 [ 1, %bb0 ]
          ret i32 %p
        }""", source='phi')
        self.lower('target triple = "x86_64"', source='target triple')
        self.lower('declare void @unfinished(ptr', source='declare void')
        self.lower("""define i32 @main() {
        bb0:
          br label %bb1
        bb1:
          %x = alloca i32, align 4
          ret i32 0
        }""", source='alloca')
        self.lower("""define void @main() {
        bb0:
          ret void
        }""", source='define void')


if __name__ == '__main__':
    unittest.main()
