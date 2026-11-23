#!/bin/bash
# tools/android/pc_shader_transfer.sh <the PC's cache folder>: takes to an Android device what the PC has already seen (its
# own pipeline list: the " z=" lines the device does not have, and the codes that are missing). The PC must have run the
# scene with LARECOMP_VK_DYN_DEPTH=0 (the depth state fixed in the pipeline, as on the Adreno; without it its lines come out
# without " z=" and are no use). Measured in the sewers (d04b1), never seen on the device: from 45 pipelines compiled when
# used and a 1025 ms frame to 11 pipelines, 18 skipped draws and 373 ms (CHANGELOG-ANDROID 0.8.2). Limitation: the PC
# records the dynamic vertex layout, so the pipelines of a few families do not match and are compiled when used (2 of 47 in
# the test, [prepcheck] in the log); LARECOMP_VK_DYN_VERTEX=0 does not fix it: the PC's game crawls with the fixed layout
# (~200 ms per frame).
# Usage: with the app CLOSED and adb seeing the device (the APK must be debuggable, APK_DEBUG=1: run-as):
#   tools/android/pc_shader_transfer.sh <the PC's cache folder> [package]
# (adb cannot write into cache/, which belongs to the app: it says "pushed" and writes nothing; that is why the transfer
# goes through run-as, with chmod 660 after copying, like the files the game creates. Creating some code may fail for lack
# of permission: that shader is compiled when used and the log says "not found"; harmless.)
set -uo pipefail
PC=${1:?the PC's cache folder (with shader_seen.txt and shader_code/)}
PKG=${2:-org.smrpgrecomp.android}
export MSYS_NO_PATHCONV=1
D=/sdcard/Android/data/$PKG/files
[ -f "$PC/shader_seen.txt" ] || { echo "no $PC/shader_seen.txt"; exit 1; }
[ -d "$PC/shader_code" ] || { echo "no $PC/shader_code"; exit 1; }
[ -z "$(adb shell pidof $PKG | tr -d '\r')" ] || { echo "the game is open: close it first"; exit 1; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

adb shell cat "$D/cache/shader_seen.txt" | tr -d '\r' > "$T/device.txt"
adb shell ls "$D/cache/shader_code/" | tr -d '\r' | sed 's/\.bin$//' | sort > "$T/device-codes.txt"

# the PC's lines the device does not have, of those that are good for the Adreno (with " z="; the compute ones, without depth)
grep -E '^C | z=' "$PC/shader_seen.txt" | grep -vxF -f "$T/device.txt" > "$T/new.txt" || true
N=$(wc -l < "$T/new.txt")
[ "$N" = 0 ] && { echo "nothing new to take (it already has every valid line)"; exit 0; }
grep -oE '(vs|fs)=[0-9a-f]{16}' "$T/new.txt" | cut -d= -f2 | sort -u > "$T/hashes.txt"
comm -23 "$T/hashes.txt" "$T/device-codes.txt" > "$T/missing.txt"
echo "transferring $N lines of the list and $(wc -l < "$T/missing.txt") missing codes"

cat "$T/device.txt" "$T/new.txt" > "$T/merge.txt"
adb push "$T/merge.txt" /sdcard/Download/seen-merge.txt > /dev/null
adb shell run-as $PKG cp /sdcard/Download/seen-merge.txt "$D/cache/shader_seen.txt"
adb shell run-as $PKG chmod 660 "$D/cache/shader_seen.txt"
adb shell rm /sdcard/Download/seen-merge.txt

# codes: by their exact name (the app cannot list /sdcard/Download, and the wildcard is not expanded)
failed=0
while read -r h; do
    [ -f "$PC/shader_code/$h.bin" ] || { echo "  warning: the PC does not have the code of $h (another scene?)"; failed=$((failed+1)); continue; }
    adb push "$PC/shader_code/$h.bin" "/sdcard/Download/code-$h.bin" > /dev/null
    adb shell run-as $PKG sh -c "cat /sdcard/Download/code-$h.bin > $D/cache/shader_code/$h.bin" \
        || { echo "  warning: $h.bin could not be created; it will be compiled when used"; failed=$((failed+1)); }
    adb shell rm "/sdcard/Download/code-$h.bin"
done < "$T/missing.txt"

echo "== the device now has:"
adb shell "wc -l < $D/cache/shader_seen.txt; ls $D/cache/shader_code/ | wc -l" | tr -d '\r'
[ $failed = 0 ] || echo "== $failed codes not created: the log will say \"not found\" when preparing those lines; harmless"
