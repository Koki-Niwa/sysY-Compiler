"""Classify and print every structured-vs-flat trace difference."""

import os

OBSERVABLE_KEYS = ('out', 'mem', 'ret', 'effects', 'input_state')


def observable_same(structured, flat):
    return all(structured[key] == flat[key] for key in OBSERVABLE_KEYS)


def report_diffs(diffs, root):
    result_class, count_class = [], []
    for source, message, structured, flat in diffs:
        target = count_class if observable_same(structured, flat) else result_class
        target.append((os.path.relpath(source, root), message))
    print('--- 可观察差异（输出/内存/返回/写入序列/输入状态）: %d ---' % len(result_class))
    for rel, message in result_class:
        print('  ✘ %s\n    %s' % (rel, message))
    print('--- 仅循环计数口径: %d ---' % len(count_class))
    for rel, _message in count_class:
        print('  · %s' % rel)
