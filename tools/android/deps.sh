#!/bin/bash
# tools/android/deps.sh <abi> [jobs]: the program's dependencies for Android, built with the NDK, in deps/<abi>/ (include/ and
# lib/), next to the repository's folder. <abi>: x86_64 (the emulator) or arm64-v8a (phones). In MSYS2 UCRT64 (Windows), with
# the Android SDK (ANDROID_SDK; NDK 29, its CMake and Ninja):
#   - SDL3 3.4.20 (the PC port's; libSDL3.so and, from its android-project, the Java that starts it),
#   - FFmpeg 9.0.2 (libavcodec/avformat/avutil/swscale.so, LGPL; the decoders the game's movies use, as
#     tools/build_deps_windows.sh),
#   - Opus (static: hwopus),
#   - shaderc (static: the GLSL of the translated shaders to SPIR-V).
# What is already done is skipped (delete deps/<abi>/.<step> to do it again). Downloads are checked by their sha256.
set -euo pipefail
ABI=${1:?usage: deps.sh x86_64|arm64-v8a [jobs]}
JOBS=${2:-$(nproc)}
case $ABI in
    x86_64) TRIPLE=x86_64-linux-android FFARCH=x86_64 ;;
    arm64-v8a) TRIPLE=aarch64-linux-android FFARCH=aarch64 ;;
    *) echo "unknown ABI: $ABI"; exit 2 ;;
esac
API=33
HERE=$(cd "$(dirname "$0")/../.." && pwd)
ROOT=$(cd "$HERE/.." && pwd)
. "$HERE/tools/local_env.sh"
SDK=${ANDROID_SDK:?the Android SDK is missing: put ANDROID_SDK in local.env (see local.env.example) or in the environment}
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
        6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1 opus-1.6.1.tar.gz  # (SHA256SUMS.txt from xiph.org)
    [ -d "$SRC/opus-1.6.1" ] || tar -xzf "$SRC/opus-1.6.1.tar.gz" -C "$SRC"
    cmake_android "$SRC/opus-1.6.1" "$B/opus" -DOPUS_BUILD_SHARED_LIBRARY=OFF -DOPUS_BUILD_TESTING=OFF \
        -DOPUS_BUILD_PROGRAMS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    mark opus
    echo "deps $ABI: Opus"
fi

# ---- FFmpeg (LGPL, shared: as on Windows)
if ! done_step ffmpeg; then
    FF=${FFMPEG_TAR:-}  # (one already downloaded, e.g. the one in build/deps-win/src of the PC port; if not, from ffmpeg.org)
    [ -f "$SRC/ffmpeg-9.0.2.tar.xz" ] || { [ -n "$FF" ] && cp "$FF" "$SRC/"; } 2>/dev/null ||
        curl -fsSL -o "$SRC/ffmpeg-9.0.2.tar.xz" https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz
    echo "8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e  $SRC/ffmpeg-9.0.2.tar.xz" | sha256sum -c --quiet
    # (its configure does not take spaces in paths -its temporary files, --cc without quotes-: it is built in a temporary
    # folder without them, and the NDK's tools go through wrappers there. The NDK cannot be ahead in the PATH: its
    # libwinpthread-1.dll breaks the cc1 of the host gcc, which configure tries too)
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
        { echo "deps $ABI: FFmpeg failed (see $B/ffmpeg.*.log; built in $FB)"; exit 1; }
    cp -R "$FB/root/ffmpeg/include/." "$P/include/"
    cp "$FB/root/ffmpeg/lib/"*.so "$P/lib/"
    grep -q 'GPL version 3\|--enable-gpl' "$FB/root/ffmpeg/lib/pkgconfig/libavcodec.pc" && { echo "FFmpeg is not LGPL"; exit 1; }
    rm -rf "$FB"
    mark ffmpeg
    echo "deps $ABI: FFmpeg"
fi

# ---- shaderc 2026.4 (the PC port's, from MSYS2) with glslang 16.6.0 and SPIRV-Tools / SPIRV-Headers 1.4.363 (GitHub's,
# checked by their sha256): the NDK's (below, no longer used) is old and does not know extensions that the shader translator
# declares (GL_EXT_texture_shadow_lod): without this one, no shader compiled
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
    rm -f "$P/lib/libshaderc_shared.so" "$P/lib/libSPIRV-Tools-shared.so"  # (the static one is linked, libshaderc_combined.a)
    [ -f "$P/lib/libshaderc_combined.a" ] || { echo "deps $ABI: shaderc 2026.4 left no libshaderc_combined.a"; exit 1; }
    mark shaderc2026
    mark shaderc  # (the NDK's, below, is no longer needed)
    echo "deps $ABI: shaderc 2026.4"
fi

# ---- shaderc (the NDK's: old; only if the one above was not built)
if ! done_step shaderc; then
    [ -d "$SRC/shaderc" ] || cp -R "$NDK/sources/third_party/shaderc" "$SRC/shaderc"
    # (its libraries; the NDK's step that joins them, an llvm-ar MRI script, fails on Windows because of the quotes: they are
    # joined here with llvm-ar qcL, which puts the contents of the others into one archive)
    SC_LIBS="libshaderc libshaderc_util libglslang libOGLCompiler libOSDependent libSPIRV libHLSL libSPIRV-Tools-opt libSPIRV-Tools"
    (cd "$SRC/shaderc" && /c/Windows/System32/cmd.exe //c "$(cygpath -w "$NDK/ndk-build.cmd")" NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=Android.mk \
        APP_STL=c++_static APP_ABI=$ABI APP_PLATFORM=android-$API NDK_OUT=obj NDK_LIBS_OUT=libs \
        libshaderc_combined -j"$JOBS" > "$B/shaderc.build.log" 2>&1) || true  # (fails when joining: below)
    for l in $SC_LIBS; do
        [ -f "$SRC/shaderc/obj/local/$ABI/$l.a" ] || { echo "deps $ABI: shaderc failed, $l.a is missing (see $B/shaderc.build.log)"; exit 1; }
    done
    rm -f "$P/lib/libshaderc_combined.a"
    (cd "$SRC/shaderc/obj/local/$ABI" && "$TC/bin/llvm-ar.exe" qcL "$PM/lib/libshaderc_combined.a" $(printf '%s.a ' $SC_LIBS) &&
        "$TC/bin/llvm-ranlib.exe" "$PM/lib/libshaderc_combined.a")
    mkdir -p "$P/include/shaderc" && cp "$SRC/shaderc/libshaderc/include/shaderc/"*.h "$P/include/shaderc/"
    mark shaderc
    echo "deps $ABI: shaderc"
fi
# ---- the Vulkan headers: MSYS2's (1.4.363, the PC port's); the NDK's are 1.3.275
if ! done_step vulkan; then
    cp -R /ucrt64/include/vulkan /ucrt64/include/vk_video /ucrt64/include/vma "$P/include/"
    mark vulkan
fi
echo "deps $ABI: ready in $P"
