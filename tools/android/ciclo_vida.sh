#!/bin/bash
# tools/android/ciclo_vida.sh <nombre>: una partida de prueba (PARTIDA=<carpeta con los archivos de save/save_0>) en
# el aparato conectado por adb; 10 veces a segundo plano (Inicio) y vuelta,
# luego 3 apagados/encendidos de pantalla, comprobando tras cada ciclo que el proceso vive y el registro avanza
# (está renderizando). Captura al final. (El mando no se puede simular por adb: se apunta y no se prueba.)
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
export MSYS_NO_PATHCONV=1
N=${1:?nombre}
PKG=org.smrpgrecomp.android
D=/sdcard/Android/data/$PKG/files
OUT=$HERE/../../build/android-pruebas/ciclo-$N
mkdir -p "$OUT"
{
    echo "LARECOMP_NO_AUDIO=1"
    echo "LARECOMP_NO_MOVIE_AUDIO=1"
    echo "LARECOMP_INPUT=1300:A:8,1600:A:8,1900:A:8,2200:A:8"
} > "$OUT/entorno.txt"
adb push "$(cygpath -w "$OUT/entorno.txt")" $D/entorno.txt > /dev/null
for f in "${PARTIDA:?PARTIDA=<carpeta con la partida de prueba: los archivos de save/save_0>}"/*; do adb push "$(cygpath -w "$f")" "$D/save/save_0/$(basename "$f")" > /dev/null; done
adb shell rm -f "$D/lasrecomp.log"
adb shell am force-stop $PKG
adb shell am start -n $PKG/.MainActivity > /dev/null
echo "== $N: arrancada $(date +%H:%M:%S)"
sleep 55   # en la partida ya
P=$(adb shell pidof $PKG | tr -d '\r')
echo "pid $P"
function avanza() {  # ¿cuántas líneas de registro hay?
    adb shell wc -l < "$D/lasrecomp.log" 2>/dev/null | tr -d '\r'
}
L0=$(avanza)
: > "$OUT/ciclos.txt"
for i in $(seq 1 10); do
    adb shell input keyevent KEYCODE_HOME
    sleep 5
    adb shell am start -n $PKG/.MainActivity > /dev/null
    sleep 8
    P=$(adb shell pidof $PKG | tr -d '\r')
    L1=$(avanza)
    echo "$(date +%H:%M:%S) ciclo $i: pid=${P:-MUERTO} registro $L0->$L1" >> "$OUT/ciclos.txt"
    L0=$L1
    [ -n "$P" ] || break
done
for i in 1 2 3; do
    adb shell input keyevent KEYCODE_POWER
    sleep 4
    adb shell input keyevent KEYCODE_POWER
    sleep 6
    P=$(adb shell pidof $PKG | tr -d '\r')
    L1=$(avanza)
    echo "$(date +%H:%M:%S) pantalla $i: pid=${P:-MUERTO} registro $L0->$L1" >> "$OUT/ciclos.txt"
    L0=$L1
    [ -n "$P" ] || break
done
adb exec-out screencap -p > "$OUT/final.png"
adb shell am force-stop $PKG
adb shell cat "$D/lasrecomp.log" > "$OUT/lasrecomp.log" 2> /dev/null
adb shell rm -f "$D/entorno.txt"
echo "== $N: fin $(date +%H:%M:%S)"
cat "$OUT/ciclos.txt"
grep -acE "VK_ERROR|device lost|\[crash|FATAL" "$OUT/lasrecomp.log" | xargs echo "errores Vulkan/crash:"
