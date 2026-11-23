#!/bin/bash
# tools/android/apk.sh [abi...]: build/apk/smrpg-recomp-android-<version>.apk, with the program of
# build/android-<abi>/libmain.so (`make PLATFORM=android ABI=<abi>`) for each ABI (by default, the ones that are built:
# x86_64 for the emulator, arm64-v8a for phones) and its libraries (../deps/<abi>, tools/android/deps.sh).
# Nothing from the game: the game (extracted/ with its code, lasgame.so) goes apart, in the app's data folder
# (runtime/platform/android.c). Without Gradle: javac, d8, aapt2, zipalign and apksigner from the Android SDK and the JDK
# (ANDROID_SDK and JDK: from the environment or from local.env, see local.env.example).
# The version: the first "x.y.z-alpha|beta (date)" line of CHANGELOG-ANDROID.txt; its versionCode, x*10000+y*100+z.
# Signature: ../claves/depuracion.keystore (made the first time; it does not go to the repository), or KEYSTORE=<path>.
set -euo pipefail
cd "$(dirname "$0")/../.."
. tools/local_env.sh
SDK=${ANDROID_SDK:?the Android SDK is missing: put ANDROID_SDK in local.env (see local.env.example) or in the environment}
BT=$SDK/build-tools/36.0.0
AJAR=$(cygpath -w "$SDK/platforms/android-36/android.jar")
JDK=${JDK:?the JDK is missing: put JDK in local.env (see local.env.example) or in the environment}
NDKTC=${NDK:-$SDK/ndk/29.0.14206865}/toolchains/llvm/prebuilt/windows-x86_64
PY=${PY:-build/gen-venv/Scripts/python.exe}
SDLJ=../deps/src/SDL3-3.4.20/android-project/app/src/main/java
w() { cygpath -w "$1"; }

VN=$(sed -n 's/^\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*-[a-z]*\) .*/\1/p' CHANGELOG-ANDROID.txt | head -1)
[ -n "$VN" ] || { echo "apk: the version is missing in CHANGELOG-ANDROID.txt"; exit 1; }
IFS=.- read -r X Y Z _ <<< "$VN"
VC=$((X * 10000 + Y * 100 + Z))
ABIS=${*:-}
[ -n "$ABIS" ] || for a in x86_64 arm64-v8a; do [ -f "build/android-$a/libmain.so" ] && ABIS="$ABIS $a"; done
[ -n "$ABIS" ] || { echo "apk: no build/android-<abi>/libmain.so (make PLATFORM=android ABI=...)"; exit 1; }

O=build/apk
W=$O/work
rm -rf "$W" && mkdir -p "$W/classes" "$W/dex" "$W/res" "$W/assets" "$W/lib"

# 1. the Java: SDL3's (its android-project) and our activity. The lists go in arrays: cygpath -w gives absolute paths
# (because of the "..") and the folder may have spaces.
list_w() { local f; while IFS= read -r -d '' f; do printf '%s\0' "$(w "$f")"; done; }
mapfile -d '' JAVAS < <(find "$SDLJ" tools/android/app/java -name '*.java' -print0 | list_w)
"$JDK/bin/javac.exe" -source 8 -target 8 -bootclasspath "$AJAR" -nowarn -Xlint:-options -encoding UTF-8 -d "$(w "$W/classes")" \
    "${JAVAS[@]}"
mapfile -d '' CLASES < <(find "$W/classes" -name '*.class' -print0 | list_w)
"$JDK/bin/java.exe" -cp "$(w "$BT/lib/d8.jar")" com.android.tools.r8.D8 --release --min-api 33 --lib "$AJAR" \
    --output "$(w "$W/dex")" "${CLASES[@]}"

# 2. resources, manifest and assets (what goes with the program: runtime/platform/android.c)
cp -R tools/android/app/res/. "$W/res/"
mkdir -p "$W/res/mipmap-xxxhdpi" && cp docs/icon/icon_512.png "$W/res/mipmap-xxxhdpi/ic_launcher.png"
cp third_party/fonts/Baloo2/Baloo2-SemiBold.ttf third_party/fonts/BIZUDPGothic/BIZUDPGothic-Regular.ttf \
   third_party/fonts/DejaVu/DejaVuSans.ttf third_party/fonts/DejaVu/DejaVuSans-Bold.ttf "$W/assets/"
cp docs/icon/icon_512.png "$W/assets/icon.png"
# the licenses of what goes in the APK (assets/licenses, summed up in LICENSES.txt): the port, its parts, the fonts and the
# Android libraries (from the source packages of tools/android/deps.sh and from the NDK)
L="$W/assets/licenses" && mkdir -p "$L"
DS=../deps/src
cp tools/android/LICENSES-ANDROID.txt "$L/LICENSES.txt"
cp LICENSE "$L/LAS-Recomp-LICENSE.txt"
cp third_party/imgui/LICENSE.txt "$L/DearImGui-LICENSE.txt"
cp third_party/smaa/LICENSE.txt "$L/SMAA-LICENSE.txt"
cp third_party/snapdragon-gsr/LICENSE.txt "$L/Snapdragon-GSR-LICENSE.txt"
cp third_party/ryujinx/LICENSE.txt "$L/Ryujinx-LICENSE.txt"  # (runtime/services/audio.c: the ported audio)
cp third_party/amd-fsr/AMD-FSR-SDK-LICENSE.md "$L/"  # (AMD FSR 1 in GLSL: ffx_fsr1.h, MIT)
VMAL=/ucrt64/share/licenses/vulkan-memory-allocator/LICENSE.txt
[ -f "$VMAL" ] || VMAL=/c/msys64/ucrt64/share/licenses/vulkan-memory-allocator/LICENSE.txt
cp "$VMAL" "$L/VMA-LICENSE.txt"  # (Vulkan Memory Allocator, all of it inside libmain.so: vma.cpp)
cp third_party/fonts/Baloo2/OFL.txt "$L/Baloo2-OFL.txt"
cp third_party/fonts/BIZUDPGothic/OFL.txt "$L/BIZUDPGothic-OFL.txt"
cp third_party/fonts/DejaVu/LICENSE "$L/DejaVu-LICENSE.txt"
cp "$DS/SDL3-3.4.20/LICENSE.txt" "$L/SDL3-LICENSE.txt"
cp "$DS/opus-1.6.1/COPYING" "$L/Opus-COPYING.txt"
cp "$DS/shaderc-2026.4/LICENSE" "$L/shaderc-LICENSE.txt"
tar -xzOf "$DS/glslang-16.6.0.tar.gz" glslang-16.6.0/LICENSE.txt > "$L/glslang-LICENSE.txt"
tar -xzOf "$DS/SPIRV-Tools-1.4.363.0.tar.gz" SPIRV-Tools-vulkan-sdk-1.4.363.0/LICENSE > "$L/SPIRV-Tools-LICENSE.txt"
tar -xJOf "$DS/ffmpeg-9.0.2.tar.xz" ffmpeg-9.0.2/COPYING.LGPLv2.1 > "$L/FFmpeg-LGPLv2.1.txt"
cp "$NDKTC/NOTICE" "$L/LLVM-libcxx-NOTICE.txt"
# (Lossless Scaling frame generation: liblsfg.so, from tools/lsfg/build.sh; with it, always its licenses: MIT and zlib ask
# for their notice to go with every copy)
LS=${LSFG_SRC:-../deps/src/lsfg-vk-android}
if ls build/lsfg/android-*/liblsfg.so > /dev/null 2>&1; then
    for lic in LICENSE.md thirdparty/dxbc/LICENSE.md thirdparty/pe-parse/LICENSE thirdparty/volk/LICENSE.md; do
        [ -f "$LS/$lic" ] || { echo "apk: liblsfg.so is there but not $LS/$lic (LSFG_SRC: where its sources are)"; exit 1; }
    done
    cp "$LS/LICENSE.md" "$L/lsfg-vk-LICENSE.txt"
    cp "$LS/thirdparty/dxbc/LICENSE.md" "$L/DXVK-dxbc-LICENSE.txt"
    cp "$LS/thirdparty/pe-parse/LICENSE" "$L/pe-parse-LICENSE.txt"
    cp "$LS/thirdparty/volk/LICENSE.md" "$L/volk-LICENSE.txt"
fi
"$BT/aapt2.exe" compile --dir "$(w "$W/res")" -o "$(w "$W/res.zip")"
"$BT/aapt2.exe" link -o "$(w "$W/base.apk")" -I "$AJAR" --manifest "$(w tools/android/app/AndroidManifest.xml)" \
    "$(w "$W/res.zip")" -A "$(w "$W/assets")" --min-sdk-version 33 --target-sdk-version 36 \
    --version-code "$VC" --version-name "$VN" ${APK_DEBUG:+--debug-mode}  # (APK_DEBUG=1: a debuggable APK)

# 3. the native libraries, without the debug information
for a in $ABIS; do
    mkdir -p "$W/lib/$a"
    for so in build/android-$a/libmain.so ../deps/$a/lib/*.so $(ls build/lsfg/android-$a/liblsfg.so 2>/dev/null); do
        "$NDKTC/bin/llvm-strip.exe" --strip-debug -o "$(w "$W/lib/$a/$(basename "$so")")" "$(w "$so")"
    done
done

# 4. put it together, align and sign
"$PY" -I tools/android/apk_zip.py "$(w "$W/base.apk")" "$(w "$W/dex")" "$(w "$W/lib")" "$(w "$W/unsigned.apk")"
"$BT/zipalign.exe" -P 16 -f 4 "$(w "$W/unsigned.apk")" "$(w "$W/aligned.apk")"
KS=${KEYSTORE:-../claves/depuracion.keystore}  # (the Spanish names stay: they are those of the key the published APKs are signed with)
if [ ! -f "$KS" ]; then
    mkdir -p "$(dirname "$KS")"
    "$JDK/bin/keytool.exe" -genkeypair -keystore "$(w "$KS")" -storepass android -keypass android -alias depuracion \
        -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=SMRPG Recomp (depuracion)" > /dev/null
fi
APK=$O/smrpg-recomp-android-$VN.apk
"$JDK/bin/java.exe" -jar "$(w "$BT/lib/apksigner.jar")" sign --ks "$(w "$KS")" --ks-pass pass:android \
    --key-pass pass:android --out "$(w "$APK")" "$(w "$W/aligned.apk")"
"$JDK/bin/java.exe" -jar "$(w "$BT/lib/apksigner.jar")" verify "$(w "$APK")"
ls -la "$APK" | awk '{print "apk: " $9 " (" $5 " bytes), ABI:'"$ABIS"', version '"$VN"' (" '"$VC"' ")"}'
