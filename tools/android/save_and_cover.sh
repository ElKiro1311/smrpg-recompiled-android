#!/bin/bash
# tools/android/save_and_cover.sh <name>: loads the test save (SAVE=<folder with the files of save/save_0>), walks to the
# right for 12 s (it crosses into the next area: the game's autosave writes the save) and, the hard way: force-stop RIGHT
# after; then it starts again and checks that the save loads (the screenshot at the end) and that the save file changed.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
export MSYS_NO_PATHCONV=1
N=${1:?name}
PKG=org.smrpgrecomp.android
D=/sdcard/Android/data/$PKG/files
OUT=$HERE/../../build/android-tests/save-$N
mkdir -p "$OUT"
G0=$(adb shell md5sum $D/save/save_0/SMR_Save_Ver0011000 2>/dev/null | awk '{print $1}' | tr -d '\r')
{
    echo "LARECOMP_NO_AUDIO=1"
    echo "LARECOMP_NO_MOVIE_AUDIO=1"
    echo "LARECOMP_INPUT=1300:A:8,1600:A:8,1900:A:8,2200:A:8,2500:STICK_RIGHT:3600,6400:STICK_LEFT:60"
} > "$OUT/env.txt"
adb push "$(cygpath -w "$OUT/env.txt")" $D/env.txt > /dev/null
for f in "${SAVE:?SAVE=<folder with the test save: the files of save/save_0>}"/*; do adb push "$(cygpath -w "$f")" "$D/save/save_0/$(basename "$f")" > /dev/null; done
adb shell md5sum $D/save/save_0/SMR_Save_Ver0011000 | tr -d '\r' > "$OUT/md5.txt"
adb shell rm -f "$D/lasrecomp.log"
adb shell am force-stop $PKG
adb shell am start -n $PKG/.MainActivity > /dev/null
echo "== $N: started $(date +%H:%M:%S); save before: $G0"
sleep 95   # loading (37 s) + walk (12 s) + margin for the autosave
adb shell am force-stop $PKG   # the hard way, right after walking / saving
echo "force-stop at 95 s"
G1=$(adb shell md5sum $D/save/save_0/SMR_Save_Ver0011000 2>/dev/null | awk '{print $1}' | tr -d '\r')
echo "save after: $G1"
[ -n "$G0" ] && [ "$G0" != "$G1" ] && echo "THE SAVE CHANGED (autosave written)" || echo "the save did NOT change"
# second start: load and take a screenshot
adb shell rm -f "$D/lasrecomp.log"
adb shell am start -n $PKG/.MainActivity > /dev/null
sleep 55
adb exec-out screencap -p > "$OUT/after-reload.png"
adb shell pidof $PKG | tr -d '\r' | grep -q . && echo "second start ALIVE" || echo "second start DEAD"
adb shell cat "$D/lasrecomp.log" > "$OUT/lasrecomp2.log" 2> /dev/null
adb shell am force-stop $PKG
adb shell rm -f "$D/env.txt"
grep -acE "VK_ERROR|device lost|\[crash|FATAL" "$OUT/lasrecomp2.log" | xargs echo "errors after reloading:"
grep -a "^\[vk\] t=" "$OUT/lasrecomp2.log" | tail -2 | cut -c1-95
