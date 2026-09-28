"""Expected corpus identity and frontend rejects for the flat behavior gate.

★ 为什么"逐文件身份"要单独有一条判据：只比各桶计数时，**两个文件错位会
  互相抵消**——一个本该产出的用例被前端拒绝（ok 少 1、frontend 多 1），
  同时一个本该被拒绝的 tensor 用例意外产出（ok 多 1、frontend 少 1），
  计数与清单完全一致，而两个文件都没被正确判定。S06 审查把这一条
  列为验收风险；`identity_mismatches()` 是它的可执行版本。
"""

import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
MANIFEST = os.path.join(ROOT, 'tests', 'manifest.tsv')


def entries():
    result = {}
    with open(MANIFEST, encoding='utf-8') as stream:
        for line in stream:
            fields = line.rstrip('\n').split('\t')
            if len(fields) == 7 and not fields[0].startswith('#'):
                result[fields[3]] = fields[6]
    return result


EXPECTED = entries()


def expected_reject(source):
    return EXPECTED.get(os.path.relpath(source, ROOT)) == 'tensor'


def corpus(root):
    """`<root>/tests` 下的全部 `.sy`，返回按工作区根取的相对路径（排序）。"""
    out = []
    for dirpath, _, files in os.walk(os.path.join(root, 'tests')):
        for f in files:
            if f.endswith('.sy'):
                out.append(os.path.relpath(os.path.join(dirpath, f), root))
    return sorted(out)


def identity_mismatches(buckets, root, only=None):
    """逐文件核对桶归属，返回 `[(相对路径, 清单期望, 实际桶)]`。

    只看"通过 / 前端拒绝"两种归属：其余状态（`verify`/`invariant`/`crash`…）
    本来就让计数对不上清单，错位判据不重复报。清单里多出的条目与树里多出的
    文件同样算错位。`--only` 是有意取子集、`--root` 指向别处时都不判。
    """
    if only or os.path.abspath(root) != ROOT or not os.path.isdir(os.path.join(root, 'tests')):
        return []
    want = dict((rel, 'frontend' if skip == 'tensor' else 'ok')
                for rel, skip in EXPECTED.items())
    got = {}
    for status, rows in buckets.items():
        for rel, _detail in rows:
            got[rel] = 'frontend' if status == 'frontend' else 'ok'
    bad = []
    for rel in sorted(set(want) | set(got)):
        if want.get(rel, '缺失') != got.get(rel, '缺失'):
            bad.append((rel, want.get(rel, '缺失'), got.get(rel, '缺失')))
    return bad


def with_identity_bucket(buckets, mism):
    """把身份错位的文件**移出原桶**并放进 `identity` 桶（返回新的桶表）。

    为什么不是"另加一个桶、原桶不动"：关卡只解析 ok/frontend 两个计数，
    错位恰好互相抵消时计数与清单仍然一致 ⇒ 必须把文件移出原桶，计数才会
    对不上（`test_flat_identity_gate.py` 对这一条有反证）。
    """
    wrong = set(rel for rel, _w, _g in mism)
    out = dict((st, [(rel, d) for rel, d in rows if rel not in wrong])
               for st, rows in buckets.items())
    out['identity'] = [(rel, '清单=%s 实际=%s' % (w, g)) for rel, w, g in mism]
    return out


def corpus_matches(files, directory, limit):
    if limit or os.path.abspath(directory) != os.path.join(ROOT, 'tests'):
        return True
    return {os.path.relpath(path, ROOT) for path in files} == set(EXPECTED)
