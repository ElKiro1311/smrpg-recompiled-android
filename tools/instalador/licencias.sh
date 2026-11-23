#!/bin/bash
# tools/instalador/licencias.sh <carpeta que se reparte>: <carpeta>/licencias con los textos de las licencias de todo lo
# que va con el programa (LICENCIAS.txt los enumera) y, en licencias/fuentes, el código fuente de lo que pide darlo:
# Unicorn 2.1.4 (GPL-2.0, el sdist de PyPI comprobado por su sha256), que el generador carga dentro del proceso del
# instalador, y por eso también el del instalador y su constructor (tools/instalador). Las fuentes del generador van
# tal cual en construir/python/gen. Desde el Makefile (`make instalador`), en MSYS2 UCRT64.
set -euo pipefail
cd "$(dirname "$0")/../.."
D=$1/licencias
mkdir -p "$D/fuentes"
U=/ucrt64/share/licenses
VENV=build/gen-venv/Lib/site-packages

cp LICENSE "$D/LAS-Recomp-LICENSE.txt"
cp tools/instalador/LICENCIAS.txt "$D/LEEME-LICENCIAS.txt"
cp third_party/amd-fsr/AMD-FSR-SDK-LICENSE.md "$D/"
cp build/deps-win/src/ffmpeg/COPYING.LGPLv2.1 "$D/FFmpeg-LGPLv2.1.txt"
cp build/deps-win/src/ffmpeg-9.0.2.tar.xz "$D/fuentes/"  # (LGPL: su fuente va con el binario; sha256 en build_deps_windows.sh)
cp third_party/ryujinx/LICENSE.txt "$D/Ryujinx-LICENSE.txt"  # (runtime/services/audio.c: el mezclador y la reverb portados)
cp third_party/fonts/Baloo2/OFL.txt "$D/Baloo2-OFL.txt"
cp third_party/fonts/BIZUDPGothic/OFL.txt "$D/BIZUDPGothic-OFL.txt"
cp third_party/fonts/DejaVu/LICENSE "$D/DejaVu-LICENSE.txt"
cp third_party/imgui/LICENSE.txt "$D/DearImGui-LICENSE.txt"
cp third_party/smaa/LICENSE.txt "$D/SMAA-LICENSE.txt"
cp third_party/snapdragon-gsr/LICENSE.txt "$D/Snapdragon-GSR-LICENSE.txt"
# las DLL de MSYS2 que van con el programa y con el constructor (y zstd, dentro del instalador)
cp $U/SDL3/LICENSE.txt "$D/SDL3-LICENSE.txt"
cp $U/opus/COPYING "$D/Opus-COPYING.txt"
cp $U/shaderc/LICENSE "$D/shaderc-LICENSE.txt"
cp $U/glslang/LICENSE.txt "$D/glslang-LICENSE.txt"
cp $U/spirv-tools/LICENSE "$D/SPIRV-Tools-LICENSE.txt"
cp $U/libiconv/COPYING.LIB "$D/libiconv-COPYING.LIB.txt"
cp $U/winpthreads/COPYING "$D/winpthreads-COPYING.txt"
cp $U/gcc/COPYING.RUNTIME "$D/GCC-COPYING.RUNTIME.txt"
cp $U/gcc/COPYING3 "$D/GCC-COPYING3.txt"
cp $U/crt/COPYING.MinGW-w64-runtime.txt "$D/MinGW-w64-runtime-COPYING.txt"
cp $U/headers/COPYING "$D/MinGW-w64-headers-COPYING.txt"
cp $U/zstd/LICENSE "$D/zstd-LICENSE.txt"
cp $U/vulkan-memory-allocator/LICENSE.txt "$D/VMA-LICENSE.txt"  # (va entero dentro de smrpg-recomp.exe: vma.cpp)
cp $U/llvm/LICENSE "$D/LLVM-LICENSE.txt"      # (LLVM, clang y lld: Apache 2.0 con las excepciones de LLVM)
cp $U/libffi/LICENSE "$D/libffi-LICENSE.txt"
cp $U/libxml2/COPYING "$D/libxml2-COPYING.txt"
cp $U/zlib/LICENSE "$D/zlib-LICENSE.txt"
# el generador: Python embebido, Capstone, lz4, Unicorn
build/gen-venv/Scripts/python.exe -I -c "import sys, zipfile; sys.stdout.buffer.write(zipfile.ZipFile(sys.argv[1]).read('LICENSE.txt'))" \
    build/fuentes/python-3.12.10-embed-amd64.zip > "$D/Python-LICENSE.txt"
cp $VENV/capstone-5.0.9.dist-info/LICENSE.TXT "$D/Capstone-LICENSE.txt"
cp "$(ls $VENV/lz4-4.4.5.dist-info/licenses/* | head -1)" "$D/lz4-LICENSE.txt"
# (la de arriba es la de los enlaces de Python; la biblioteca LZ4 en C que llevan dentro es BSD-2: su aviso, de su
# código fuente, el sdist de PyPI comprobado por su sha256)
L4=build/fuentes/lz4-4.4.5.tar.gz
L4SHA=5f0b9e53c1e82e88c10d7c180069363980136b9d7a8306c4dca4f760d60c39f0
[ -f $L4 ] || { mkdir -p build/fuentes; curl -fsSL -o $L4 https://files.pythonhosted.org/packages/source/l/lz4/lz4-4.4.5.tar.gz; }
echo "$L4SHA  $L4" | sha256sum -c --quiet
tar -xzOf $L4 lz4-4.4.5/lz4libs/lz4.h | awk 'NR==1,/\*\//' > "$D/LZ4-biblioteca-BSD-2.txt"
# de dónde sale, exactamente, lo que se reparte de MSYS2 (sus paquetes fuente: lo que piden las licencias GPL/LGPL de
# libgcc, libstdc++, libiconv...)
MSYS2_SRC=$D/MSYS2-fuentes.txt
{
    echo "Paquetes de MSYS2 UCRT64 de los que salen las bibliotecas que van con el programa y con construir\\,"
    echo "y su código fuente completo (paquetes fuente de MSYS2, con los parches y la receta de compilación):"
    for base in $(for p in libstdc++ libgcc gcc crt headers libwinpthread libiconv llvm-libs clang-libs lld libffi libxml2 zlib zstd \
                      sdl3 opus shaderc glslang spirv-tools; do
                      d=$(ls -d /var/lib/pacman/local/mingw-w64-ucrt-x86_64-$p-[0-9]* 2>/dev/null | head -1)
                      [ -n "$d" ] && echo "$(awk '/%BASE%/{getline; print}' $d/desc)-$(awk '/%VERSION%/{getline; print}' $d/desc)"
                  done | sort -u); do
        echo "  $base: https://repo.msys2.org/mingw/sources/$base.src.tar.zst"
    done
} > "$MSYS2_SRC"
grep -q "mingw-w64-gcc-" "$MSYS2_SRC" && grep -q "mingw-w64-llvm-" "$MSYS2_SRC" || { echo "licencias: faltan paquetes en $MSYS2_SRC"; exit 1; }
S=build/fuentes/unicorn-2.1.4.tar.gz
SHA=00567a70e323f749b419cd86bee4f9115beab7ebba32194581c090cbb7c59cff  # (pypi.org/pypi/unicorn/2.1.4/json, el sdist)
[ -f $S ] || { mkdir -p build/fuentes; curl -fsSL -o $S \
    https://files.pythonhosted.org/packages/b2/1b/b4248aa8422e86de690cf8e85cf8feae4c33405a097d1ebe71570bdaa6f5/unicorn-2.1.4.tar.gz; }
echo "$SHA  $S" | sha256sum -c --quiet
cp $S "$D/fuentes/"
tar -xzOf $S unicorn-2.1.4/src/qemu/COPYING > "$D/Unicorn-GPL-2.0.txt"
# el instalador y su constructor (Unicorn se carga en su proceso): sus fuentes
build/gen-venv/Scripts/python.exe -I - "$(cygpath -w "$D/fuentes/instalador-fuente.zip")" <<'EOF'
import glob, sys, zipfile
fs = sorted(glob.glob('tools/instalador/*')) + ['LICENSE', 'Makefile', 'docs/icon/icon.ico',
                                                  'runtime/game_addresses/romfs_files.h']  # (instalador.c la incluye)
with zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED) as z:
    for f in fs:
        z.write(f, 'instalador-fuente/' + f.replace('\\', '/'))
EOF
echo "licencias: $(ls "$D" | wc -l) archivos, fuentes: $(ls "$D/fuentes" | tr '\n' ' ')"
