#!/usr/bin/env python3
"""Read-only collection of this phone's original Qualcomm Bluetooth firmware.

The phone must be in recovery (TWRP/OrangeFox) with ADB. The Android
"bluetooth" partition (FAT, mounted by Android as /vendor/bt_firmware) is read
with `cat` from /dev/block/by-name; nothing on the phone is written or mounted.
The image and its files go to private/firmware/<stamp> on this
computer only (vendor firmware: never commit or redistribute).
"""
import argparse, hashlib, json, os, subprocess, sys
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENV = dict(os.environ, MTOOLS_SKIP_CHECK='1')


def adb(args, serial, **kw):
    return subprocess.run(['adb'] + (['-s', serial] if serial else []) + args,
                          capture_output=True, timeout=kw.pop('timeout', 30), **kw)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--serial')
    a = ap.parse_args()
    state = adb(['get-state'], a.serial).stdout.decode().strip()
    if state != 'recovery':
        sys.exit(f'手机需处于recovery（当前：{state or "未连接"}）')
    names = adb(['shell', 'ls', '/dev/block/by-name'], a.serial).stdout.decode().split()
    parts = [n for n in names if n in ('bluetooth', 'bluetooth_a', 'bluetooth_b')]
    if not parts:
        sys.exit('没有bluetooth分区：' + ' '.join(sorted(names)))
    slot = adb(['shell', 'getprop', 'ro.boot.slot_suffix'], a.serial).stdout.decode().strip()
    want = 'bluetooth' + slot if 'bluetooth' + slot in parts else parts[0]
    out = ROOT / 'private/firmware' / datetime.now().strftime('%Y%m%d-%H%M%S')
    out.mkdir(parents=True)
    image = adb(['exec-out', 'cat', '/dev/block/by-name/' + want], a.serial, timeout=120).stdout
    size = adb(['shell', 'blockdev', '--getsize64', '/dev/block/by-name/' + want], a.serial).stdout.decode().strip()
    if not size.isdigit() or len(image) != int(size):
        sys.exit(f'{want}: 读取不完整（{len(image)} / {size}）')
    (out / f'{want}.img').write_bytes(image)
    listing = subprocess.run(['mdir', '-/', '-a', '-i', str(out / f'{want}.img'), '::'],
                             capture_output=True, text=True, env=ENV)
    if listing.returncode:
        sys.exit(f'{want}不是可读FAT：{listing.stderr.strip()}')
    files = out / 'files'
    files.mkdir()
    copy = subprocess.run(['mcopy', '-s', '-n', '-i', str(out / f'{want}.img'), '::*', str(files) + '/'],
                          capture_output=True, text=True, env=ENV)
    if copy.returncode:
        sys.exit(f'{want}: 文件提取失败：{copy.stderr.strip()}')
    manifest = {'partition': want, 'slot': slot, 'image_size': len(image),
                'image_sha256': hashlib.sha256(image).hexdigest(), 'files': []}
    for p in sorted(files.rglob('*')):
        if p.is_file():
            data = p.read_bytes()
            manifest['files'].append({'path': str(p.relative_to(files)), 'size': len(data),
                                      'sha256': hashlib.sha256(data).hexdigest()})
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'已只读收集 {want}（{len(image)} 字节，{len(manifest["files"])} 个文件）-> {out}')
    for f in manifest['files']:
        print(f'  {f["path"]}  {f["size"]}')


if __name__ == '__main__':
    main()
