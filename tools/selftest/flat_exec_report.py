"""Classify and print every structured-vs-flat trace difference."""

import os


def report_diffs(diffs, root):
    result_class, count_class = [], []
    for source, message, structured, flat in diffs:
        observables_same = (structured is not None and flat is not None and
                            structured['out'] == flat['out'] and
                            structured['mem'] == flat['mem'] and
                            structured['ret'] == flat['ret'])
        target = count_class if observables_same else result_class
        target.append((os.path.relpath(source, root), message))
    print('--- 结果类差异（`out`/`mem`/`ret` 至少一项不同）: %d ---' % len(result_class))
    for rel, message in result_class:
        print('  ✘ %s\n    %s' % (rel, message))
    print('--- 仅循环计数口径: %d ---' % len(count_class))
    for rel, _message in count_class:
        print('  · %s' % rel)
