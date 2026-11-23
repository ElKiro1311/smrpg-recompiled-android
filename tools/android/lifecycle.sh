#!/bin/bash
# tools/android/lifecycle.sh <name>: a test save (SAVE=<folder with the files of save/save_0>) on the device connected
# through adb; 10 times to the background (Home) and back, then 3 times screen off and on, checking after each cycle that
# the process is alive and the log moves on (it is rendering). A screenshot at the end. (The controller cannot be
# simulated through adb: noted and not tested.)
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
export MSYS_NO_PATHCONV=1
N=${1:?name}
PKG=org.smrpgrecomp.android
D=/sdcard/Android/data/$PKG/files
OUT=$HERE/../../build/android-tests/lifecycle-$N
mkdir -p "$OUT"
{
    echo "LARECOMP_NO_AUDIO=1"
    echo "LARECOMP_NO_MOVIE_AUDIO=1"
    echo "LARECOMP_INPUT=1300:A:8,1600:A:8,1900:A:8,2200:A:8"
} > "$OUT/env.txt"
adb push "$(cygpath -w "$OUT/env.txt")" $D/env.txt > /dev/null
for f in "${SAVE:?SAVE=<folder with the test save: the files of save/save_0>}"/*; do adb push "$(cygpath -w "$f")" "$D/save/save_0/$(basename "$f")" > /dev/null; done
adb shell rm -f "$D/lasrecomp.log"
adb shell am force-stop $PKG
adb shell am start -n $PKG/.MainActivity > /dev/null
echo "== $N: started $(date +%H:%M:%S)"
sleep 55   # in the game by now
P=$(adb shell pidof $PKG | tr -d '\r')
echo "pid $P"
function log_lines() {  # how many lines the log has
    adb shell wc -l < "$D/lasrecomp.log" 2>/dev/null | tr -d '\r'
}
L0=$(log_lines)
: > "$OUT/cycles.txt"
for i in $(seq 1 10); do
    adb shell input keyevent KEYCODE_HOME
    sleep 5
    adb shell am start -n $PKG/.MainActivity > /dev/null
    sleep 8
    P=$(adb shell pidof $PKG | tr -d '\r')
    L1=$(log_lines)
    echo "$(date +%H:%M:%S) cycle $i: pid=${P:-DEAD} log $L0->$L1" >> "$OUT/cycles.txt"
    L0=$L1
    [ -n "$P" ] || break
done
for i in 1 2 3; do
    adb shell input keyevent KEYCODE_POWER
    sleep 4
    adb shell input keyevent KEYCODE_POWER
    sleep 6
    P=$(adb shell pidof $PKG | tr -d '\r')
    L1=$(log_lines)
    echo "$(date +%H:%M:%S) screen $i: pid=${P:-DEAD} log $L0->$L1" >> "$OUT/cycles.txt"
    L0=$L1
    [ -n "$P" ] || break
done
adb exec-out screencap -p > "$OUT/final.png"
adb shell am force-stop $PKG
adb shell cat "$D/lasrecomp.log" > "$OUT/lasrecomp.log" 2> /dev/null
adb shell rm -f "$D/env.txt"
echo "== $N: end $(date +%H:%M:%S)"
cat "$OUT/cycles.txt"
grep -acE "VK_ERROR|device lost|\[crash|FATAL" "$OUT/lasrecomp.log" | xargs echo "Vulkan errors / crashes:"
