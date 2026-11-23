# tools/local_env.sh: las rutas de las herramientas de fuera del repositorio (SDK y NDK de Android, JDK...) de
# local.env, en la raíz del repositorio (la carpeta de encima de esta). Lo cargan con «.» tools/android/apk.sh,
# tools/android/deps.sh y tools/lsfg/build.sh. local.env: NOMBRE=valor por línea, sin comillas (los espacios valen);
# git no lo sube (ver local.env.ejemplo). Lo que ya venga del entorno manda: el archivo solo rellena lo que falta.
# (Se busca por la ruta de este archivo, no por $1: un «.» sin argumentos hereda los del guion que lo carga.)
_le_f=$(dirname "${BASH_SOURCE[0]}")/../local.env
if [ -f "$_le_f" ]; then
    while IFS='=' read -r _le_k _le_v || [ -n "$_le_k" ]; do
        _le_v=${_le_v%$'\r'}
        case $_le_k in '' | \#* | *[!A-Za-z0-9_]*) continue ;; esac
        [ -n "${!_le_k:-}" ] || export "$_le_k=$_le_v"
    done < "$_le_f"
fi
unset _le_f _le_k _le_v
