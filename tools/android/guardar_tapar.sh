#!/bin/bash
# tools/android/guardar_tapar.sh <nombre>: carga la partida de prueba (PARTIDA=<carpeta con los archivos de
# save/save_0>), camina a la derecha 12 s (cruza a la zona
# siguiente: el autoguardado del juego escribe la partida), y a lo bruto: force-stop JUSTO después; luego
# arranca otra vez y comprueba que la partida carga (la captura del final) y que el archivo de guardado cambió.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
export MSYS_NO_PATHCONV=1
N=${1:?nombre}
PKG=org.smrpgrecomp.android
D=/sdcard/Android/data/$PKG/files
OUT=$HERE/../../build/android-pruebas/guardar-$N
mkdir -p "$OUT"
G0=$(adb shell md5sum $D/save/save_0/SMR_Save_Ver0011000 2>/dev/null | awk '{print $1}' | tr -d '\r')
{
    echo "LARECOMP_NO_AUDIO=1"
    echo "LARECOMP_NO_MOVIE_AUDIO=1"
    echo "LARECOMP_INPUT=1300:A:8,1600:A:8,1900:A:8,2200:A:8,2500:STICK_RIGHT:3600,6400:STICK_LEFT:60"
} > "$OUT/entorno.txt"
adb push "$(cygpath -w "$OUT/entorno.txt")" $D/entorno.txt > /dev/null
for f in "${PARTIDA:?PARTIDA=<carpeta con la partida de prueba: los archivos de save/save_0>}"/*; do adb push "$(cygpath -w "$f")" "$D/save/save_0/$(basename "$f")" > /dev/null; done
adb shell md5sum $D/save/save_0/SMR_Save_Ver0011000 | tr -d '\r' > "$OUT/md5.txt"
adb shell rm -f "$D/lasrecomp.log"
adb shell am force-stop $PKG
adb shell am start -n $PKG/.MainActivity > /dev/null
echo "== $N: arrancada $(date +%H:%M:%S); guardado antes: $G0"
sleep 95   # carga (37 s) + caminata (12 s) + margen para el autoguardado
adb shell am force-stop $PKG   # a lo bruto, justo después de caminar/guardar
echo "force-stop a los 95 s"
G1=$(adb shell md5sum $D/save/save_0/SMR_Save_Ver0011000 2>/dev/null | awk '{print $1}' | tr -d '\r')
echo "guardado después: $G1"
[ -n "$G0" ] && [ "$G0" != "$G1" ] && echo "EL GUARDADO CAMBIÓ (autoguardado escrito)" || echo "el guardado NO cambió"
# segunda arrancada: cargar y capturar
adb shell rm -f "$D/lasrecomp.log"
adb shell am start -n $PKG/.MainActivity > /dev/null
sleep 55
adb exec-out screencap -p > "$OUT/tras-recargar.png"
adb shell pidof $PKG | tr -d '\r' | grep -q . && echo "segunda arrancada VIVA" || echo "segunda arrancada MUERTA"
adb shell cat "$D/lasrecomp.log" > "$OUT/lasrecomp2.log" 2> /dev/null
adb shell am force-stop $PKG
adb shell rm -f "$D/entorno.txt"
grep -acE "VK_ERROR|device lost|\[crash|FATAL" "$OUT/lasrecomp2.log" | xargs echo "errores tras recargar:"
grep -a "^\[vk\] t=" "$OUT/lasrecomp2.log" | tail -2 | cut -c1-95
