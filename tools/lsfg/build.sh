#!/bin/bash
# tools/lsfg/build.sh <windows|android-arm64-v8a|android-x86_64>: la biblioteca de Lossless Scaling Frame Generation
# del port (tools/lsfg/lsfg_smrpg.h): build/lsfg/windows/lsfg.dll o build/lsfg/android-<abi>/liblsfg.so.
# Desde el código de lsfg-vk-android (GameNative, rama release, commit 437208a, con sus submódulos: dxbc, pe-parse,
# volk) en ../deps/src/lsfg-vk-android (LSFG_SRC para otro sitio):
#   git clone --branch release https://github.com/GameNative/lsfg-vk-android ../deps/src/lsfg-vk-android
#   git -C ../deps/src/lsfg-vk-android submodule update --init --depth 1
# Windows: en MSYS2 UCRT64 (clang). Android: el NDK (NDK, o ANDROID_SDK/ndk/29.0.14206865: del entorno o de
# local.env, ver local.env.ejemplo). Sin CMake: las mismas listas de archivos que sus CMakeLists (framegen, dxbc,
# pe-parse, volk) y tools/lsfg/lsfg_smrpg.cpp.
set -euo pipefail
cd "$(dirname "$0")/../.."
. tools/local_env.sh
T=${1:?destino: windows, android-arm64-v8a o android-x86_64}
S=${LSFG_SRC:-../deps/src/lsfg-vk-android}
[ -f "$S/framegen/public/lsfg_3_1.hpp" ] && [ -f "$S/thirdparty/dxbc/CMakeLists.txt" ] ||
    { echo "lsfg: falta el código de lsfg-vk-android en $S (ver la cabecera de este guion)"; exit 1; }
O=build/lsfg/$T
mkdir -p "$O/obj"
case "$T" in
windows)
    CC=clang CXX=clang++
    OUT=$O/lsfg.dll
    TFLAGS="-DNOMINMAX -DWIN32_LEAN_AND_MEAN"
    LDFLAGS="-shared -fuse-ld=lld -static -static-libgcc -static-libstdc++ -Wl,--exclude-all-symbols"
    UNICODE=unicode_codecvt  # (la de winapi no compila en MinGW: char16_t frente a wchar_t)
    ;;
android-*)
    ABI=${T#android-}
    NDK=${NDK:-${ANDROID_SDK:+$ANDROID_SDK/ndk/29.0.14206865}}
    [ -n "$NDK" ] || { echo "lsfg: falta el NDK: pon ANDROID_SDK o NDK en local.env (ver local.env.ejemplo) o en el entorno"; exit 1; }
    TC=$NDK/toolchains/llvm/prebuilt/windows-x86_64
    case "$ABI" in arm64-v8a) TRIPLE=aarch64-linux-android33 ;; x86_64) TRIPLE=x86_64-linux-android33 ;;
        *) echo "lsfg: ABI desconocida $ABI"; exit 1 ;; esac
    # (la ruta del NDK lleva un espacio: sus compiladores van por el PATH, delante de los de MSYS2)
    export PATH="$TC/bin:$PATH"
    CC="clang.exe --target=$TRIPLE" CXX="clang++.exe --target=$TRIPLE"
    OUT=$O/liblsfg.so
    TFLAGS="-fPIC -DVK_USE_PLATFORM_ANDROID_KHR"  # (los tipos de AHardwareBuffer de su camino de Android)
    LDFLAGS="-shared -static-libstdc++ -Wl,-soname,liblsfg.so -Wl,--no-undefined -llog -landroid -Wl,-z,max-page-size=16384"
    UNICODE=unicode_codecvt
    ;;
*) echo "lsfg: destino desconocido $T"; exit 1 ;;
esac
INC="-I$S/framegen/include -I$S/framegen/public -I$S/framegen/v3.1_include -I$S/framegen/v3.1p_include \
     -I$S/thirdparty/dxbc/include/dxbc -I$S/thirdparty/dxbc/include/spirv -I$S/thirdparty/dxbc/include/util \
     -I$S/thirdparty/dxbc/include/dxvk -I$S/thirdparty/dxbc/include \
     -I$S/thirdparty/pe-parse/pe-parser-library/include -I$S/thirdparty/volk -I$S/src/extract -Itools/lsfg"
COMMON="-O2 -fvisibility=hidden -fvisibility-inlines-hidden -DVK_NO_PROTOTYPES -w $TFLAGS"
# (los objetos solo se comparan con su .cpp: si cambian las fuentes de lsfg-vk —otro commit o cambios sin él—, la
# cabecera de la API o el sombreador de mipmaps que se mete, o las opciones, se compila todo de cero)
ESPERADO=437208a  # (el commit probado; MSYS2 no trae git: el de Git para Windows, si está)
GIT=$(command -v git || ls "/c/Program Files/Git/cmd/git.exe" 2>/dev/null || true)
if [ -n "$GIT" ] && [ "$("$GIT" -C "$S" rev-parse --short=7 HEAD 2>/dev/null)" != "$ESPERADO" ]; then
    echo "lsfg: aviso: $S no está en el commit $ESPERADO, el probado"
fi
# (la huella: todas sus fuentes y cabeceras, con su tamaño y su hora; sin depender de git)
SELLO=$({ find "$S/framegen" "$S/thirdparty/dxbc" "$S/thirdparty/pe-parse/pe-parser-library" "$S/thirdparty/volk" \
            "$S/src/extract" -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.c' -o -name '*.cpp' -o -name '*.inc' \) \
            -printf '%p %s %T@\n' 2>/dev/null | sort
          cat tools/lsfg/lsfg_smrpg.h
          echo "$CC|$CXX|$COMMON|$INC|$LDFLAGS|${NDK:-}"; } | sha256sum | cut -c1-16)
if [ "$(cat "$O/sello" 2>/dev/null)" != "$SELLO" ]; then
    rm -rf "$O/obj" && mkdir -p "$O/obj" && echo "$SELLO" > "$O/sello"
fi
# las listas (como sus CMakeLists)
{
    find "$S/framegen/src" "$S/framegen/v3.1_src" "$S/framegen/v3.1p_src" -name '*.cpp' | sed 's/^/c++20 /'
    ls "$S"/thirdparty/dxbc/src/dxbc/*.cpp "$S"/thirdparty/dxbc/src/spirv/*.cpp "$S"/thirdparty/dxbc/src/util/*.cpp | sed 's/^/c++17 /'
    for f in buffer parse $UNICODE; do echo "c++17 $S/thirdparty/pe-parse/pe-parser-library/src/$f.cpp"; done
    echo "c $S/thirdparty/volk/volk.c"
    echo "c++20 tools/lsfg/lsfg_smrpg.cpp"
} > "$O/fuentes.txt"
compilar() {  # <std> <fuente>: su objeto (solo si cambió)
    local std=$1 src=$2 obj
    obj=$O/obj/$(echo "$src" | sed 's|^\.\./||; s|[/.]|_|g').o
    [ -f "$obj" ] && [ "$obj" -nt "$src" ] && { echo "$obj"; return; }
    if [ "$std" = c ]; then $CC $COMMON $INC -c "$src" -o "$obj" || return 1
    else $CXX -std=$std $COMMON $INC -c "$src" -o "$obj" || return 1; fi
    echo "$obj"
}
export -f compilar
export O CC CXX COMMON INC
xargs -P "${JOBS:-$(nproc)}" -L1 bash -c 'compilar "$0" "$1"' < "$O/fuentes.txt" > "$O/objetos.txt" || { echo "lsfg: falló la compilación"; exit 1; }
[ "$(wc -l < "$O/objetos.txt")" = "$(wc -l < "$O/fuentes.txt")" ] || { echo "lsfg: falló la compilación"; exit 1; }
# shellcheck disable=SC2046
$CXX -o "$OUT" $(cat "$O/objetos.txt") $LDFLAGS
echo "lsfg: $OUT ($(stat -c %s "$OUT") bytes)"
