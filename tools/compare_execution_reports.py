#!/usr/bin/env python3
"""Fail-closed comparison of existing S11 execution reports (no compilation)."""
import argparse
import json
import math
import statistics
from pathlib import Path

PASSED = {'PASS', 'PASS(FORMAT_C1)', 'PASS(FORMAT_C2/C3)'}

def compare(base, candidate):
    def require(ok, message):
        if not ok:
            raise ValueError(message)
    require(all(isinstance(x, dict) for x in (base, candidate)), 'invalid report')
    for name, report in (('baseline', base), ('candidate', candidate)):
        require(report.get('schema_version') == 1, name + ': schema_version')
        require(report.get('summary', {}).get('fail') == 0 and
                report.get('summary', {}).get('timeout') == 0, name + ': correctness summary')
        m = report.get('measurement', {})
        require(isinstance(m, dict) and type(m.get('repeat_count')) is int and
                m['repeat_count'] >= 3 and type(m.get('warmup_count')) is int and
                m['warmup_count'] >= 0 and m.get('statistic') == 'median' and
                m.get('spread_kind') == 'mad', name + ': measurement protocol')
        require(m.get('metric_scope') in ('native-host', 'qemu-proxy') and
                m.get('real_target_performance') is False, name + ': metric scope')
        rows = report.get('results')
        require(isinstance(rows, list) and len(rows) > 0, name + ': results')
        keys = [r.get('key') for r in rows]
        require(all(isinstance(k, str) for k in keys) and len(set(keys)) == len(keys),
                name + ': duplicate/invalid case identity')
        for r in rows:
            require(r.get('status') in PASSED, name + ': correctness failed: ' + str(r.get('key')))
            samples = r.get('run_samples_seconds')
            require(r.get('run_protocol') == {'warmup_count': m['warmup_count'],
                    'repeat_count': m['repeat_count']}, name + ': row protocol')
            require(isinstance(samples, list) and len(samples) == m['repeat_count'] and
                    all(type(v) in (float, int) and math.isfinite(v) and v > 0 for v in samples),
                    name + ': missing/invalid samples')
            median = statistics.median(samples)
            mad = statistics.median(abs(v - median) for v in samples)
            require(math.isclose(r.get('run_median_seconds', -1), median, rel_tol=1e-9, abs_tol=1e-12) and
                    r.get('run_spread', {}).get('kind') == 'mad' and
                    math.isclose(r['run_spread'].get('seconds', -1), mad, rel_tol=1e-9, abs_tol=1e-12),
                    name + ': invalid statistics')
    require(base['target'] == candidate['target'], 'different target')
    require(base.get('manifest', {}).get('sha256') and
            base['manifest']['sha256'] == candidate.get('manifest', {}).get('sha256'), 'different manifest hash')
    for key in ('toolchain',):
        require(base.get(key) == candidate.get(key), 'different toolchain facts')
    require(all(base.get('mode', {}).get(k) == candidate.get('mode', {}).get(k)
                for k in ('target', 'boundary', 'no_structured')), 'different compilation boundary')
    bm, cm = base['measurement'], candidate['measurement']
    require(all(bm.get(k) == cm.get(k) for k in
                ('warmup_count', 'repeat_count', 'statistic', 'spread_kind', 'metric_scope',
                 'target_hardware_runtime', 'real_target_performance')), 'different measurement protocol')
    br = {r['key']: r for r in base['results']}
    cr = {r['key']: r for r in candidate['results']}
    require(br.keys() == cr.keys(), 'different case identities')
    per = []
    for key in sorted(br):
        a, b = br[key], cr[key]
        require(all(a.get(f) and a.get(f) == b.get(f) for f in
                    ('source_sha256', 'stdin_sha256', 'expected_sha256') if f != 'stdin_sha256' or
                    a.get(f) is not None or b.get(f) is not None), 'different source/input/expected hashes: ' + key)
        require(a.get('stdin_sha256') == b.get('stdin_sha256'), 'different stdin hash: ' + key)
        require(all(type(r.get('executable_bytes')) is int and r['executable_bytes'] >= 0
                    for r in (a, b)), 'missing size: ' + key)
        ratio = a['run_median_seconds'] / b['run_median_seconds']
        per.append({'key': key, 'baseline_median_seconds': a['run_median_seconds'],
                    'candidate_median_seconds': b['run_median_seconds'],
                    'speedup': ratio, 'baseline_executable_bytes': a['executable_bytes'],
                    'candidate_executable_bytes': b['executable_bytes'],
                    'size_delta_bytes': b['executable_bytes'] - a['executable_bytes']})
    worst = min(per, key=lambda r: (r['speedup'], r['key']))
    best = max(per, key=lambda r: (r['speedup'], r['key']))
    return {'schema_version': 1,
            'baseline': {'revision': base['compiler'].get('revision'), 'compiler_sha256': base['compiler'].get('executable_sha256'), 'mode': base.get('mode')},
            'candidate': {'revision': candidate['compiler'].get('revision'), 'compiler_sha256': candidate['compiler'].get('executable_sha256'), 'mode': candidate.get('mode')},
            'target': base['target'], 'metric_scope': bm['metric_scope'],
            'target_hardware_runtime': bm['target_hardware_runtime'],
            'real_target_performance': False,
            'measurement_protocol': {k: bm[k] for k in ('warmup_count', 'repeat_count', 'statistic', 'spread_kind')},
            'case_count': len(per), 'per_case': per,
            'aggregate': {'geomean_speedup': math.exp(statistics.mean(math.log(r['speedup']) for r in per)),
                          'worst_speedup': worst['speedup'], 'worst_case': worst['key'],
                          'best_speedup': best['speedup'], 'best_case': best['key']}}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('baseline')
    parser.add_argument('candidate')
    parser.add_argument('--json', help='write comparison JSON to file (otherwise stdout)')
    args = parser.parse_args()
    try:
        result = compare(json.loads(Path(args.baseline).read_text()),
                         json.loads(Path(args.candidate).read_text()))
    except (OSError, ValueError, TypeError, KeyError, ZeroDivisionError, OverflowError) as exc:
        parser.exit(1, 'FAIL: ' + str(exc) + '\n')
    rendered = json.dumps(result, ensure_ascii=False, indent=2)
    if args.json:
        Path(args.json).write_text(rendered + '\n')
    else:
        print(rendered)

if __name__ == '__main__':
    main()
