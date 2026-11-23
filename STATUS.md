# Port status (updated with every version)

What changes in each version is in [`CHANGELOG.txt`](CHANGELOG.txt). This file says where the boot stands now, how it is
tested and what is known about the game.

## Where the boot stands

- It gets through nninitStartup (heap with virtual memory), the sdk's 112 constructors and the game's 288, and enters
  nnMain.
- It mounts the RomFS, initializes NVN and Vulkan (device, 3-texture window, vsync), creates about 30 Unity threads and
  uploads the first textures.
- With version 11 (memory ordering of `LDAR`/`STLR`) the main thread carries on, creates `Loading.PreloadManager` and
  starts loading the first scene, "level0".
- Versions 13-16: it gets past "level0" (lost wakeup in the condvars), creates the shaders (the console's NVN
  properties), presents frames (without the Link's Awakening hooks) and stops crashing (the sdk is no longer loaded
  inside main).
- 0.18.0-alfa: it runs at 60 fps and draws the 3D scene (shader translator: conditional EXIT, bindless TEX.B…).
- **Current state (version 19):** you see the logos, the opening movie (CRIWARE's VP9 through `movie::Decoder` with
  FFmpeg), the title screen, the difficulty menu, the new game cutscene and the Bowser's Keep scene. The audio of the
  movies is decoded (Opus through the `hwopus` service with libopus).
- Test route without a controller: `LARECOMP_INPUT="1300:A:8,1600:A:8,1900:A:8,2200:A:8"` (A skips the movie, A on the
  title, in the menu and on the difficulty) and `LARECOMP_SHOT_AT="1450,1750,2600,4800"` → `build/frames/frame_N.png`
  (the game's image, not a desktop screenshot). `LARECOMP_AUDOUT_WAV=build/audio.wav` dumps the audio.
- 0.20.0-alfa: the characters are visible (Mario, Bowser and Peach in the cutscene and Mario on the castle bridge, frame
  ~5100 of the route); all shaders are translated (0 draws skipped).
- **Versions 21-23: it is playable.** After the cutscene, Mario enters the castle with the HUD; the game autosaves
  (`save/save_0/SMR_Save_Ver0011000`), the "Load" screen loads the save (with the local time since 22), Mario moves, the
  camera follows him and the first battle (two Terrapins) works: attack, damage, effects and back to the map.
- To play: `play.cmd` (with sound, controller and keyboard; the controls are in the file itself). To test without
  closing anyone's game: `tools/run_test.sh <seconds> <log> VARIABLE=value…`; with `LARECOMP_DATA_ROOT=<folder>` (with
  `extracted` and `build` linked) the test uses its own saves and settings, and `LARECOMP_HIDDEN=1` does not show the
  window.
- **0.24.0-beta:** the application is Super Mario RPG Recomp (`build/smrpg-recomp.exe`, its own icon). Menu (Esc / F1)
  in Spanish with: true widescreen (Unity draws at the monitor's aspect ratio: `runtime/hle/unity_screen.c` +
  `tools/game_overrides.c`), named internal resolution (720p-4320p), x2-x16 supersampling, sharpness (RCAS), anisotropic
  filtering, FPS limit (up to 240; at 120 movement does not speed up) and HDR10.
- Overriding a game function without regenerating: define it in `tools/game_overrides.c` (it is linked first with
  `--allow-multiple-definition`, so it also wins for the internal calls of its generated file).
- **Versions 25-31:** game language (25), texture detail (26), save thumbnails (27), menu in Spanish (28), the whole UI
  in widescreen (29: `CanvasScaler.HandleScaleWithScreenSize` overridden), shader sweep (`tools/shader_sweep.c`: 82,006
  of the game's 82,032 are translated and compile, 30) and HD textures with dumping (31: `<data>/textures` and
  `<data>/textures-dump`, `runtime/nvn/vulkan/texpack.c`).
- **Versions 32-35: AMD FSR.** "AMD FSR" anti-aliasing (native) and upscaling (Quality x1.5, Balanced x1.7, Performance
  x2, Ultra performance x3), with a choice of FSR 4.1.2, 3.1.6 or 2.3.4. The motion vectors are those of URP 12 (its
  MotionVectorRenderPass, forced in `UniversalRenderer.GetRenderPassInputs`, `tools/game_overrides.c`); the subpixel
  jitter goes in the viewport and FSR runs before post-processing (`runtime/nvn/vulkan/exec.c` fsr_pass_begin,
  `vk_fsr.cpp`). The AMD FSR SDK 2.4 DLLs are needed next to `build/smrpg-recomp.exe` (`amd_fidelityfx_loader_vk.dll`,
  `amd_fidelityfx_upscaler_vk.dll`; they are not in the repository: `third_party/amd-fsr/README.txt`).
- **0.38.0-beta: AMD frame generation** (neural-network FG 4.0.2 on RDNA 4, FG 3.1.6; `vk_fg.cpp`): AMD's swapchain with
  two more queues; configuration and preparation after FSR on the recorder thread; the camera comes from URP's
  view-projection (`exec.c` fsr_camera_scan). The Y of the vectors is flipped relative to FSR's (measured: with FSR's,
  FG 4 only blends). Without HDR, the window without the runtime's menu (HUDLessColor) keeps the FPS counter from
  warping. Tests: `LARECOMP_FG_PSHOT=N` (what is presented, to `build/fg_pres_*.png`), `LARECOMP_FG_SET`,
  `LARECOMP_FG_MV`, `LARECOMP_FG_DEBUG=2` (AMD's checker), `LARECOMP_FG_CAMDUMP=N`. It needs
  `amd_fidelityfx_framegeneration_vk.dll`.
- **0.39.0-beta:** the videos (title, movies) fill their texture when scaled above 1x (before, shrunk into the corner).
  Tests: `LARECOMP_SET` changes video settings on the fly; named window captures through `LARECOMP_HOST_INPUT`.
- **0.40.0-beta:** display mode (windowed, borderless, exclusive fullscreen) and display resolution
  (`runtime/ui/menu.mm` menu_apply_window_mode_now).
- **0.41.0-beta:** the game's aspect ratio (21:9, 16:10) follows the window on the fly too, with a truly wide view: AM
  message 31 on change (`runtime/hle/unity_screen.c` rt_unity_screen_follow), Unity's SetResolution (main 0x700062f1d0)
  overridden without its clamp to 1920x1080 or the custom-resolution flag (`tools/game_overrides.c`), window textures
  recreated at the new size (`t->screen`, `runtime/nvn/vulkan/textures.c`). Controls guide in Menu > Controls and
  "Configure controls" in Spanish (`runtime/ui/controls_ui.mm`).
- **0.43.0-beta:** the background of the menus (the screen capture from Menu.StartScreenCapture in a 1920x1080 RawImage)
  fills the widescreen: RawImage.set_texture overridden, which switches those images to the aspect ratio of their
  texture (`tools/game_overrides.c`).
- **0.44.0-beta:** Menu > Video: "Performance now" (measured CPU, GPU, RAM, disk, video memory: `runtime/perf_meter.c`),
  the impact of each graphics setting on all five in traffic-light colors and the buttons Save, Undo changes, Reset
  video and Restart the game (`runtime/ui/menu.mm`).
- **0.45.0-beta:** in Video, the box on the right has the meter at the top, the impact of the selected setting above the
  buttons and the buttons at the bottom, always in view (`runtime/ui/option_rows.mm` opt_pane_*).
- Areas checked without playing (11-16, `tools/zone_tour.py`: the save is UTF-16 JSON and its `_respawn_floor_name`
  decides the scene on load): about 36 areas from all over the map draw correctly and with no skipped draws (Mushroom
  Way, Mario's house, Mushroom Kingdom, Bandit's Way, Kero Sewers, Rose Town and Rose Way, Forest Maze, Moleville,
  Booster Pass and Tower, Marrymore, Star Hill, Sunken Ship, Seaside Town, Land's End, Barrel Volcano outside and
  inside, world map, beanstalk and Nimbus Land, Bowser's Keep); in others Mario appears off the map (the save's sentinel
  position) and they are inconclusive.
- **0.46.0-beta:** "True black" (URP's color grading LUT with its black at 0: `vulkan/exec.c` lut_black_fix),
  "Widescreen movies" (the movies fill the 21:9: `runtime/hle/movie_fit.c`) and battles always at 60 FPS
  (`runtime/hle/battle_rate.c`).
- **0.46.1-beta:** AMD FSR works at the internal resolution: at 2160p in 21:9 (5160x2160) FSR 4 costs 3.1 ms per frame
  on an RX 9070 XT (1.4 ms at 3440x1440; FSR 3.1 2.25 ms, FSR 2.3 1.7 ms; SMAA, nothing measurable): 240 -> ~175 FPS.
  The menu's impact box explains it; the GPU profiler (`LARECOMP_GPUPROF_AT`) gives FSR its own row.
- **0.47.0-beta:** installer (`tools/installer`, `make installer` → `build/dist/SMRPG-Recomp-Installer-<v>.exe`): the
  copy of the game + folder → copies the program (with nothing from the game), puts the copy in extracted/, builds the
  game code on that PC (`smrpg-recomp.exe --build-game`: lasgen + slimmed-down zig, `runtime/gamebuild.c` with
  `game_overrides.c` first and `-FORCE:MULTIPLE`) and creates the shortcuts. Without --play it also goes straight to the
  game.
- **0.48.0-beta:** the installer builds the game code INSIDE its own process, with no child processes:
  `builder\builder.dll` (`tools/installer/game_builder.cpp`) carries the generator in an embedded Python 3.12 (isolated
  with `python312._pth`), clang 23 and lld from MSYS2 as libraries (the same `-cc1` and link lines as `make`) and what
  they include and link (`tools/installer/toolchain.sh`); all the PC needs is Windows. Generated C identical to that of
  `make gen` and objects identical to those of `make` (except for the COFF timestamp). Threads with a 64 MB stack (with
  2 MB, `MemorySSAUpdater::recursePhi` overflowed it), a fault net of our own on the compilation threads (LLVM's
  `CrashRecoveryContext` was turned off by the faults that unicorn triggers and catches), lld with `--threads=1` (its
  thread pool hung the process on exit). Any x86-64 CPU: v3 if there is AVX2, otherwise v2 or x86-64 (`lasgame.dll.cpu`,
  checked by `runtime/gamelib.c` on load). Standalone test: `tools/installer/test_builder.c`. With Control Flow Guard
  imposed by Windows (LAS Recomp #337) unicorn brings the process down: the installer reopens itself with CFG turned off
  by the process creation policy (to test it: `tools/installer/with_cfg.c` starts a program with CFG forced). To test
  the net: `SMRPG_BUILD_TEST_FAIL=<chunk>:1` (access to 0) or `:2` (stack).
- Above 60 FPS: in gameplay the game is time-based (movement runs the same at 240 as at 60). Battles are not: their
  logic (BattleManager.update_common) takes one 1/60 s step per frame and at 240 FPS it ran four times faster than their
  animations and sound; since 0.46.0 the vsync ticks at 60 Hz while a battle lasts. Scripted input (`LARECOMP_INPUT`)
  goes by frames: at 240 FPS its numbers have to be multiplied by 4 (not in battle).
- IL2CPP names (classes, methods and their addresses in main): Il2CppDumper 6.7.46 on `extracted/exefs/main` and
  `romfs/Data/Managed/Metadata/global-metadata.dat` (with `DOTNET_ROLL_FORWARD=Major` on .NET 8).
- Pending:
  - A real controller and audio through the speakers, not tested by me (the tests run with `LARECOMP_NO_AUDIO=1`; audio
    is checked with `LARECOMP_AUDOUT_WAV`).
  - Widescreen: the game puts its UI in a fixed 1920x1080 panel; the title background, the dimming behind dialogs and
    the fade to white from the title to the opening do not reach the sides (fades to black do look right since 0.46.0:
    the sides are now truly black).
  - 22 shaders not translated, none that the game uses (VR, wireframe, LookDev, CopyDepth with MSAA, 7 internal ones in
    main).
  - Frame generation: the game's own UI (battles, the field panel) is interpolated with the scene (there is no
    "HUD-less" image from the game: it would take the image after post-processing and before the UI); with HDR, the FPS
    counter too. Not tested with the window visible or in fullscreen (the tests run hidden).
  - Beyond the first battle, not played through: Mushroom Way uses RGBA4 (version 23, not checked in the game).

### How Unity draws on NVN (what the translator needed)

- Separate textures and samplers: the shader loads the texture's word from `c[2][0x168 + 8r]` and the sampler's from
  `c[2][0x568 + 8r]`, ORs them together and samples with `TEX.B` / `TLD.B` (bindless). The translator traces those words
  (`tex_b_words`) and the renderer composes the handle (`shader_tex_handle`).
- Vertex shaders discard early with `VOTE.VTG` + `EXIT CC.FCSM_TR`: that EXIT must not be executed.

### Things in the Link's Awakening runtime that do not apply to SMRPG

- The `v1_0_0` address table (`runtime/game_addresses/*_v1_0_0.h`) is that of Link's Awakening 1.0.0: in SMRPG those
  addresses do not point to anything of its own. `g_game_las` (false) turns off whatever reads them every frame; if
  another bug that goes through `GA(...)` shows up, it is along the same lines.
- The generator no longer applies LA's patches or hooks to SMRPG (`tools/game_addresses.py GAME = 'smrpg'`). The
  randomizer variants (`tools/rando/patch_variants.json`) are generated, but without a seed they execute the original
  instruction.
- Module bases: main at 0x7000000000 (up to 0x7004dbc000 with its .bss) and sdk at 0x7005000000.

### Reading the hung game's memory with gdb

Guest addresses are process addresses, so gdb reads them as is (on Windows `long` is 32-bit: use `long long`):

```sh
PID=$(tasklist //FI "IMAGENAME eq smrpg-recomp.exe" //FO CSV //NH | head -1 | cut -d, -f2 | tr -d '"')
gdb -p $PID -batch -ex 'x/8gx 0x750f804b80+0x390' -ex detach -ex quit
```

## How to test a boot

From MSYS2 (UCRT64), in the repository folder, with the game dump linked at `extracted/`:

```sh
(export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"; make -j16)
# (if tools/lifter.py, tools/gen.py or runtime/cpu.h change: "make gen" first, and the whole game is recompiled)
export PATH="/c/msys64/ucrt64/bin:$PATH"   # without MSYS2's /usr/bin in front! (see below)
LARECOMP_HANG_DUMP=20 LARECOMP_NO_AUDIO=1 LARECOMP_NO_MOVIE_AUDIO=1 LARECOMP_FORCE_LAUNCHER=0 \
  timeout 90 ./build/smrpg-recomp.exe --play extracted > build/boot_X.log 2>&1
```

- Pitfall: with `/c/msys64/usr/bin` first in the PATH, MSYS2's `timeout` (mixed with Git's bash) does not pass the
  environment variables on to the game: no `LARECOMP_*` gets through. Compiling needs that PATH (in a subshell);
  launching, only `ucrt64/bin`.
- `LARECOMP_FNPROF=0:999999`: each hang dump writes `build/fnprof_hangN.txt` (address and number of times each game
  function ran). With `LARECOMP_FNPROF_BT=<addr>,...` you see who calls each one first and with which x0.
- `LARECOMP_WATCH_ADDR=<lo>,<hi>`: which game functions touch that memory (from startup).
- `LARECOMP_CRASH_HOLD=<s>`: after a crash the process waits that many seconds before exiting (for `gdb -p`).

- `init sdk` / `init main`: the constructors. A fatal error ends with the game's stack ("guest stack scan") and the last
  32 host calls of the thread that failed.
- `LARECOMP_HANG_DUMP=<s>`: if the main thread spends that long in a wait, every thread is dumped with its Unity name,
  what it is waiting for, its stack and the kernel trace (the last 2,048 mutex and condvar events).
- Addresses 0x7000000000+ belong to `main` and 0x7005000000+ to the `sdk` (until version 15, 0x7004000000+).

## Analyzing the game code

- `nxnative/dm/tools/nso2elf.py` converts `exefs/main`, `exefs/sdk` and `exefs/subsdk0` to ELF for Ghidra (they are not
  uploaded: they are game code). In Ghidra the image base has to be set to 0x7000000000 (main) or 0x7005000000 (sdk).
- The PLT stubs call through the GOT: to know which function they call, the JUMP_SLOT relocation of their GOT entry has
  to be resolved (that is how the sdk's 8,821 stubs were renamed to `plt_<symbol>`).
- Unity's NVN function pointers are in `main` variables (0x3da5xxx) that its loader fills in with
  nvnDeviceGetProcAddress; they are recognized by how they are used.
- The `sdk` and `subsdk0` of 1.0.0 are identical to those of 1.0.1 (same build ID), the one nxnative runs: what nxnative
  knows about this game's SDK holds as is, and its `docs/reference/` (threads, memory, rendering) covers many
  Unity-on-Switch problems already solved.

## What is known about the game

- `nn::init::Start` (sdk 0xfa3c0): nnosInitialize → (early call) → nninitStartup → (late call: the sdk and main
  constructors) → nnMain.
- The NPDM declares 16 MB of "system resource": nn::os virtual memory (VammManager) is enabled. The game's
  `nninitStartup` (main 0x62de90) reserves (total − used memory) ≈ 3.1 GB of virtual space for its `StandardAllocator`
  (object at main 0x3d822d0) and does not use SetHeapSize. The `AddressRegionManager` works on the alias region: it
  keeps the first GB and aligns large blocks to 1 GB. That is why the alias region is 48 GB.
- MapPhysicalMemory only maps (and zeroes) the missing pages, and only in the alias region.
- `svcBreak` with reason 0 is "Panic" (`nn::diag::detail::AbortImpl`): before that, the game calls am
  SetTerminateResult.
- Unity (`main`) defines global operator new/delete that replace the sdk's weak ones (interposition, like the rtld).
- nn::ro: Unity only calls Initialize and LookupSymbol (there are no NROs); the rtld does not exist here.
- The IL2CPP GC stops the threads with SuspendThread/GetThreadContext: cooperative pause at safe points.
- NVN 55: the game's `NVNtextureView` takes 16 bytes; its `NVNtexture`, 0x80. Unity requests 277 NVN functions; 19 are
  missing from the runtime (Barrier, SetAlphaRef, SetPointSize, CopyBufferToBuffer, "Deferred" binds…) and several are
  empty (BindSeparateSampler(s), ReportValue, DrawTexture…): they will have to be implemented when Unity draws.
- Memory ordering: on AArch64 `STLR` + `LDAR` are seq_cst; on x86 `STLR` has to be translated with a full barrier.
- Unity threads: MainThread, UnityGfxDeviceWorker, Loading.AsyncRead, Job.Worker 0-1, Background Job.Worker 0-15, GC
  Finalizer, AssetGarbageCollectorHelper, BatchDeleteObjects, NetworkInterfaceWrapper, LibcurlGc, an unnamed vsync
  thread (main 0x6b536c) and NxSystemThread (the SDK's).
- `main` imports 31 functions from `subsdk0` (nn::movie). The runtime does not load `subsdk0`: `runtime/hle/movie.c`
  implements them with FFmpeg (version 19: the ones CRIWARE Sofdec uses). Not implemented yet, because no calls have
  been seen: `GetH264NalUnitStartPositionAndNalSize` and the `Extractor` ones called by symbol.
- The movies are CRIWARE USM with VP9 1920x1080 at 60 frames/s and Opus audio (one stereo stream, through `hwopus`). CRI
  creates the decoder with type 3 (VP9), format 3 and mode 2 (native texture).
- CRI asks 196 times for the buffer size of an Opus multistream decoder of 2 mono streams (`hwopus` command 7) without
  opening it: it is its memory calculation, not a bug.
