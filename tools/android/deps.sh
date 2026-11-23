#!/bin/bash
# tools/android/deps.sh <abi> [jobs]: las dependencias del programa para Android, compiladas con el NDK, en
# deps/<abi>/ (include/ y lib/), junto a la carpeta del repositorio. <abi>: x86_64 (el emulador) o arm64-v8a (los
# móviles). En MSYS2 UCRT64 (Windows), con el SDK de Android de tools/android (ANDROID_SDK; NDK 29, su CMake y Ninja):
#   - SDL3 3.4.20 (la del port de PC; libSDL3.so y, de su android-project, el Java que la arranca),
#   - FFmpeg 9.0.2 (libavcodec/avformat/avutil/swscale.so, LGPL; los decodificadores que usan los vídeos del juego,
#     como tools/build_deps_windows.sh),
#   - Opus (estática: hwopus),
#   - shaderc (estática: el GLSL de los shaders traducidos a SPIR-V; la del NDK, sources/third_party/shaderc).
# Lo ya hecho se salta (borrar deps/<abi>/.<paso> para rehacerlo). Las descargas se comprueban por su sha256.
set -euo pipefail
ABI=${1:?uso: deps.sh x86_64|arm64-v8a [hilos]}
JOBS=${2:-$(nproc)}
case $ABI in
    x86_64) TRIPLE=x86_64-linux-android FFARCH=x86_64 ;;
    arm64-v8a) TRIPLE=aarch64-linux-android FFARCH=aarch64 ;;
    *) echo "ABI desconocida: $ABI"; exit 2 ;;
esac
API=33
HERE=$(cd "$(dirname "$0")/../.." && pwd)
ROOT=$(cd "$HERE/.." && pwd)
. "$HERE/tools/local_env.sh"
SDK=${ANDROID_SDK:?falta el SDK de Android: pon ANDROID_SDK en local.env (ver local.env.ejemplo) o en el entorno}
NDK=$(cygpath -m "${NDK:-$SDK/ndk/29.0.14206865}")
TC=$NDK/toolchains/llvm/prebuilt/windows-x86_64
CMAKE=$SDK/cmake/4.1.2/bin/cmake.exe
NINJA=$(cygpath -m "$SDK/cmake/4.1.2/bin/ninja.exe")
SRC=$ROOT/deps/src
P=$ROOT/deps/$ABI
B=$ROOT/deps/build-$ABI
mkdir -p "$SRC" "$P/include" "$P/lib" "$B"
PM=$(cygpath -m "$P")
export TMPDIR="$B/tmp" TMP="$(cygpath -w "$B")\\tmp" TEMP="$(cygpath -w "$B")\\tmp"
mkdir -p "$B/tmp"

fetch() {  # url sha256 file
    [ -f "$SRC/$3" ] || curl -fsSL -o "$SRC/$3" "$1"
    echo "$2  $SRC/$3" | sha256sum -c --quiet
}
done_step() { [ -f "$P/.$1" ]; }
mark() { touch "$P/.$1"; }

cmake_android() {  # src build extra...
    local s=$1 b=$2
    shift 2
    "$CMAKE" --fresh -S "$(cygpath -m "$s")" -B "$(cygpath -m "$b")" -G Ninja -DCMAKE_MAKE_PROGRAM="$NINJA" \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI="$ABI" \
        -DANDROID_PLATFORM=android-$API -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PM" \
        -DCMAKE_C_FLAGS="\"-ffile-prefix-map=$(cygpath -m "$s")=.\"" \
        -DCMAKE_CXX_FLAGS="\"-ffile-prefix-map=$(cygpath -m "$s")=.\"" "$@" > "$b.configure.log"  # (__FILE__ without paths of this PC)
    "$CMAKE" --build "$(cygpath -m "$b")" -j "$JOBS" > "$b.build.log"
    "$CMAKE" --install "$(cygpath -m "$b")" > "$b.install.log"
}

# ---- SDL3
if ! done_step sdl3; then
    fetch https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz \
        f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f SDL3-3.4.20.tar.gz
    [ -d "$SRC/SDL3-3.4.20" ] || tar -xzf "$SRC/SDL3-3.4.20.tar.gz" -C "$SRC"
    cmake_android "$SRC/SDL3-3.4.20" "$B/sdl3" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF \
        -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_TESTS=OFF
    mark sdl3
    echo "deps $ABI: SDL3"
fi

# ---- Opus
if ! done_step opus; then
    fetch https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz \
        6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1 opus-1.6.1.tar.gz  # (SHA256SUMS.txt de xiph.org)
    [ -d "$SRC/opus-1.6.1" ] || tar -xzf "$SRC/opus-1.6.1.tar.gz" -C "$SRC"
    cmake_android "$SRC/opus-1.6.1" "$B/opus" -DOPUS_BUILD_SHARED_LIBRARY=OFF -DOPUS_BUILD_TESTING=OFF \
        -DOPUS_BUILD_PROGRAMS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    mark opus
    echo "deps $ABI: Opus"
fi

# ---- FFmpeg (LGPL, compartida: como en Windows)
if ! done_step ffmpeg; then
    FF=${FFMPEG_TAR:-}  # (uno ya descargado, p. ej. el de build/deps-win/src del port de PC; si no, de ffmpeg.org)
    [ -f "$SRC/ffmpeg-9.0.2.tar.xz" ] || { [ -n "$FF" ] && cp "$FF" "$SRC/"; } 2>/dev/null ||
        curl -fsSL -o "$SRC/ffmpeg-9.0.2.tar.xz" https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz
    echo "8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e  $SRC/ffmpeg-9.0.2.tar.xz" | sha256sum -c --quiet
    # (su configure no admite espacios en las rutas —sus temporales, --cc sin comillas—: se compila en un temporal
    # sin ellos, y las herramientas del NDK van por envoltorios de ahí. El NDK no puede ir delante en el PATH: su
    # libwinpthread-1.dll rompe el cc1 del gcc anfitrión, que configure también prueba)
    FB=$(mktemp -d /tmp/smrpg-ffmpeg-XXXXXX)
    tar -xJf "$SRC/ffmpeg-9.0.2.tar.xz" -C "$FB" --strip-components=1
    mkdir -p "$FB/ndk" "$FB/tmp"
    for t in clang clang++ llvm-ar llvm-nm llvm-ranlib llvm-strip; do
        printf '#!/bin/sh\nexec "%s/bin/%s.exe" "$@"\n' "$(cygpath -u "$TC")" "$t" > "$FB/ndk/$t"
        chmod +x "$FB/ndk/$t"
    done
    (cd "$FB" && export TMPDIR="$FB/tmp" &&
        ./configure --prefix=/ffmpeg --enable-cross-compile --target-os=android --arch=$FFARCH \
        --cc="$FB/ndk/clang --target=$TRIPLE$API" --cxx="$FB/ndk/clang++ --target=$TRIPLE$API" \
        --ar="$FB/ndk/llvm-ar" --nm="$FB/ndk/llvm-nm" --ranlib="$FB/ndk/llvm-ranlib" --strip="$FB/ndk/llvm-strip" --enable-pic \
        --enable-shared --disable-static \
        --disable-autodetect --disable-programs --disable-doc --disable-network --disable-avdevice --disable-avfilter \
        --disable-swresample --disable-everything \
        --enable-protocol=file --enable-demuxer=matroska,mov \
        --enable-decoder=vp9,vp8,vorbis,opus,h264,aac --enable-parser=vp9,vp8,vorbis,opus,h264,aac \
        > "$B/ffmpeg.configure.log" 2>&1 &&
        sed -i "s|$FB|.|g" config.h &&  # (the configuration FFmpeg carries inside, without the paths of this PC)
        make -j"$JOBS" > "$B/ffmpeg.build.log" 2>&1 &&
        make install DESTDIR="$FB/root" > "$B/ffmpeg.install.log" 2>&1) ||
        { echo "deps $ABI: FFmpeg falló (mira $B/ffmpeg.*.log; compilado en $FB)"; exit 1; }
    cp -R "$FB/root/ffmpeg/include/." "$P/include/"
    cp "$FB/root/ffmpeg/lib/"*.so "$P/lib/"
    grep -q 'GPL version 3\|--enable-gpl' "$FB/root/ffmpeg/lib/pkgconfig/libavcodec.pc" && { echo "FFmpeg no es LGPL"; exit 1; }
    rm -rf "$FB"
    mark ffmpeg
    echo "deps $ABI: FFmpeg"
fi

# ---- shaderc 2026.4 (la del port de PC, de MSYS2) con glslang 16.6.0 y SPIRV-Tools / SPIRV-Headers 1.4.363 (las de
# GitHub, comprobadas por su sha256): la del NDK (abajo, ya sin usar) es antigua y no conoce extensiones que el
# traductor de shaders declara (GL_EXT_texture_shadow_lod): sin esta, ningún shader compilaba
if ! done_step shaderc2026; then
    fetch https://github.com/google/shaderc/archive/refs/tags/v2026.4.tar.gz \
        f06ce5bcca94e5df7f34e115743597d0ad2e13c5fe9213c67dc8c76031241947 shaderc-2026.4.tar.gz
    fetch https://github.com/KhronosGroup/glslang/archive/refs/tags/16.6.0.tar.gz \
        9c09b901149c729df745057dafa815278aaa101b84d2b6e14f16a42de52f97f2 glslang-16.6.0.tar.gz
    fetch https://github.com/KhronosGroup/SPIRV-Tools/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz \
        e6c83a215538fbfd265ccf398d3877985055df1273dbed73912bd05307664688 SPIRV-Tools-1.4.363.0.tar.gz
    fetch https://github.com/KhronosGroup/SPIRV-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz \
        5b3a0d099f5c7649e29c0938476c462e71d66ad89a0bde42fa1ba3eb476956c9 SPIRV-Headers-1.4.363.0.tar.gz
    S2=$SRC/shaderc-2026.4
    if [ ! -d "$S2/third_party/spirv-headers" ]; then
        rm -rf "$S2" && tar -xzf "$SRC/shaderc-2026.4.tar.gz" -C "$SRC"
        mkdir -p "$S2/third_party/glslang" "$S2/third_party/spirv-tools" "$S2/third_party/spirv-headers"
        tar -xzf "$SRC/glslang-16.6.0.tar.gz" -C "$S2/third_party/glslang" --strip-components=1
        tar -xzf "$SRC/SPIRV-Tools-1.4.363.0.tar.gz" -C "$S2/third_party/spirv-tools" --strip-components=1
        tar -xzf "$SRC/SPIRV-Headers-1.4.363.0.tar.gz" -C "$S2/third_party/spirv-headers" --strip-components=1
    fi
    rm -f "$P/lib/libshaderc_combined.a"
    cmake_android "$S2" "$B/shaderc2026" -DANDROID_STL=c++_static -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON \
        -DSPIRV_SKIP_EXECUTABLES=ON -DSPIRV_SKIP_TESTS=ON -DSPIRV_WERROR=OFF \
        -DENABLE_GLSLANG_BINARIES=OFF -DGLSLANG_TESTS=OFF -DENABLE_CTEST=OFF -DBUILD_TESTING=OFF
    rm -f "$P/lib/libshaderc_shared.so" "$P/lib/libSPIRV-Tools-shared.so"  # (se enlaza la estática, libshaderc_combined.a)
    [ -f "$P/lib/libshaderc_combined.a" ] || { echo "deps $ABI: shaderc 2026.4 no dejó libshaderc_combined.a"; exit 1; }
    mark shaderc2026
    mark shaderc  # (la del NDK, de abajo, ya no hace falta)
    echo "deps $ABI: shaderc 2026.4"
fi

# ---- shaderc (la del NDK: antigua; solo si no se hizo la de arriba)
if ! done_step shaderc; then
    [ -d "$SRC/shaderc" ] || cp -R "$NDK/sources/third_party/shaderc" "$SRC/shaderc"
    # (sus bibliotecas; el paso del NDK que las junta, un guion MRI de llvm-ar, falla en Windows por las comillas:
    # se juntan aquí con llvm-ar qcL, que mete en un archivo el contenido de los otros)
    SC_LIBS="libshaderc libshaderc_util libglslang libOGLCompiler libOSDependent libSPIRV libHLSL libSPIRV-Tools-opt libSPIRV-Tools"
    (cd "$SRC/shaderc" && /c/Windows/System32/cmd.exe //c "$(cygpath -w "$NDK/ndk-build.cmd")" NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=Android.mk \
        APP_STL=c++_static APP_ABI=$ABI APP_PLATFORM=android-$API NDK_OUT=obj NDK_LIBS_OUT=libs \
        libshaderc_combined -j"$JOBS" > "$B/shaderc.build.log" 2>&1) || true  # (falla al juntar: abajo)
    for l in $SC_LIBS; do
        [ -f "$SRC/shaderc/obj/local/$ABI/$l.a" ] || { echo "deps $ABI: shaderc falló, falta $l.a (mira $B/shaderc.build.log)"; exit 1; }
    done
    rm -f "$P/lib/libshaderc_combined.a"
    (cd "$SRC/shaderc/obj/local/$ABI" && "$TC/bin/llvm-ar.exe" qcL "$PM/lib/libshaderc_combined.a" $(printf '%s.a ' $SC_LIBS) &&
        "$TC/bin/llvm-ranlib.exe" "$PM/lib/libshaderc_combined.a")
    mkdir -p "$P/include/shaderc" && cp "$SRC/shaderc/libshaderc/include/shaderc/"*.h "$P/include/shaderc/"
    mark shaderc
    echo "deps $ABI: shaderc"
fi
# ---- las cabeceras de Vulkan: las de MSYS2 (1.4.363, las del port de PC); las del NDK son de la 1.3.275
if ! done_step vulkan; then
    cp -R /ucrt64/include/vulkan /ucrt64/include/vk_video /ucrt64/include/vma "$P/include/"
    mark vulkan
fi
echo "deps $ABI: listo en $P"
