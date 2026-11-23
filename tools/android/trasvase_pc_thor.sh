#!/bin/bash
# tools/android/trasvase_pc_thor.sh <carpeta-caché-del-PC>: lleva a un aparato Android lo ya visto en el PC (la lista
# propia de tuberías: las líneas con « z=» que la Thor no tenga, y los códigos que falten). El PC tiene que haber
# corrido la escena con LARECOMP_VK_DYN_DEPTH=0 (la profundidad fija en la tubería, como en la Adreno; sin eso sus
# líneas salen sin « z=» y no valen). Medido en las alcantarillas (d04b1), nunca vistas en el aparato: de 45 tuberías
# compiladas al usarse y un fotograma de 1025 ms a 11 tuberías, 18 dibujos saltados y 373 ms (VERSIONES-ANDROID
# 0.8.2). Limitación: el PC graba la disposición de vértices dinámica y las tuberías de unas pocas familias no
# coinciden y se compilan al usarse (2 de 47 en la prueba, [prepcheck] en el registro); LARECOMP_VK_DYN_VERTEX=0 no
# lo arregla: el juego del PC se arrastra con la disposición fija (~200 ms por fotograma).
# Uso: con la app CERRADA y adb viendo el aparato (la APK tiene que ser depurable: run-as):
#   tools/android/trasvase_pc_thor.sh <carpeta de la caché del PC> [paquete]
# (adb no puede escribir en cache/, que es de la app: dice «pushed» y no escribe; por eso el trasvase va por
# «run-as», con chmod 660 tras copiar, como los archivos que crea el juego. Alguna creación de código puede fallar
# sin permiso: ese sombreado se compilará al usarse y el registro dirá «not found»; inofensivo.)
set -uo pipefail
PC=${1:?carpeta de la caché del PC (con shader_seen.txt y shader_code/)}
PKG=${2:-org.smrpgrecomp.android}
export MSYS_NO_PATHCONV=1
D=/sdcard/Android/data/$PKG/files
[ -f "$PC/shader_seen.txt" ] || { echo "no hay $PC/shader_seen.txt"; exit 1; }
[ -d "$PC/shader_code" ] || { echo "no hay $PC/shader_code"; exit 1; }
[ -z "$(adb shell pidof $PKG | tr -d '\r')" ] || { echo "el juego está abierto: ciérralo primero"; exit 1; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

adb shell cat "$D/cache/shader_seen.txt" | tr -d '\r' > "$T/thor.txt"
adb shell ls "$D/cache/shader_code/" | tr -d '\r' | sed 's/\.bin$//' | sort > "$T/thor-codes.txt"

# las líneas del PC que el aparato no tiene, de las que valen para la Adreno (con « z=»; las de cómputo, sin profundidad)
grep -E '^C | z=' "$PC/shader_seen.txt" | grep -vxF -f "$T/thor.txt" > "$T/nuevas.txt" || true
N=$(wc -l < "$T/nuevas.txt")
[ "$N" = 0 ] && { echo "nada nuevo que llevar (ya tiene todas las líneas válidas)"; exit 0; }
grep -oE '(vs|fs)=[0-9a-f]{16}' "$T/nuevas.txt" | cut -d= -f2 | sort -u > "$T/hashes.txt"
comm -23 "$T/hashes.txt" "$T/thor-codes.txt" > "$T/faltan.txt"
echo "trasvasando $N líneas de la lista y $(wc -l < "$T/faltan.txt") códigos que faltan"

cat "$T/thor.txt" "$T/nuevas.txt" > "$T/merge.txt"
adb push "$T/merge.txt" /sdcard/Download/seen-merge.txt > /dev/null
adb shell run-as $PKG cp /sdcard/Download/seen-merge.txt "$D/cache/shader_seen.txt"
adb shell run-as $PKG chmod 660 "$D/cache/shader_seen.txt"
adb shell rm /sdcard/Download/seen-merge.txt

# códigos: por nombre exacto (la app no puede listar /sdcard/Download, y el comodín no se expande)
fallidos=0
while read -r h; do
    [ -f "$PC/shader_code/$h.bin" ] || { echo "  aviso: el PC no tiene el código de $h (¿otra escena?)"; fallidos=$((fallidos+1)); continue; }
    adb push "$PC/shader_code/$h.bin" "/sdcard/Download/code-$h.bin" > /dev/null
    adb shell run-as $PKG sh -c "cat /sdcard/Download/code-$h.bin > $D/cache/shader_code/$h.bin" \
        || { echo "  aviso: no se pudo crear $h.bin; se compilará al usarse"; fallidos=$((fallidos+1)); }
    adb shell rm "/sdcard/Download/code-$h.bin"
done < "$T/faltan.txt"

echo "== el aparato queda con:"
adb shell "wc -l < $D/cache/shader_seen.txt; ls $D/cache/shader_code/ | wc -l" | tr -d '\r'
[ $fallidos = 0 ] || echo "== $fallidos códigos sin crear: el registro dirá «not found» al preparar esas líneas; inofensivo"
