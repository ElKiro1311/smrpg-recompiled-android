#!/bin/bash
# tools/android/sesion_larga.sh <nombre> <segundos> <archivo_de_entrada>: una partida de prueba (PARTIDA=<carpeta con
# los archivos de save/save_0>) en el aparato conectado por adb, con
# una entrada programada larguísima (generada aparte), y cada 60 s el RSS del proceso (+ si vive). Al final cierra
# y trae el registro. La temperatura del aparato se vigila aparte.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
export MSYS_NO_PATHCONV=1
N=${1:?nombre}; SECS=${2:?segundos}; INPUT_FILE=${3:?archivo de entrada}
PKG=org.smrpgrecomp.android
D=/sdcard/Android/data/$PKG/files
OUT=$HERE/../../build/android-pruebas/sesion-$N
mkdir -p "$OUT"
INPUT=$(cat "$INPUT_FILE")
{
    echo "LARECOMP_NO_AUDIO=1"
    echo "LARECOMP_NO_MOVIE_AUDIO=1"
    echo "LARECOMP_INPUT=$INPUT"
} > "$OUT/entorno.txt"
adb push "$(cygpath -w "$OUT/entorno.txt")" $D/entorno.txt > /dev/null
for f in "${PARTIDA:?PARTIDA=<carpeta con la partida de prueba: los archivos de save/save_0>}"/*; do adb push "$(cygpath -w "$f")" "$D/save/save_0/$(basename "$f")" > /dev/null; done
adb shell rm -f "$D/lasrecomp.log"
adb shell am force-stop $PKG
T0=$(date +%s)
adb shell am start -n $PKG/.MainActivity > /dev/null
echo "== $N: arrancada $(date +%H:%M:%S), ${SECS}s, entrada de $(wc -c < "$INPUT_FILE") bytes"
: > "$OUT/rss.txt"
while [ $(( $(date +%s) - T0 )) -lt $SECS ]; do
    sleep 30
    P=$(adb shell pidof $PKG | tr -d '\r')
    if [ -z "$P" ]; then
        echo "$(date +%H:%M:%S) MUERTA" >> "$OUT/rss.txt"
        break
    fi
    RSS=$(adb shell cat /proc/$P/status 2>/dev/null | tr -d '\r' | grep VmRSS | awk '{print $2}')
    GPU=$(adb shell cat /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage 2>/dev/null | tr -d '\r')
    echo "$(date +%H:%M:%S) $(( $(date +%s) - T0 ))s RSS=${RSS}kB GPU=${GPU}%" >> "$OUT/rss.txt"
done
adb shell am force-stop $PKG
adb shell cat "$D/lasrecomp.log" > "$OUT/lasrecomp.log" 2> /dev/null
adb shell rm -f "$D/entorno.txt"
echo "== $N: cerrada $(date +%H:%M:%S); $(wc -l < "$OUT/rss.txt") muestras de RSS"
tail -5 "$OUT/rss.txt"
grep -acE "VK_ERROR|device lost|\[crash|FATAL" "$OUT/lasrecomp.log" | xargs echo "errores Vulkan/crash:"
grep -a "^\\[vk\\] t=" "$OUT/lasrecomp.log" | tail -3 | cut -c1-100
