"""Expected corpus identity and frontend rejects for the flat behavior gate."""

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


def corpus_matches(files, directory, limit):
    if limit or os.path.abspath(directory) != os.path.join(ROOT, 'tests'):
        return True
    return {os.path.relpath(path, ROOT) for path in files} == set(EXPECTED)
