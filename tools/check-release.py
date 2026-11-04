#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Check public-source inventory, recorded provenance and packaging syntax."""
import ast
import hashlib
import json
import subprocess
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
manifest = json.loads((ROOT / 'provenance.json').read_text())
for entry in manifest['exported_files']:
    value = hashlib.sha256((ROOT / entry['path']).read_bytes()).hexdigest()
    if value != entry['export_sha256']:
        raise SystemExit(f'Export provenance mismatch: {entry["path"]}')
tomllib.loads((ROOT / 'REUSE.toml').read_text())
paths = subprocess.check_output(['git', 'ls-files', '-co', '--exclude-standard'], cwd=ROOT,
                                text=True).splitlines()
for name in paths:
    path = Path(name)
    if path.parts[0] in ('private', 'build', 'artifacts', 'firmware', 'logs'):
        raise SystemExit(f'Private/generated path included: {name}')
    if path.suffix.lower() in ('.img', '.bin', '.tlv', '.dylib', '.ips', '.tracev3'):
        raise SystemExit(f'Binary/device capture included: {name}')
    data = (ROOT / name).read_bytes()
    if b'\0' in data:
        raise SystemExit(f'Non-text file included: {name}')
    if path.suffix == '.py':
        ast.parse(data, filename=name)
    if path.suffix == '.sh':
        subprocess.run(['bash', '-n', str(ROOT / name)], check=True)
print(f'Public text inventory and provenance verified: {len(paths)} files')
