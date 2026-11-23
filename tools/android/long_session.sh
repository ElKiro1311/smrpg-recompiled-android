#!/bin/bash
# tools/android/long_session.sh <name> <seconds> <input file>: a test save (SAVE=<folder with the files of save/save_0>) on
# the device connected through adb, with a very long scripted input (generated apart), and every 60 s the process's RSS
# (and whether it is alive). At the end it closes it and brings the log. The device's temperature is watched apart.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
export MSYS_NO_PATHCONV=1
N=${1:?name}; SECS=${2:?seconds}; INPUT_FILE=${3:?input file}
PKG=org.smrpgrecomp.android
D=/sdcard/Android/data/$PKG/files
OUT=$HERE/../../build/android-tests/session-$N
mkdir -p "$OUT"
INPUT=$(cat "$INPUT_FILE")
{
    echo "LARECOMP_NO_AUDIO=1"
    echo "LARECOMP_NO_MOVIE_AUDIO=1"
    echo "LARECOMP_INPUT=$INPUT"
} > "$OUT/env.txt"
adb push "$(cygpath -w "$OUT/env.txt")" $D/env.txt > /dev/null
for f in "${SAVE:?SAVE=<folder with the test save: the files of save/save_0>}"/*; do adb push "$(cygpath -w "$f")" "$D/save/save_0/$(basename "$f")" > /dev/null; done
adb shell rm -f "$D/lasrecomp.log"
adb shell am force-stop $PKG
T0=$(date +%s)
adb shell am start -n $PKG/.MainActivity > /dev/null
echo "== $N: started $(date +%H:%M:%S), ${SECS}s, input of $(wc -c < "$INPUT_FILE") bytes"
: > "$OUT/rss.txt"
while [ $(( $(date +%s) - T0 )) -lt $SECS ]; do
    sleep 30
    P=$(adb shell pidof $PKG | tr -d '\r')
    if [ -z "$P" ]; then
        echo "$(date +%H:%M:%S) DEAD" >> "$OUT/rss.txt"
        break
    fi
    RSS=$(adb shell cat /proc/$P/status 2>/dev/null | tr -d '\r' | grep VmRSS | awk '{print $2}')
    GPU=$(adb shell cat /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage 2>/dev/null | tr -d '\r')
    echo "$(date +%H:%M:%S) $(( $(date +%s) - T0 ))s RSS=${RSS}kB GPU=${GPU}%" >> "$OUT/rss.txt"
done
adb shell am force-stop $PKG
adb shell cat "$D/lasrecomp.log" > "$OUT/lasrecomp.log" 2> /dev/null
adb shell rm -f "$D/env.txt"
echo "== $N: closed $(date +%H:%M:%S); $(wc -l < "$OUT/rss.txt") RSS samples"
tail -5 "$OUT/rss.txt"
grep -acE "VK_ERROR|device lost|\[crash|FATAL" "$OUT/lasrecomp.log" | xargs echo "Vulkan errors / crashes:"
grep -a "^\[vk\] t=" "$OUT/lasrecomp.log" | tail -3 | cut -c1-100
