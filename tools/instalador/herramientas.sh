#!/bin/bash
# tools/instalador/herramientas.sh <salida>: la carpeta construir\ que va con el programa (`make instalador`), con todo lo
# que necesita construir.dll para construir el código del juego en el PC de quien instala, sin nada de ese PC:
#   - construir.dll (ya en <salida>) y sus DLL: libclang-cpp, libLLVM y lo que cargan (de MSYS2 UCRT64);
#   - clang\include: las cabeceras de clang (enteras: son las suyas, 16 MB);
#   - mingw\include: de las de MinGW-w64, solo las que incluye el código del juego (cpu.h y simd.h, que incluye
#     simd_fp.h; gamelib_entry.c y game_overrides.c; con las opciones del código del juego, para x86-64-v3, -v2 y
#     x86-64): la lista sale de clang -MD;
#   - mingw\lib: el CRT de MinGW-w64 y libgcc que usa la línea de enlace de `make` (construir.cpp link_lib);
#   - python\: Python 3.12 «embeddable» de python.org (su sha256 fijado), aislado por python312._pth, con capstone,
#     unicorn y lz4 (los del entorno del generador, build/gen-venv) y el generador (tools/gen.py y sus módulos y datos).
# Comprueba al final que una compilación de prueba con solo estas cabeceras sale bien.
set -euo pipefail
cd "$(dirname "$0")/../.."
O=$1
U=/ucrt64
CLANG_VER=$(ls $U/lib/clang | sort -V | tail -1)
GCC_DIR=$(ls -d $U/lib/gcc/x86_64-w64-mingw32/* | sort -V | tail -1)
W=build/construir-trabajo
rm -rf "$W" "$O/clang" "$O/mingw" "$O/python"
mkdir -p "$W" "$O/clang" "$O/mingw/include" "$O/mingw/lib" "$O/python"

# 1. las DLL que cargan construir.dll y libclang-cpp / libLLVM (las de MSYS2; las de una versión anterior, fuera).
#    libffi-8.dll se llama igual que la de Python: la de aquí la carga LLVM primero y _ctypes usa esa (Windows reutiliza
#    la DLL ya cargada con ese nombre); es la misma API y la libffi de MinGW admite la convención FFI_WIN64 que pide el
#    Python de MSVC. La copia de Python (python\libffi-8.dll) queda de respaldo.
find "$O" -maxdepth 1 -name '*.dll' ! -name construir.dll -delete
for d in $(ldd "$O/construir.dll" | awk '/ucrt64/ {print $3}' | sort -u); do cp "$d" "$O/"; done

# 2. las cabeceras de clang (enteras) y las de MinGW-w64 que usa el código del juego
cp -R $U/lib/clang/$CLANG_VER/include "$O/clang/include"
printf '#include "cpu.h"\n#include "simd.h"\n' > $W/probe.c  # (simd_fp.h es una plantilla de simd.h: no va sola)
for cpu in x86-64-v3 x86-64-v2 x86-64; do
    for src in $W/probe.c tools/gamelib_entry.c tools/game_overrides.c; do
        clang -O1 -fno-strict-aliasing -fvisibility=hidden -D_FILE_OFFSET_BITS=64 -march=$cpu -Iruntime '-DGAMELIB_ID="0"' \
            -c $src -o $W/o.o -MD -MF $W/$cpu-$(basename $src).d
    done
done
n=0
for h in $(cat $W/*.d | tr ' ' '\n' | tr -d '\r\\' | grep -i "msys64/ucrt64/include/" | sed 's|.*/ucrt64/include/||I' | sort -u); do
    mkdir -p "$O/mingw/include/$(dirname "$h")"
    cp "$U/include/$h" "$O/mingw/include/$h"
    n=$((n + 1))
done
echo "construir: $n cabeceras de MinGW-w64"

# 3. el CRT de MinGW-w64 y libgcc (la línea de enlace de `make`: clang -shared -###)
cp $U/lib/dllcrt2.o $GCC_DIR/crtbegin.o $GCC_DIR/crtend.o "$O/mingw/lib/"
for l in mingw32 moldname mingwex msvcrt pthread advapi32 shell32 user32 kernel32; do cp $U/lib/lib$l.a "$O/mingw/lib/"; done
cp $GCC_DIR/libgcc.a "$O/mingw/lib/"
if [ -f $GCC_DIR/libgcc_s.a ]; then cp $GCC_DIR/libgcc_s.a "$O/mingw/lib/"; else cp $U/lib/libgcc_s.a "$O/mingw/lib/"; fi

# 4. Python embebido (aislado), sus paquetes y el generador
PYZ=build/fuentes/python-3.12.10-embed-amd64.zip
PYSHA=4acbed6dd1c744b0376e3b1cf57ce906f9dc9e95e68824584c8099a63025a3c3  # (python.org; firmado por la PSF)
[ -f $PYZ ] || { mkdir -p build/fuentes; curl -fsSL -o $PYZ https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip; }
echo "$PYSHA  $PYZ" | sha256sum -c --quiet
build/gen-venv/Scripts/python.exe -I -m zipfile -e $PYZ "$O/python"  # (sin unzip: el Python del entorno del generador)
# (lo que el generador no usa: red, cifrado, bases de datos, el propio python.exe)
rm -f "$O/python/"{python.exe,pythonw.exe,python.cat,python312._pth,_ssl.pyd,_hashlib.pyd,libssl-3.dll,libcrypto-3.dll,_sqlite3.pyd,sqlite3.dll,_msi.pyd,_wmi.pyd,_socket.pyd,_asyncio.pyd,_overlapped.pyd,_uuid.pyd,winsound.pyd}
cp tools/instalador/python312._pth "$O/python/"
mkdir -p "$O/python/Lib/site-packages" "$O/python/gen/tools/rando" "$O/python/gen/runtime/game_addresses"
for pk in capstone unicorn lz4; do cp -R build/gen-venv/Lib/site-packages/$pk "$O/python/Lib/site-packages/"; done
find "$O/python/Lib/site-packages" -name '__pycache__' -prune -exec rm -rf {} +
cp tools/gen.py tools/lifter.py tools/analyze.py tools/nso.py tools/passes.py tools/patches.py tools/simd_c.py \
   tools/game_addresses.py "$O/python/gen/tools/"
cp tools/rando/patch_variants.json "$O/python/gen/tools/rando/"
cp runtime/game_addresses/*.h "$O/python/gen/runtime/game_addresses/"

# 5. comprobación: el código del juego compila con solo estas cabeceras (clang, -nostdinc)
for src in $W/probe.c tools/game_overrides.c; do
    clang -O1 -fno-strict-aliasing -march=x86-64-v3 -nostdinc -isystem "$O/clang/include" -isystem "$O/mingw/include" \
        -Iruntime -c $src -o $W/o2.o || { echo "construir: $src no compila con las cabeceras copiadas"; exit 1; }
done
# 6. en lo de arriba, solo lo que se espera (lo que haya de más, un temporal olvidado..., iría dentro del instalador)
extra=$(cd "$O" && ls -A | grep -vxE 'construir\.dll|[A-Za-z0-9_+.-]+\.dll|clang|mingw|python' || true)
[ -z "$extra" ] || { echo "construir: sobra en $O: $extra"; exit 1; }
echo "construir: $(find "$O" -type f | wc -l) archivos, $(du -sh "$O" | cut -f1)"
