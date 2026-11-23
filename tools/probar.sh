#!/bin/sh
# tools/probar.sh <segundos> <registro> [VARIABLE=valor ...]
# Un arranque de prueba que no toca otra copia del juego abierta (la de quien está jugando): al final cierra solo la
# suya, por su PID de Windows (antes: taskkill por nombre, que cerraba todos). Su PID es el smrpg-recomp.exe que
# aparece al lanzarla y no estaba antes ($! es el de env, y /proc/$!/winpid el suyo, no el del juego).
# Ejemplo: tools/probar.sh 60 build/boot_X.log LARECOMP_NO_AUDIO=1 LARECOMP_INPUT="1300:A:8"
cd "$(dirname "$0")/.." || exit 1
secs=$1
log=$2
shift 2
running() { tasklist //FI "IMAGENAME eq smrpg-recomp.exe" //FO CSV //NH 2>/dev/null | grep -i smrpg-recomp; }
pids() { running | cut -d, -f2 | tr -d '"'; }
# Con otra copia abierta (también con el nombre antiguo, lasrecomp.exe) solo se lanza con una carpeta de datos aislada
# (LARECOMP_DATA_ROOT=... entre los argumentos: sus propios save/ y larecomp.ini): no comparten guardados ni ajustes.
aislada=0
for a in "$@"; do case "$a" in LARECOMP_DATA_ROOT=?*) aislada=1 ;; esac; done
antes=" $(pids | tr '\n' ' ') "
if [ $aislada = 0 ] && { [ -n "$(echo $antes)" ] || tasklist //FI "IMAGENAME eq lasrecomp.exe" //NH 2>/dev/null | grep -qi lasrecomp; }; then
    echo "[probar] el juego ya está abierto: no se lanza la prueba (con LARECOMP_DATA_ROOT=<carpeta aislada>, sí)"
    exit 2
fi
if [ -d /ucrt64/bin ]; then export PATH="/ucrt64/bin:$PATH"; else export PATH="/c/msys64/ucrt64/bin:$PATH"; fi
env LARECOMP_FORCE_LAUNCHER=0 "$@" ./build/smrpg-recomp.exe --play extracted > "$log" 2>&1 &
pid=$!
winpid=
for i in $(seq 1 40); do
    for p in $(pids); do
        case "$antes" in *" $p "*) ;; *) winpid=$p; break ;; esac
    done
    [ -n "$winpid" ] && break
    timeout 0.25 tail -f /dev/null
done
timeout "$secs" tail --pid="$pid" -f /dev/null
if [ -n "$winpid" ]; then taskkill //PID "$winpid" //T //F > /dev/null 2>&1; fi  # (/T: también su hijo - plat_exec relanza el programa y espera)
wait "$pid" 2>/dev/null
echo "[probar] terminado (PID de Windows ${winpid:-?})"
