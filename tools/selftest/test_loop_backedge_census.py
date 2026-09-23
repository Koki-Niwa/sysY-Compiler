#!/usr/bin/env python3
"""Focused regressions for structured paths that do or do not need a backedge."""

import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import loop_backedge_census as census
from flat_mod import FlatMod


ROOT = Path(census.ROOT)
COMPILER = ROOT / 'compiler/build/compiler'


def dumps(source, outdir):
    sir = outdir / 'input.sir'
    flat = outdir / 'input.flat'
    subprocess.run([str(COMPILER), str(source), '--emit=structured-ir',
                    '--normalize', '-o', str(sir)], check=True, capture_output=True)
    subprocess.run([str(COMPILER), str(source), '--emit=flat-ir', '-o', str(flat)],
                   check=True, capture_output=True)
    return sir.read_text(), flat.read_text()


class BackedgeCensusTests(unittest.TestCase):
    def test_break_only_outer_loops(self):
        with tempfile.TemporaryDirectory() as tmp:
            outdir = Path(tmp)
            for name, required in [('04_break_continue.sy', 5),
                                   ('26_scope4.sy', 2)]:
                with self.subTest(name=name):
                    source = ROOT / 'tests/final_arm/h_functional' / name
                    sir, flat = dumps(source, outdir)
                    need, heads, _detail, bad = census.inspect_ir(sir, flat)
                    self.assertEqual((need, heads, bad), (required, required, ''))

    def test_missing_real_backedge_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            outdir = Path(tmp)
            source = outdir / 'loop.sy'
            source.write_text('int main(){int i=0;while(i<2){i=i+1;}return i;}\n')
            sir, flat = dumps(source, outdir)
            self.assertEqual(census.inspect_ir(sir, flat)[:2], (1, 1))

            fm = FlatMod(flat)
            blocks = fm.funcs['main'][0]
            head = fm.loops['main'][0]
            dom = fm._dominators(blocks, fm.preds['main'])
            src = next(b for b, insts in blocks.items()
                       if b != head and head in dom[b] and
                       any(it.kind == 'br' and head in it.blocks for it in insts))
            exit_block = blocks[head][-1].blocks[-1]
            current = None
            lines = flat.splitlines(keepends=True)
            for i, line in enumerate(lines):
                label = re.match(r'L(\d+):', line)
                if label:
                    current = int(label.group(1))
                if current == src and re.match(r'\s+br L%d(?:\s|$)' % head, line):
                    lines[i] = re.sub(r'\bL%d\b' % head, 'L%d' % exit_block, line)
                    break
            else:
                self.fail('could not locate the compiler-produced backedge')

            need, heads, _detail, bad = census.inspect_ir(sir, ''.join(lines))
            self.assertEqual((need, heads), (1, 0))
            self.assertIn('main:所需1/平面0', bad)


if __name__ == '__main__':
    unittest.main()
