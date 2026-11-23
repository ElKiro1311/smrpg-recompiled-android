# Estado del port (se actualiza en cada versión)

Lo que cambia en cada versión está en [`VERSIONES.txt`](VERSIONES.txt). Este archivo dice dónde está el arranque ahora,
cómo se prueba y qué se sabe del juego.

## Dónde está el arranque

- Pasa nninitStartup (heap con memoria virtual), los 112 constructores del sdk y los 288 del juego, y entra en nnMain.
- Monta el RomFS, inicializa NVN y Vulkan (dispositivo, ventana de 3 texturas, vsync), crea unos 30 hilos de Unity y
  sube las primeras texturas.
- Con la versión 11 (orden de memoria de `LDAR`/`STLR`) el hilo principal sigue adelante, crea `Loading.PreloadManager`
  y empieza a cargar la primera escena, «level0».
- Versiones 13-16: pasa «level0» (despertar perdido en las condvar), crea los shaders (propiedades NVN de la consola),
  presenta fotogramas (sin los ganchos de Link's Awakening) y deja de caerse (el sdk ya no se carga dentro de main).
- 0.18.0-alfa: corre a 60 fps y dibuja la escena 3D (traductor de shaders: EXIT con condición, TEX.B bindless…).
- **Estado actual (versión 19):** se ven los logos, el vídeo de apertura (VP9 de CRIWARE por `movie::Decoder` con
  FFmpeg), la pantalla de título, el menú de dificultad, la cinemática de partida nueva y la escena del castillo de
  Bowser. El audio de los vídeos se decodifica (Opus por el servicio `hwopus` con libopus).
- Ruta de prueba sin mando: `LARECOMP_INPUT="1300:A:8,1600:A:8,1900:A:8,2200:A:8"` (A salta el vídeo, A en el
  título, en el menú y en la dificultad) y `LARECOMP_SHOT_AT="1450,1750,2600,4800"` → `build/frames/frame_N.png`
  (la imagen del juego, no una captura del escritorio). `LARECOMP_AUDOUT_WAV=build/audio.wav` vuelca el audio.
- 0.20.0-alfa: los personajes se ven (Mario, Bowser y Peach en la cinemática y Mario en el puente del castillo,
  fotograma ~5100 de la ruta); todos los shaders se traducen (0 dibujos saltados).
- **Versiones 21-23: se juega.** Tras la cinemática, Mario entra en el castillo con el HUD; el juego se autoguarda
  (`save/save_0/SMR_Save_Ver0011000`), la pantalla «Load» carga la partida (con la hora local desde la 22), Mario se
  mueve, la cámara le sigue y la primera batalla (dos Terrapins) funciona: ataque, daño, efectos y vuelta al mapa.
- Para jugar: `jugar.cmd` (con sonido, mando y teclado; los controles van en el propio archivo). Para probar sin
  cerrar la partida de nadie: `tools/probar.sh <segundos> <registro> VARIABLE=valor…`; con
  `LARECOMP_DATA_ROOT=<carpeta>` (con `extracted` y `build` enlazados) la prueba usa sus propios guardados y ajustes, y
  `LARECOMP_HIDDEN=1` no muestra la ventana.
- **0.24.0-beta:** la aplicación es Super Mario RPG Recomp (`build/smrpg-recomp.exe`, icono propio). Menú (Esc / F1) en
  español con: pantalla ancha de verdad (Unity dibuja al aspecto del monitor: `runtime/hle/unity_screen.c` +
  `tools/game_overrides.c`), resolución interna con nombre (720p-4320p), supersampling x2-x16, nitidez (RCAS),
  anisótropo, límite de FPS (hasta 240; a 120 el movimiento no se acelera) y HDR10.
- Sustituir una función del juego sin regenerar: definirla en `tools/game_overrides.c` (se enlaza la primera con
  `--allow-multiple-definition`, así gana también para las llamadas internas de su archivo generado).
- **Versiones 25-31:** idioma del juego (25), detalle de texturas (26), miniaturas de los guardados (27), menú en
  español (28), interfaz entera en pantalla ancha (29: `CanvasScaler.HandleScaleWithScreenSize` sustituido), barrido
  de shaders (`tools/shader_sweep.c`: 82.006 de los 82.032 del juego se traducen y compilan, 30) y texturas HD con
  volcado (31: `<datos>/texturas` y `<datos>/texturas-volcadas`, `runtime/nvn/vulkan/texpack.c`).
- **Versiones 32-35: AMD FSR.** Antialiasing «AMD FSR» (nativo) y escalado (Calidad x1,5, Equilibrado x1,7,
  Rendimiento x2, Ultra x3), con FSR 4.1.2, 3.1.6 o 2.3.4 a elegir. Los vectores de movimiento son los de URP 12
  (su MotionVectorRenderPass, forzada en `UniversalRenderer.GetRenderPassInputs`, `tools/game_overrides.c`); el
  desplazamiento subpíxel va en el viewport y FSR corre antes del posproceso (`runtime/nvn/vulkan/exec.c`
  fsr_pass_begin, `vk_fsr.cpp`). Hacen falta junto a `build/smrpg-recomp.exe` las DLL del SDK de AMD FSR 2.4
  (`amd_fidelityfx_loader_vk.dll`, `amd_fidelityfx_upscaler_vk.dll`; no están en el repositorio:
  `third_party/amd-fsr/LEEME.txt`).
- **0.38.0-beta: generación de fotogramas de AMD** (FG 4.0.2 por red neuronal en RDNA 4, FG 3.1.6; `vk_fg.cpp`): la
  swapchain de AMD con dos colas más; configuración y preparación tras FSR en el hilo grabador; la cámara sale de la
  vista-proyección de URP (`exec.c` fsr_camera_scan). La Y de los vectores va al revés que para FSR (medido: con la de
  FSR, FG 4 solo funde). Sin HDR, la ventana sin el menú del runtime (HUDLessColor) evita que el contador de FPS se
  deforme. Pruebas: `LARECOMP_FG_PSHOT=N` (lo presentado, a `build/fg_pres_*.png`), `LARECOMP_FG_SET`, `LARECOMP_FG_MV`,
  `LARECOMP_FG_DEBUG=2` (comprobador de AMD), `LARECOMP_FG_CAMDUMP=N`. Necesita `amd_fidelityfx_framegeneration_vk.dll`.
- **0.39.0-beta:** los vídeos (título, películas) llenan su textura escalada a más de 1x (antes, encogidos en la esquina).
  Pruebas: `LARECOMP_SET` cambia ajustes de vídeo en marcha; capturas de ventana con nombre por `LARECOMP_HOST_INPUT`.
- **0.40.0-beta:** modo de pantalla (ventana, sin bordes, pantalla completa exclusiva) y resolución de pantalla
  (`runtime/ui/menu.mm` menu_apply_window_mode_now).
- **0.41.0-beta:** el aspecto del juego (21:9, 16:10) sigue a la ventana también en marcha, con vista ancha real: aviso 31
  de AM al cambiar (`runtime/hle/unity_screen.c` rt_unity_screen_follow), SetResolution de Unity (main 0x700062f1d0)
  sustituida sin su recorte a 1920x1080 ni la marca de resolución propia (`tools/game_overrides.c`), texturas de ventana
  rehechas a la nueva medida (`t->screen`, `runtime/nvn/vulkan/textures.c`). Guía de controles en Menú > Controles y
  «Configurar controles» en español (`runtime/ui/controls_ui.mm`).
- **0.43.0-beta:** el fondo de los menús (la captura de pantalla de Menu.StartScreenCapture en una RawImage de
  1920x1080) ocupa la pantalla ancha: RawImage.set_texture sustituida, que pasa esas imágenes al aspecto de su
  textura (`tools/game_overrides.c`).
- **0.44.0-beta:** Menú > Vídeo: «Rendimiento ahora» (CPU, GPU, RAM, disco, memoria de vídeo medidos:
  `runtime/perf_meter.c`), el impacto de cada ajuste gráfico en los cinco con colores de semáforo y
  los botones Guardar, Deshacer cambios, Restablecer vídeo y Reiniciar el juego (`runtime/ui/menu.mm`).
- **0.45.0-beta:** en Vídeo, el recuadro de la derecha lleva el medidor arriba, el impacto del ajuste elegido
  encima de los botones y los botones abajo, siempre a la vista (`runtime/ui/option_rows.mm` opt_pane_*).
- Zonas comprobadas sin jugar (11-16, `tools/recorrer_zonas.py`: el guardado es JSON en UTF-16 y su
  `_respawn_floor_name` decide la escena al cargar): unas 36 zonas de todo el mapa se dibujan bien y sin dibujos
  saltados (Mushroom Way, casa de Mario, Reino Champiñón, camino de los bandidos, cloacas de Kero, Rose Town y Rose
  Way, bosque laberinto, Moleville, Booster Pass y torre, Marrymore, Star Hill, barco hundido, Seaside Town, Land's End,
  volcán Barrel por fuera y por dentro, mapa del mundo, habichuela y Nimbus Land, castillo de Bowser); en otras Mario
  aparece fuera del mapa (la posición centinela del guardado) y no son concluyentes.
- **0.46.0-beta:** «Negro puro» (la LUT de etalonaje de URP con su negro a 0: `vulkan/exec.c` lut_black_fix), «Vídeos
  en pantalla ancha» (las películas llenan el 21:9: `runtime/hle/movie_fit.c`) y combates siempre a 60 FPS
  (`runtime/hle/battle_rate.c`).
- **0.46.1-beta:** AMD FSR trabaja a la resolución interna: a 2160p en 21:9 (5160x2160) FSR 4 cuesta 3,1 ms por imagen
  en una RX 9070 XT (1,4 ms a 3440x1440; FSR 3.1 2,25 ms, FSR 2.3 1,7 ms; SMAA, nada medible): 240 -> ~175 FPS. El
  recuadro de impacto del menú lo explica; el perfilador de GPU (`LARECOMP_GPUPROF_AT`) da a FSR su propia fila.
- **0.47.0-beta:** instalador (`tools/instalador`, `make instalador` → `build/dist/SMRPG-Recomp-Instalador-<v>.exe`):
  la copia del juego + carpeta → copia el programa (sin nada del juego), pone la copia en extracted/, construye el
  código del juego en ese PC
  (`smrpg-recomp.exe --build-game`: lasgen + zig adelgazado, `runtime/gamebuild.c` con `game_overrides.c` primero y
  `-FORCE:MULTIPLE`) y crea los accesos directos. Sin --play también va directo al juego.
- **0.48.0-beta:** el instalador construye el código del juego DENTRO de su propio proceso, sin procesos hijos:
  `construir\construir.dll` (`tools/instalador/construir.cpp`) lleva el generador en un Python 3.12 embebido (aislado
  con `python312._pth`), clang 23 y lld de MSYS2 como bibliotecas (la misma línea `-cc1` y de enlace que `make`) y lo
  que incluyen y enlazan (`tools/instalador/herramientas.sh`); del PC solo hace falta Windows. C generado idéntico a
  `make gen` y objetos idénticos a los de `make` (salvo la hora COFF). Hilos con 64 MB de pila (con 2 MB,
  `MemorySSAUpdater::recursePhi` la desbordaba), red de fallos propia en los hilos de compilación (el
  `CrashRecoveryContext` de LLVM se apagaba con los fallos que unicorn provoca y recoge), lld con `--threads=1` (su
  grupo de hilos colgaba la salida del proceso). Cualquier CPU x86-64: v3 si hay AVX2, si no v2 o x86-64
  (`lasgame.dll.cpu`, comprobado por `runtime/gamelib.c` al cargar). Prueba suelta: `tools/instalador/probar_construir.c`.
  Con Control Flow Guard impuesto por Windows (#337 de LAS Recomp) unicorn tumba el proceso: el instalador se reabre a
  sí mismo con CFG apagado por la directiva de creación del proceso (probarlo: `tools/instalador/con_cfg.c` arranca un
  programa con CFG forzado). Probar la red: `SMRPG_CONSTRUIR_PRUEBA_FALLO=<trozo>:1` (acceso a 0) o `:2` (pila).
- Más de 60 FPS: en la partida el juego cuenta por tiempo (el movimiento va igual a 240 que a 60). Los combates no: su
  lógica (BattleManager.update_common) da un paso de 1/60 s por imagen y a 240 FPS iba cuatro veces más deprisa que
  sus animaciones y su sonido; desde la 0.46.0 el vsync late a 60 Hz mientras dura un combate. Las entradas
  programadas (`LARECOMP_INPUT`) van por fotogramas: a 240 FPS hay que multiplicar sus números por 4 (en combate, no).
- Nombres de IL2CPP (clases, métodos y sus direcciones en main): Il2CppDumper 6.7.46 sobre `extracted/exefs/main` y
  `romfs/Data/Managed/Metadata/global-metadata.dat` (con `DOTNET_ROLL_FORWARD=Major` en .NET 8).
- Pendiente:
  - Mando real y audio por los altavoces, sin probar por mí (las pruebas van con `LARECOMP_NO_AUDIO=1`; el audio
    se comprueba con `LARECOMP_AUDOUT_WAV`).
  - Pantalla ancha: el juego pone su interfaz en un panel fijo de 1920x1080; el fondo del título, el oscurecido de
    los diálogos y el fundido a blanco del título a la apertura no llegan a los lados (los fundidos a negro sí se ven
    bien desde la 0.46.0: los lados ya son negros de verdad).
  - 22 shaders sin traducir, ninguno que el juego use (VR, wireframe, LookDev, CopyDepth con MSAA, 7 internos de main).
  - Generación de fotogramas: la interfaz del propio juego (batallas, el panel del campo) se interpola con la escena (sin
    imagen «sin interfaz» del juego: haría falta la imagen tras el posproceso y antes de la interfaz); con HDR, también
    el contador de FPS. Sin probar con la ventana visible ni a pantalla completa (las pruebas van ocultas).
  - Más allá de la primera batalla, sin recorrer: Mushroom Way usa RGBA4 (versión 23, sin comprobar en el juego).

### Cómo dibuja Unity en NVN (lo que hizo falta en el traductor)

- Texturas y samplers separados: el shader carga la palabra de la textura de `c[2][0x168 + 8r]` y la del sampler de
  `c[2][0x568 + 8r]`, las une con OR y muestrea con `TEX.B` / `TLD.B` (bindless). El traductor rastrea esas palabras
  (`tex_b_words`) y el renderizador compone el manejador (`shader_tex_handle`).
- Los shaders de vértices descartan pronto con `VOTE.VTG` + `EXIT CC.FCSM_TR`: ese EXIT no debe ejecutarse.

### Cosas del runtime de Link's Awakening que no valen para SMRPG

- La tabla de direcciones `v1_0_0` (`runtime/game_addresses/*_v1_0_0.h`) es la de Link's Awakening 1.0.0: en SMRPG
  esas direcciones no apuntan a nada suyo. `g_game_las` (false) apaga lo que las lee en cada fotograma; si aparece
  otro fallo que pasa por `GA(...)`, va por el mismo camino.
- El generador ya no aplica los parches ni ganchos de LA a SMRPG (`tools/game_addresses.py GAME = 'smrpg'`). Las
  variantes del aleatorizador (`tools/rando/patch_variants.json`) sí se generan, pero sin semilla ejecutan la
  instrucción original.
- Las bases de los módulos: main en 0x7000000000 (hasta 0x7004dbc000 con su .bss) y sdk en 0x7005000000.

### Leer la memoria del juego colgado con gdb

Las direcciones del invitado son direcciones del proceso, así que gdb las lee tal cual (en Windows `long` es de 32 bits:
usar `long long`):

```sh
PID=$(tasklist //FI "IMAGENAME eq smrpg-recomp.exe" //FO CSV //NH | head -1 | cut -d, -f2 | tr -d '"')
gdb -p $PID -batch -ex 'x/8gx 0x750f804b80+0x390' -ex detach -ex quit
```

## Cómo probar un arranque

Desde MSYS2 (UCRT64), en la carpeta del repositorio, con el volcado del juego enlazado en `extracted/`:

```sh
(export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"; make -j16)
# (si cambian tools/lifter.py, tools/gen.py o runtime/cpu.h: antes «make gen», y se recompila todo el juego)
export PATH="/c/msys64/ucrt64/bin:$PATH"   # ¡sin /usr/bin de MSYS2 delante! (ver abajo)
LARECOMP_HANG_DUMP=20 LARECOMP_NO_AUDIO=1 LARECOMP_NO_MOVIE_AUDIO=1 LARECOMP_FORCE_LAUNCHER=0 \
  timeout 90 ./build/smrpg-recomp.exe --play extracted > build/boot_X.log 2>&1
```

- Trampa: con `/c/msys64/usr/bin` delante en el PATH, el `timeout` de MSYS2 (mezclado con el bash de Git) no pasa las
  variables de entorno al juego: ninguna `LARECOMP_*` llega. Para compilar hace falta ese PATH (en un subshell); para
  lanzar, solo `ucrt64/bin`.
- `LARECOMP_FNPROF=0:999999`: cada volcado de cuelgue escribe `build/fnprof_cuelgueN.txt` (dirección y veces que
  corrió cada función del juego). Con `LARECOMP_FNPROF_BT=<dir>,...` se ve quién llama primero a cada una y con qué x0.
- `LARECOMP_WATCH_ADDR=<lo>,<hi>`: qué funciones del juego tocan esa memoria (desde el arranque).
- `LARECOMP_CRASH_HOLD=<s>`: tras un fallo el proceso espera esos segundos antes de salir (para `gdb -p`).

- `init sdk` / `init main`: los constructores. Un error fatal termina con la pila del juego («guest stack scan») y las
  últimas 32 llamadas al host del hilo que falló.
- `LARECOMP_HANG_DUMP=<s>`: si el hilo principal pasa tanto tiempo en una espera, se vuelca cada hilo con su nombre de
  Unity, lo que espera, su pila y la traza del kernel (los últimos 2.048 eventos de mutex y condvar).
- Las direcciones 0x7000000000+ son de `main` y las 0x7005000000+ del `sdk` (hasta la versión 15, 0x7004000000+).

## Analizar el código del juego

- `nxnative/dm/tools/nso2elf.py` convierte `exefs/main`, `exefs/sdk` y `exefs/subsdk0` en ELF para Ghidra (no se suben:
  son código del juego). En Ghidra hay que poner la base de imagen en 0x7000000000 (main) o 0x7005000000 (sdk).
- Los stubs de la PLT llaman a través de la GOT: para saber a qué función llaman hay que resolver la reubicación
  JUMP_SLOT de su entrada de la GOT (así se renombraron los 8.821 stubs del sdk como `plt_<símbolo>`).
- Los punteros a funciones NVN de Unity están en variables de `main` (0x3da5xxx) que su cargador rellena con
  nvnDeviceGetProcAddress; se reconocen por el uso.
- El `sdk` y el `subsdk0` de la 1.0.0 son idénticos a los de la 1.0.1 (mismo build ID), la que juega nxnative: lo que
  nxnative sabe del SDK de este juego sirve tal cual, y su `docs/reference/` (threads, memory, rendering) recoge muchos
  problemas de Unity en Switch ya resueltos.

## Lo que se sabe del juego

- `nn::init::Start` (sdk 0xfa3c0): nnosInitialize → (llamada temprana) → nninitStartup → (llamada tardía: los
  constructores de sdk y main) → nnMain.
- El NPDM declara 16 MB de «system resource»: la memoria virtual de nn::os (VammManager) está activada. El
  `nninitStartup` del juego (main 0x62de90) reserva (memoria total − usada) ≈ 3,1 GB de espacio virtual para su
  `StandardAllocator` (objeto en main 0x3d822d0) y no usa SetHeapSize. El `AddressRegionManager` trabaja sobre la región
  alias: se queda el primer GB y alinea a 1 GB lo grande. Por eso la región alias es de 48 GB.
- MapPhysicalMemory solo mapea (y pone a cero) las páginas que faltan, y solo en la región alias.
- `svcBreak` con motivo 0 es «Panic» (`nn::diag::detail::AbortImpl`): antes, el juego llama a am SetTerminateResult.
- Unity (`main`) define operator new/delete globales que sustituyen a los débiles del sdk (interposición, como el rtld).
- nn::ro: Unity solo llama a Initialize y a LookupSymbol (no hay NRO); el rtld no existe aquí.
- El GC de IL2CPP para los hilos con SuspendThread/GetThreadContext: pausa cooperativa en puntos seguros.
- NVN 55: la `NVNtextureView` del juego ocupa 16 bytes; la `NVNtexture`, 0x80. Unity pide 277 funciones NVN; faltan
  19 en el runtime (Barrier, SetAlphaRef, SetPointSize, CopyBufferToBuffer, binds «Deferred»…) y varias son vacías
  (BindSeparateSampler(s), ReportValue, DrawTexture…): habrá que hacerlas cuando Unity dibuje.
- Orden de memoria: en AArch64 `STLR` + `LDAR` son seq_cst; en x86 hay que traducir `STLR` con barrera completa.
- Hilos de Unity: MainThread, UnityGfxDeviceWorker, Loading.AsyncRead, Job.Worker 0-1, Background Job.Worker 0-15,
  GC Finalizer, AssetGarbageCollectorHelper, BatchDeleteObjects, NetworkInterfaceWrapper, LibcurlGc, un hilo de vsync
  sin nombre (main 0x6b536c) y NxSystemThread (del SDK).
- `main` importa 31 funciones de `subsdk0` (nn::movie). El runtime no carga `subsdk0`: `runtime/hle/movie.c` las
  implementa con FFmpeg (versión 19: las que usa CRIWARE Sofdec). Sin implementar aún, porque no se han visto
  llamadas: `GetH264NalUnitStartPositionAndNalSize` y las del `Extractor` llamadas por símbolo.
- Los vídeos son USM de CRIWARE con VP9 1920x1080 a 60 imágenes/s y audio Opus (un flujo estéreo, por `hwopus`).
  CRI crea el decodificador con tipo 3 (VP9), formato 3 y modo 2 (textura nativa).
- CRI pide 196 veces el tamaño del búfer de un decodificador Opus multistream de 2 flujos mono (orden 7 de
  `hwopus`) sin abrirlo: es su cálculo de memoria, no un fallo.
