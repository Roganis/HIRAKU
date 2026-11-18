#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""hiraku-server's API description: docs/openapi.yaml (hand-written) and its JSON twin docs/openapi.json
(made here), which the server serves too (GET /api/v1/openapi.json) and the test eamuse_openapi reads
where Python has no YAML module. Run it after every change to the YAML. Needs PyYAML.

    tools/eamuse/openapi.py            write docs/openapi.json from docs/openapi.yaml
    tools/eamuse/openapi.py --check    exit 1 when docs/openapi.json isn't docs/openapi.yaml's

The YAML is read strictly: a key given twice in one mapping, or a value YAML would turn into
something JSON hasn't (a date), stops it with the line.
"""
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
YAML = os.path.join(ROOT, 'docs', 'openapi.yaml')
JSON = os.path.join(ROOT, 'docs', 'openapi.json')


def load_yaml(path):
    """the document, a key twice in a mapping refused (PyYAML keeps the last one silently)"""
    import yaml

    class Strict(yaml.SafeLoader):
        pass

    def mapping(loader, node, deep=False):
        keys = set()
        for k, _ in node.value:
            key = loader.construct_object(k, deep=deep)
            if key in keys:
                raise SystemExit(f'{path}:{k.start_mark.line + 1}: the key {key!r} twice')
            keys.add(key)
        return loader.construct_mapping(node, deep)
    Strict.add_constructor(yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, mapping)
    with open(path, encoding='utf-8') as f:
        return yaml.load(f, Loader=Strict)


def plain(v, where='$'):
    """v made only of what JSON has (a text key for every mapping), else stop with where"""
    if isinstance(v, dict):
        for k in v:
            if not isinstance(k, str):
                raise SystemExit(f'{YAML}: {where}: the key {k!r} is not a text (quote it)')
        return {k: plain(x, f'{where}.{k}') for k, x in v.items()}
    if isinstance(v, list):
        return [plain(x, f'{where}[{i}]') for i, x in enumerate(v)]
    if v is None or isinstance(v, (bool, int, float, str)):
        return v
    raise SystemExit(f'{YAML}: {where}: {type(v).__name__} is not JSON (quote it)')


def twin_text(doc):
    return json.dumps(doc, indent=1, ensure_ascii=False) + '\n'


def main():
    check = sys.argv[1:] == ['--check']
    if sys.argv[1:] and not check:
        raise SystemExit(__doc__)
    text = twin_text(plain(load_yaml(YAML)))
    if check:
        try:
            with open(JSON, encoding='utf-8') as f:
                same = f.read() == text
        except OSError:
            same = False
        print('docs/openapi.json is docs/openapi.yaml\'s' if same else 'docs/openapi.json is out of date: run tools/eamuse/openapi.py')
        return 0 if same else 1
    with open(JSON, 'w', encoding='utf-8') as f:
        f.write(text)
    print(f'wrote docs/openapi.json ({len(text)} bytes)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
