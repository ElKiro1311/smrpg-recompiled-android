# SMRPG Recomp para Android

[English](README.md) · **Español**

Port no oficial de **Super Mario RPG** (Nintendo Switch, 2023, versión 1.0.0) a **Android** por recompilación estática:
el programa del juego se traduce a C, se compila para el procesador del móvil o la consola portátil y corre sobre el
runtime de [LASRecomp](https://github.com/peiikai/LASRecomp) (servicios del sistema, kernel y NVN → Vulkan).

El port de Windows está en su propio repositorio:
[**smrpg-recompiled**](https://github.com/ElKiro1311/smrpg-recompiled).

> **Aviso legal.** Proyecto de aficionados, sin ánimo de lucro, **sin relación con Nintendo ni con Square Enix** y sin
> su respaldo. *Super Mario RPG* y *Mario* son marcas de Nintendo; el juego es © Nintendo / SQUARE ENIX. Ni este
> repositorio ni sus releases contienen archivos, código, gráficos, música o textos del juego. Para jugar hacen falta
> los archivos de **tu propia copia** del juego, ya extraídos (las carpetas `exefs` y `romfs` de la versión 1.0.0); no
> se dan instrucciones para obtenerlos. No compartas tu instalación, el código del juego compilado (`lasgame.so`) ni las
> texturas que vuelque el juego: son del juego.

## Estado

Alfa. Va a 60 FPS en una consola con Snapdragon 8 Gen 2 (AYN Thor, Adreno 740): vídeos, título, menús, exploración,
combates, guardar y cargar, en todo lo probado. Pide **Android 13 o más nuevo, un aparato ARM de 64 bits y Vulkan 1.3**;
en otras GPU no está probado.

Qué tiene: resolución interna desde 720p, escalado con Snapdragon GSR 1 y GSR 2 y con AMD FSR 1, generación de
fotogramas de Lossless Scaling (con **tu** copia), texturas HD, botones a tu gusto, pausa en segundo plano y menús en español
(la versión en inglés está en camino). Lo que cambia en cada versión: [`VERSIONES-ANDROID.txt`](VERSIONES-ANDROID.txt).

## Instalar

1. Descarga `smrpg-recomp-android-<versión>.apk` de la
   [última release](https://github.com/ElKiro1311/smrpg-recompiled-android/releases) e instálalo.
2. El APK no lleva nada del juego, así que también hace falta el código del juego compilado para tu aparato
   (`lasgame.so`). Por ahora ese paso necesita un PC con Windows: mira **Compilar** más abajo (con el APK de la release
   bastan los pasos 1 y 2).
3. Copia al aparato tu juego extraído y ese código, de modo que
   `Android/data/org.smrpgrecomp.android/files/extracted/` tenga `exefs/`, `romfs/`, `lasgame.so` y `lasgame.so.id`.

El menú de ajustes se abre con el botón Atrás (o con Home en un mando). La generación de fotogramas usa el
`Lossless.dll` de tu copia de Lossless Scaling (de pago, en Steam), que copias a
`Android/data/org.smrpgrecomp.android/files/`: ni el repositorio ni el APK lo llevan ni lo descargan.

## Compilar

En Windows, con MSYS2 UCRT64. Hace falta el SDK de Android con `ndk;29.0.14206865`, `build-tools;36.0.0`,
`platforms;android-36` y `cmake;4.1.2`, y un JDK 17 o más nuevo, con sus rutas en un `local.env`, que git no sube (ver
[`local.env.ejemplo`](local.env.ejemplo)). Con tu juego extraído en `extracted/`:

1. `tools/android/deps.sh arm64-v8a`: SDL3, FFmpeg (LGPL), Opus y shaderc, en `../deps/arm64-v8a`;
2. `make gen PLATFORM=android ABI=arm64-v8a` y `make -j8 PLATFORM=android ABI=arm64-v8a`: `libmain.so` y el código del
   juego, `lasgame.so`, en `build/android-arm64-v8a/`;
3. `tools/android/apk.sh arm64-v8a`: el APK, en `build/apk/`; se instala con `adb install`.

## Fallos y sugerencias

En las [issues de este repositorio](https://github.com/ElKiro1311/smrpg-recompiled-android/issues), con
`lasrecomp.log` de la carpeta de datos del juego. No en las de LASRecomp: este port no es cosa suya.

## Créditos y licencias

- [LASRecomp](https://github.com/peiikai/LASRecomp), de Peiikai y colaboradores (MIT): el recompilador, el runtime y el
  renderizador. Su README original está en [`docs/README-LASRecomp.md`](docs/README-LASRecomp.md).
- [nxnative](https://github.com/seedds/nxnative): referencia de cómo se comporta este juego.
- [Snapdragon GSR](https://github.com/SnapdragonStudios/snapdragon-gsr) de Qualcomm, AMD FSR 1,
  [lsfg-vk-android](https://github.com/GameNative/lsfg-vk-android) (la generación de Lossless Scaling), Vulkan Memory
  Allocator, Dear ImGui, SMAA, SDL3, FFmpeg (LGPL), Opus, shaderc y otros, cada uno con su licencia, en `third_party/` y
  dentro del APK (`assets/licencias`).
- El código de LASRecomp y el de este port van con la licencia MIT de [`LICENSE`](LICENSE). Esa licencia no cubre
  Super Mario RPG ni nada de Nintendo, ni lo de `third_party/`, que conserva su propia licencia.
