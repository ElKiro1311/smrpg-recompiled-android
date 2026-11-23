#!/bin/bash
# tools/android/apk.sh [abi...]: build/apk/smrpg-recomp-android-<versión>.apk, con el programa de
# build/android-<abi>/libmain.so (`make PLATFORM=android ABI=<abi>`) para cada ABI (por defecto, las que estén hechas:
# x86_64 para el emulador, arm64-v8a para los móviles) y sus bibliotecas (../deps/<abi>, tools/android/deps.sh).
# Sin nada del juego: el juego (extracted/ con su código, lasgame.so) va aparte, en la carpeta de datos de la app
# (runtime/platform/android.c). Sin Gradle: javac, d8, aapt2, zipalign y apksigner del SDK de Android y del JDK
# (ANDROID_SDK y JDK: del entorno o de local.env, ver local.env.ejemplo).
# La versión: la primera línea «x.y.z-alfa|beta (fecha)» de VERSIONES-ANDROID.txt; su versionCode, x*10000+y*100+z.
# Firma: ../claves/depuracion.keystore (se crea la primera vez; no va al repositorio).
set -euo pipefail
cd "$(dirname "$0")/../.."
. tools/local_env.sh
SDK=${ANDROID_SDK:?falta el SDK de Android: pon ANDROID_SDK en local.env (ver local.env.ejemplo) o en el entorno}
BT=$SDK/build-tools/36.0.0
AJAR=$(cygpath -w "$SDK/platforms/android-36/android.jar")
JDK=${JDK:?falta el JDK: pon JDK en local.env (ver local.env.ejemplo) o en el entorno}
NDKTC=${NDK:-$SDK/ndk/29.0.14206865}/toolchains/llvm/prebuilt/windows-x86_64
PY=${PY:-build/gen-venv/Scripts/python.exe}
SDLJ=../deps/src/SDL3-3.4.20/android-project/app/src/main/java
w() { cygpath -w "$1"; }

VN=$(sed -n 's/^\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*-[a-z]*\) .*/\1/p' VERSIONES-ANDROID.txt | head -1)
[ -n "$VN" ] || { echo "apk: falta la versión en VERSIONES-ANDROID.txt"; exit 1; }
IFS=.- read -r X Y Z _ <<< "$VN"
VC=$((X * 10000 + Y * 100 + Z))
ABIS=${*:-}
[ -n "$ABIS" ] || for a in x86_64 arm64-v8a; do [ -f "build/android-$a/libmain.so" ] && ABIS="$ABIS $a"; done
[ -n "$ABIS" ] || { echo "apk: ningún build/android-<abi>/libmain.so (make PLATFORM=android ABI=...)"; exit 1; }

O=build/apk
W=$O/trabajo
rm -rf "$W" && mkdir -p "$W/classes" "$W/dex" "$W/res" "$W/assets" "$W/lib"

# 1. el Java: el de SDL3 (su android-project) y la actividad propia. Las listas van en arreglos: cygpath -w da rutas
# absolutas (por el «..») y la carpeta puede tener espacios.
lista_w() { local f; while IFS= read -r -d '' f; do printf '%s\0' "$(w "$f")"; done; }
mapfile -d '' JAVAS < <(find "$SDLJ" tools/android/app/java -name '*.java' -print0 | lista_w)
"$JDK/bin/javac.exe" -source 8 -target 8 -bootclasspath "$AJAR" -nowarn -Xlint:-options -encoding UTF-8 -d "$(w "$W/classes")" \
    "${JAVAS[@]}"
mapfile -d '' CLASES < <(find "$W/classes" -name '*.class' -print0 | lista_w)
"$JDK/bin/java.exe" -cp "$(w "$BT/lib/d8.jar")" com.android.tools.r8.D8 --release --min-api 33 --lib "$AJAR" \
    --output "$(w "$W/dex")" "${CLASES[@]}"

# 2. recursos, manifiesto y assets (lo que va con el programa: runtime/platform/android.c)
cp -R tools/android/app/res/. "$W/res/"
mkdir -p "$W/res/mipmap-xxxhdpi" && cp docs/icon/icon_512.png "$W/res/mipmap-xxxhdpi/ic_launcher.png"
cp third_party/fonts/Baloo2/Baloo2-SemiBold.ttf third_party/fonts/BIZUDPGothic/BIZUDPGothic-Regular.ttf \
   third_party/fonts/DejaVu/DejaVuSans.ttf third_party/fonts/DejaVu/DejaVuSans-Bold.ttf "$W/assets/"
cp docs/icon/icon_512.png "$W/assets/icon.png"
# las licencias de lo que va en el APK (assets/licencias, resumen en LICENCIAS.txt): el port, sus piezas, las fuentes y
# las bibliotecas de Android (de los paquetes de fuentes de tools/android/deps.sh y del NDK)
L="$W/assets/licencias" && mkdir -p "$L"
DS=../deps/src
cp tools/android/LICENCIAS-ANDROID.txt "$L/LICENCIAS.txt"
cp LICENSE "$L/LAS-Recomp-LICENSE.txt"
cp third_party/imgui/LICENSE.txt "$L/DearImGui-LICENSE.txt"
cp third_party/smaa/LICENSE.txt "$L/SMAA-LICENSE.txt"
cp third_party/snapdragon-gsr/LICENSE.txt "$L/Snapdragon-GSR-LICENSE.txt"
cp third_party/ryujinx/LICENSE.txt "$L/Ryujinx-LICENSE.txt"  # (runtime/services/audio.c: el audio portado)
cp third_party/amd-fsr/AMD-FSR-SDK-LICENSE.md "$L/"  # (AMD FSR 1 en GLSL: ffx_fsr1.h, MIT)
VMAL=/ucrt64/share/licenses/vulkan-memory-allocator/LICENSE.txt
[ -f "$VMAL" ] || VMAL=/c/msys64/ucrt64/share/licenses/vulkan-memory-allocator/LICENSE.txt
cp "$VMAL" "$L/VMA-LICENSE.txt"  # (Vulkan Memory Allocator, entero dentro de libmain.so: vma.cpp)
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
# (la generación de fotogramas de Lossless Scaling: liblsfg.so, de tools/lsfg/build.sh; con ella, sus licencias
# siempre: MIT y zlib piden que su aviso vaya con cada copia)
LS=${LSFG_SRC:-../deps/src/lsfg-vk-android}
if ls build/lsfg/android-*/liblsfg.so > /dev/null 2>&1; then
    for lic in LICENSE.md thirdparty/dxbc/LICENSE.md thirdparty/pe-parse/LICENSE thirdparty/volk/LICENSE.md; do
        [ -f "$LS/$lic" ] || { echo "apk: está liblsfg.so pero no $LS/$lic (LSFG_SRC: donde estén sus fuentes)"; exit 1; }
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

# 3. las bibliotecas nativas, sin la información de depuración
for a in $ABIS; do
    mkdir -p "$W/lib/$a"
    for so in build/android-$a/libmain.so ../deps/$a/lib/*.so $(ls build/lsfg/android-$a/liblsfg.so 2>/dev/null); do
        "$NDKTC/bin/llvm-strip.exe" --strip-debug -o "$(w "$W/lib/$a/$(basename "$so")")" "$(w "$so")"
    done
done

# 4. juntar, alinear y firmar
"$PY" -I tools/android/apk_zip.py "$(w "$W/base.apk")" "$(w "$W/dex")" "$(w "$W/lib")" "$(w "$W/sin-firmar.apk")"
"$BT/zipalign.exe" -P 16 -f 4 "$(w "$W/sin-firmar.apk")" "$(w "$W/alineado.apk")"
KS=../claves/depuracion.keystore
if [ ! -f "$KS" ]; then
    mkdir -p ../claves
    "$JDK/bin/keytool.exe" -genkeypair -keystore "$(w "$KS")" -storepass android -keypass android -alias depuracion \
        -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=SMRPG Recomp (depuracion)" > /dev/null
fi
APK=$O/smrpg-recomp-android-$VN.apk
"$JDK/bin/java.exe" -jar "$(w "$BT/lib/apksigner.jar")" sign --ks "$(w "$KS")" --ks-pass pass:android \
    --key-pass pass:android --out "$(w "$APK")" "$(w "$W/alineado.apk")"
"$JDK/bin/java.exe" -jar "$(w "$BT/lib/apksigner.jar")" verify "$(w "$APK")"
ls -la "$APK" | awk '{print "apk: " $9 " (" $5 " bytes), ABI:'"$ABIS"', versión '"$VN"' (" '"$VC"' ")"}'
