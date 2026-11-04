#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Local compilation only. No device connection, installation, reboot or flashing.
set -euo pipefail
project_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
if (( $# < 1 || $# > 2 )); then
    echo "Usage: bash tools/build-userland.sh /path/to/original-20H392-bluetoothd [output-directory]" >&2
    exit 2
fi
original=$(realpath -- "$1")
out=${2:-$project_root/build/userland}
theos_root=${THEOS:-$HOME/theos}
sdk=${RENOIR_IOS_SDK:-$theos_root/sdks/iPhoneOS16.5.sdk}
clang="$theos_root/toolchain/linux/iphone/bin/clang"
ldid="$theos_root/toolchain/linux/iphone/bin/ldid"
mkdir -p "$out"
# The patcher verifies the exact original before producing anything for installation.
python3 "$project_root/tools/patch-renoir-bluetoothd.py" "$original" "$out/bluetoothd"
"$clang" -target arm64-apple-ios16.5 -isysroot "$sdk" -O2 -Wall -Wextra -Werror -fblocks \
    "$project_root/userland/renoir_bluetool.c" -o "$out/BlueTool"
"$clang" -target arm64-apple-ios16.5 -isysroot "$sdk" -O2 -Wall -Wextra -Werror -dynamiclib \
    -install_name /usr/lib/RenoirBTType0.dylib \
    "$project_root/userland/renoir_bt_type0.c" -o "$out/RenoirBTType0.dylib"
"$ldid" -S "$out/BlueTool"
"$ldid" -S "$out/RenoirBTType0.dylib"
"$ldid" -e "$original" > "$out/bluetoothd.entitlements.xml"
"$ldid" -S"$out/bluetoothd.entitlements.xml" -Icom.apple.bluetoothd "$out/bluetoothd"
for name in BlueTool RenoirBTType0.dylib bluetoothd; do
    sha256sum "$out/$name" | cut -d' ' -f1 > "$out/$name.sha256"
done
echo "Built and signed locally: $out (not installed)"
