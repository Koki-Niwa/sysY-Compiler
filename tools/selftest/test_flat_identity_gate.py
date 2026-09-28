#!/usr/bin/env python3
"""逐文件身份判据的反证（S06 审查记的"两个文件错位会互相抵消"）。

`check_flat.py` 原先只把结果分成各桶、由关卡比对**计数**与 `manifest.tsv`
是否一致。一个本该产出的用例被前端拒绝（ok −1、frontend +1）同时一个
tensor 用例意外产出（ok +1、frontend −1）时，计数完全一致而两个文件都没
被正确判定。本文件证明新的 `identity_mismatches()` 抓得住这一类：

    python3 compiler/tools/selftest/test_flat_identity_gate.py

退出码 0 = 判据在四种错位上都报红、在正确桶上不误报。
"""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import flat_exec_manifest as M  # noqa: E402


class IdentityGateTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='flat_identity_')
        self.root = self.tmp.name
        os.makedirs(os.path.join(self.root, 'tests', 'functional'))
        self.ok = 'tests/functional/a.sy'
        self.tensor = 'tests/functional/b.sy'
        for rel in (self.ok, self.tensor):
            with open(os.path.join(self.root, rel), 'w', encoding='utf-8') as f:
                f.write('int main(){return 0;}\n')
        self.saved = (M.ROOT, M.EXPECTED)
        M.ROOT = self.root
        M.EXPECTED = {self.ok: '-', self.tensor: 'tensor'}

    def tearDown(self):
        M.ROOT, M.EXPECTED = self.saved
        self.tmp.cleanup()

    def buckets(self, ok_bucket, frontend_bucket):
        return {'ok': [(r, '') for r in ok_bucket],
                'frontend': [(r, '') for r in frontend_bucket]}

    def test_correct_buckets_are_not_flagged(self):
        bad = M.identity_mismatches(self.buckets([self.ok], [self.tensor]), self.root)
        self.assertEqual([], bad, '正确的桶归属不许误报')

    def test_swap_cancels_in_counts_but_is_flagged(self):
        """★ 核心反证：两边计数都不变（ok=1、frontend=1），身份却都错了。"""
        buckets = self.buckets([self.tensor], [self.ok])
        self.assertEqual(1, len(buckets['ok']))
        self.assertEqual(1, len(buckets['frontend']))
        bad = M.identity_mismatches(buckets, self.root)
        self.assertEqual([(self.ok, 'ok', 'frontend'), (self.tensor, 'frontend', 'ok')],
                         sorted(bad))
        # 移出原桶之后，计数与清单不再一致（关卡比对的就是这两个计数）
        moved = M.with_identity_bucket(buckets, bad)
        self.assertEqual([], moved['ok'])
        self.assertEqual([], moved['frontend'])
        self.assertEqual([self.ok, self.tensor], sorted(r for r, _d in moved['identity']))

    def test_tensor_case_that_compiles_is_flagged(self):
        bad = M.identity_mismatches(self.buckets([self.ok, self.tensor], []), self.root)
        self.assertEqual([(self.tensor, 'frontend', 'ok')], bad)

    def test_missing_and_extra_files_are_flagged(self):
        os.remove(os.path.join(self.root, self.ok))          # 清单里有、树里没有
        extra = 'tests/functional/c.sy'                       # 树里有、清单里没有
        with open(os.path.join(self.root, extra), 'w', encoding='utf-8') as f:
            f.write('int main(){return 0;}\n')
        bad = M.identity_mismatches(self.buckets([extra], [self.tensor]), self.root)
        self.assertEqual([(self.ok, 'ok', '缺失'), (extra, '缺失', 'ok')], sorted(bad))

    def test_subset_and_foreign_root_are_not_judged(self):
        buckets = self.buckets([self.tensor], [self.ok])
        self.assertEqual([], M.identity_mismatches(buckets, self.root, only='a'))
        self.assertEqual([], M.identity_mismatches(buckets, '/nonexistent'))


if __name__ == '__main__':
    unittest.main(verbosity=2)
