#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Verify the pinned Bluetooth patch series; optionally apply it to a kernel128 tree."""
import argparse
import hashlib
import json
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None


def verify(tree, expected):
    for name, value in expected.items():
        if digest(tree / name) != value:
            raise ValueError(f'Unexpected version: {tree / name}')


def apply(tree, patches):
    for item in patches:
        subprocess.run(['git', 'apply', '--whitespace=nowarn', str(ROOT / 'patches' / item['file'])],
                       cwd=tree, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, help='Full Renoir kernel128 baseline; default: bundled fixture')
    parser.add_argument('--apply', action='store_true', help='Apply to --tree after validation succeeds')
    args = parser.parse_args()
    if args.apply and not args.tree:
        parser.error('--apply requires an explicit --tree')
    series = json.loads((ROOT / 'patches/series.json').read_text())
    baseline = args.tree.resolve() if args.tree else ROOT / 'kernel/base'
    for item in series['patches']:
        if digest(ROOT / 'patches' / item['file']) != item['sha256']:
            raise ValueError(f'Patch hash mismatch: {item["file"]}')
    verify(baseline, series['base_sha256'])
    with tempfile.TemporaryDirectory(prefix='renoir-bt-series-') as temp:
        target = Path(temp)
        for name, value in series['base_sha256'].items():
            if value is not None:
                out = target / name
                out.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(baseline / name, out)
        apply(target, series['patches'])
        verify(target, series['result_sha256'])
        for snapshot in (ROOT / 'kernel/files').rglob('*.inc'):
            relative = snapshot.relative_to(ROOT / 'kernel/files')
            if snapshot.read_bytes() != (target / relative).read_bytes():
                raise ValueError(f'Source snapshot differs: {relative}')
    if args.apply:
        apply(baseline, series['patches'])
        verify(baseline, series['result_sha256'])
    print(f'{len(series["patches"])} patches: hashes, ordered application and final source match')
    print('Applied to kernel tree' if args.apply else 'Validation only; input tree unchanged')


if __name__ == '__main__':
    main()
