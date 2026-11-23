# LAS Recomp build
# macOS: Homebrew clang/SDL3/FFmpeg, Metal renderer.
# Windows: MSYS2 UCRT64 shell (see docs/BUILD-windows.md), clang + Vulkan renderer.
# Linux: clang + Vulkan renderer, x86-64 or aarch64 (see docs/linux.md).
UNAME := $(shell uname -s)
comma := ,
ifneq ($(filter MINGW% MSYS% CYGWIN% Windows_NT,$(UNAME) $(OS)),)
PLATFORM := windows
else ifeq ($(UNAME),Linux)
PLATFORM := linux
else
PLATFORM := macos
endif
# (SMRPG Android: `make PLATFORM=android ABI=...`; la carpeta y el C generado, antes de las listas de objetos)
ifeq ($(PLATFORM),android)
ABI      ?= x86_64
B        ?= build/android-$(ABI)
ifeq ($(ABI),arm64-v8a)
GEN      ?= generated-aarch64
endif
endif

# Linux: ARCH (x86_64 / aarch64, default this machine's) picks the target, separately from PLATFORM. The generated code
# differs per arch (AArch64 hosts run the guest's FP/SIMD as inline asm, others use the portable C of simd.h), so a
# cross build has its own generated-<arch>/ and build/<arch>/; SYSROOT is the target's root filesystem (headers, libs).
GEN      ?= generated
ifeq ($(PLATFORM),linux)
HOSTARCH := $(patsubst arm64,aarch64,$(shell uname -m))
ARCH     ?= $(HOSTARCH)
ifneq ($(ARCH),$(HOSTARCH))
CROSS    := --target=$(ARCH)-linux-gnu $(if $(SYSROOT),--sysroot=$(SYSROOT))
GEN      := generated-$(ARCH)
B        ?= build/$(ARCH)
endif
GENSIMD  := --simd $(if $(filter aarch64,$(ARCH)),asm,c)
endif

# B: build dir. MACOS_MIN: oldest macOS to run on (default: this one). DEPS: prefix with SDL3 / FFmpeg instead of
# Homebrew's (make deps). `make app` sets all three (build/mac15, 15.0, build/deps); plain `make` uses none.
B         ?= build
MACOS_MIN ?=
DEPS      ?=
MINFLAG  := $(if $(MACOS_MIN),-mmacosx-version-min=$(MACOS_MIN))
CC       := clang $(MINFLAG) $(CROSS)
CXX      := clang++ $(MINFLAG) $(CROSS)
IMGUI    := third_party/imgui
GENFLAGS := -O1 -fno-strict-aliasing -Wno-return-type -Wno-unused-label -fvisibility=hidden -Iruntime

# The game code (made by `make gen` from your own game files) is its own library next to the app (runtime/gamelib.h);
# the app itself holds none of it
# (strip: with no generated code the space between the two would be all that's left, and $(if) takes a lone space as
# true. No comment after the value: make keeps the spaces before it)
GEN_SRC  := $(strip $(wildcard $(GEN)/main/*.c) $(wildcard $(GEN)/sdk/*.c))
# (SMRPG) tools/game_overrides.o el primero: sus funciones sustituyen a las generadas (GAMELIB_LDFLAGS, Windows)
GEN_OBJ  := $(B)/obj/tools/game_overrides.o $(patsubst %.c,$(B)/obj/%.o,$(patsubst $(GEN)/%,%,$(GEN_SRC))) $(B)/obj/tools/gamelib_entry.o
RT_SRC   := $(wildcard runtime/*.c runtime/*.cpp) $(wildcard runtime/hle/*.c) $(wildcard runtime/services/*.c) \
            $(wildcard runtime/nvn/*.c) $(wildcard runtime/nvn/maxwell/*.c) runtime/platform/$(PLATFORM).c
RT_HDRS  := $(wildcard runtime/*.h runtime/game_addresses/*.h) $(wildcard runtime/platform/*.h) $(wildcard runtime/nvn/*.h) \
            $(wildcard runtime/nvn/maxwell/*.h) $(wildcard runtime/nvn/vulkan/*.h) $(wildcard runtime/ui/*.h)
UI_SRC   := $(wildcard runtime/ui/*.mm)
UI_OBJ   := $(patsubst runtime/%.mm,$(B)/rt/%.o,$(UI_SRC))
IMGUI_OBJ := $(B)/imgui/imgui.o $(B)/imgui/imgui_draw.o $(B)/imgui/imgui_tables.o $(B)/imgui/imgui_widgets.o \
             $(B)/imgui/imgui_impl_sdl3.o

ifeq ($(PLATFORM),macos)
SDL      := $(or $(DEPS),/opt/homebrew/opt/sdl3)
FFMPEG   := $(or $(DEPS),/opt/homebrew/opt/ffmpeg)
EXE      := $(B)/lasrecomp
DEPINC   := -I$(SDL)/include -I$(FFMPEG)/include
LDFLAGS  := -L$(SDL)/lib -lSDL3 -Wl,-rpath,$(SDL)/lib -L$(FFMPEG)/lib -lavformat -lavcodec -lavutil -lswscale -lopus -Wl,-rpath,$(FFMPEG)/lib \
            -framework CoreFoundation -framework Foundation -framework Metal -framework MetalFX -framework QuartzCore -framework AppKit -framework ImageIO -framework CoreGraphics -framework UniformTypeIdentifiers
BACKEND_SRC := $(wildcard runtime/nvn/*.m)
BACKEND_OBJ := $(patsubst runtime/%.m,$(B)/rt/%.o,$(BACKEND_SRC))
IMGUI_OBJ += $(B)/imgui/imgui_impl_metal.o
UIFLAGS  := -fobjc-arc
GAMELIB  := $(B)/lasgame.dylib
GAMELIB_LDFLAGS := -dynamiclib -undefined dynamic_lookup  # (the runtime functions it calls: the app's own)
GAMELIB_SHARED := -shared -Wl,-undefined,dynamic_lookup  # (the same, for the start screen's build: zig cc)
else ifeq ($(PLATFORM),linux)
EXE      := $(B)/lasrecomp
DEPINC   := $(if $(DEPS),-I$(DEPS)/include)
ifeq ($(ARCH),x86_64)
GENARCH  ?= -march=x86-64-v3  # as on Windows: AVX2 + FMA for the lifted FP/SIMD code (checked at startup)
endif
GENFLAGS += $(GENARCH) -fPIC
OSFLAGS  := -Wno-format  # (%llx for uint64_t, which is unsigned long here: the same size)
# shaderc as the system's pkg-config file names it (Arch / Fedora / AOSC ship only libshaderc_shared, issue #22);
# without one -lshaderc (Debian / Ubuntu's libshaderc.so). SHADERC=... overrides it.
SHADERC  ?= $(shell pkg-config --libs shaderc 2>/dev/null || echo -lshaderc)
# -rdynamic: crash backtraces name host functions (runtime/platform/linux.c)
LDFLAGS  := -fuse-ld=lld -rdynamic $(if $(DEPS),-L$(DEPS)/lib) -lSDL3 -lavformat -lavcodec -lavutil -lswscale -lopus \
            -lvulkan $(SHADERC) -lpthread -ldl -lm
BACKEND_SRC := $(wildcard runtime/nvn/vulkan/*.c) $(wildcard runtime/nvn/vulkan/*.cpp)
BACKEND_OBJ := $(patsubst runtime/%.cpp,$(B)/rt/%.o,$(patsubst runtime/%.c,$(B)/rt/%.o,$(BACKEND_SRC)))
IMGUI_OBJ += $(B)/imgui/imgui_impl_vulkan.o
UIFLAGS  := -x c++  # the .mm UI sources are plain C++ apart from __APPLE__ blocks
GAMELIB  := $(B)/lasgame.so
GAMELIB_LDFLAGS := -shared -fuse-ld=lld  # (the runtime functions it calls: the app's own, -rdynamic)
GAMELIB_SHARED := -shared
else ifeq ($(PLATFORM),windows)
# (SMRPG) el ejecutable de la aplicación (SMRPG Recomp)
EXE      := $(B)/smrpg-recomp.exe
DEPINC   := $(if $(DEPS),-I$(DEPS)/include)
OSFLAGS  := -D_FILE_OFFSET_BITS=64 -D_POSIX_THREAD_SAFE_FUNCTIONS -D_CRT_RAND_S -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
            $(if $(RELEASE),-DLARECOMP_ZIP_BUILD)  # (the log's [info] line: the release zip)
GENARCH  ?= -march=x86-64-v3  # AVX2 + FMA for the lifted FP/SIMD code (simd.h); override for older CPUs
GENFLAGS += -D_FILE_OFFSET_BITS=64 $(GENARCH)
LDFLAGS  := -fuse-ld=lld -Wl,--stack,16777216 $(if $(DEPS),-L$(DEPS)/lib) -lSDL3 -lavformat -lavcodec -lavutil -lswscale -lopus \
            -lvulkan-1 -lws2_32 -lwinmm -lole32 -lshell32 -lcomdlg32 -luuid -lshaderc_shared -lpdh \
            $(if $(RELEASE),-Wl$(comma)--subsystem$(comma)windows -Wl$(comma)--strip-debug)  # release: no console window (log -> lasrecomp.log + lasrecomp-backup-*.log), no debug info
BACKEND_SRC := $(wildcard runtime/nvn/vulkan/*.c) $(wildcard runtime/nvn/vulkan/*.cpp)
BACKEND_OBJ := $(patsubst runtime/%.cpp,$(B)/rt/%.o,$(patsubst runtime/%.c,$(B)/rt/%.o,$(BACKEND_SRC)))
BACKEND_OBJ += $(B)/rt/platform/windows_rc.o  # the .exe's icon (Explorer, taskbar)
WINDRES  ?= windres
IMGUI_OBJ += $(B)/imgui/imgui_impl_vulkan.o
UIFLAGS  := -x c++ $(OSFLAGS)  # the .mm UI sources are plain C++ apart from __APPLE__ blocks
GAMELIB  := $(B)/lasgame.dll
# the runtime functions it calls: exported by the .exe, linked through its import library
LDFLAGS  += -Wl,--export-all-symbols -Wl,--out-implib,$(B)/smrpg-recomp.dll.a
GAMELIB_LDFLAGS := -shared -fuse-ld=lld $(B)/smrpg-recomp.dll.a -Wl,--allow-multiple-definition
GAMELIB_SHARED := -shared  # (+ the import library: runtime/gamebuild.c)
endif

# (SMRPG Android) `make PLATFORM=android ABI=x86_64|arm64-v8a`: el programa como build/android-<abi>/libmain.so (lo
# carga SDLActivity: tools/android/app) y el código del juego como lasgame.so, enlazado contra él. Con el NDK del SDK
# de Android (NDK, o ANDROID_SDK/ndk/29.0.14206865: del entorno o de local.env, ver local.env.ejemplo) y las
# dependencias de tools/android/deps.sh (../deps/<abi>). El APK: tools/android/apk.sh.
# x86_64 es para el emulador (el mismo C generado que el PC, --simd c); arm64-v8a, para los móviles.
ifeq ($(PLATFORM),android)
ANDROID_API := 33
# (sin comentario detrás de los valores: make guardaría en la ruta los espacios de antes del #)
LOCAL_ANDROID_SDK := $(shell sed -n 's/^ANDROID_SDK=//p' local.env 2>/dev/null | tail -n 1 | tr -d '\r')
LOCAL_NDK := $(shell sed -n 's/^NDK=//p' local.env 2>/dev/null | tail -n 1 | tr -d '\r')
ANDROID_SDK := $(or $(ANDROID_SDK),$(LOCAL_ANDROID_SDK))
NDK      ?= $(or $(LOCAL_NDK),$(if $(ANDROID_SDK),$(ANDROID_SDK)/ndk/29.0.14206865))
ifeq ($(NDK),)
$(error falta el NDK de Android: pon ANDROID_SDK o NDK en local.env (ver local.env.ejemplo) o en el entorno)
endif
NDKTC    := $(NDK)/toolchains/llvm/prebuilt/windows-x86_64
ATRIPLE  := $(if $(filter arm64-v8a,$(ABI)),aarch64,x86_64)-linux-android$(ANDROID_API)
CC       := "$(NDKTC)/bin/clang.exe" --target=$(ATRIPLE) -fPIC  # (todo va en bibliotecas compartidas: todas las reglas)
CXX      := "$(NDKTC)/bin/clang++.exe" --target=$(ATRIPLE) -fPIC
# (relativa: abspath no mira CURDIR, y con espacios en la carpeta rompería -I y -L; sin comentario detrás)
ADEPS    ?= ../deps/$(ABI)
EXE      := $(B)/libmain.so
DEPINC   := -I$(ADEPS)/include
OSFLAGS  := -fPIC -Wno-format -DSMRPG_ANDROID
# la versión de Android (la del APK: la primera «x.y.z-alfa|beta» de VERSIONES-ANDROID.txt) en el menú y el registro
ANDROID_VN := $(shell sed -n 's/^\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*-[a-z]*\) .*/\1/p' VERSIONES-ANDROID.txt | head -1)
OSFLAGS  += -DANDROID_APP_VERSION=\"$(ANDROID_VN)\"
$(B)/rt/main.o $(B)/rt/ui/menu.o: VERSIONES-ANDROID.txt
ifeq ($(ABI),arm64-v8a)
GENSIMD  := --simd asm
# el juego, 0x60_0000_0000 más abajo (runtime/guest_addr.h): con 39 bits de direcciones, su sitio de siempre es de Android
GUEST_SHIFT := 0x6000000000
OSFLAGS  += -DGUEST_SHIFT=$(GUEST_SHIFT)ull
GENFLAGS += -DGUEST_SHIFT=$(GUEST_SHIFT)ull
else
# (el emulador de Android no ofrece AVX2, FMA ni BMI2 a su CPU invitada: x86-64-v2; lasgame.so.cpu lo dice)
GENARCH  ?= -march=x86-64-v2
GAMELIB_CPU := $(patsubst -march=%,%,$(GENARCH))
GENFLAGS += $(GENARCH)
endif
GENFLAGS += -fPIC
LDFLAGS  := -shared -fuse-ld=lld -Wl,-soname,libmain.so -Wl,--no-undefined -L$(ADEPS)/lib -lSDL3 -lavformat -lavcodec -lavutil -lswscale \
            -lopus -lshaderc_combined -lvulkan -landroid -lnativewindow -llog -ldl -lm -static-libstdc++
# (AMD FSR y su generación de fotogramas son DLL de Windows: vk_amd_none.c, «no disponible»)
BACKEND_SRC := $(filter-out %/vk_fsr.cpp %/vk_fg.cpp,$(wildcard runtime/nvn/vulkan/*.c) $(wildcard runtime/nvn/vulkan/*.cpp))
BACKEND_OBJ := $(patsubst runtime/%.cpp,$(B)/rt/%.o,$(patsubst runtime/%.c,$(B)/rt/%.o,$(BACKEND_SRC)))
IMGUI_OBJ += $(B)/imgui/imgui_impl_vulkan.o
UIFLAGS  := -x c++ $(OSFLAGS)
GAMELIB  := $(B)/lasgame.so
# (el código del juego llama a funciones del programa: con libmain.so como dependencia, el enlazador de Android las
# resuelve contra la que ya está cargada; game_overrides.o el primero gana, como en Windows)
GAMELIB_LDFLAGS := -shared -fuse-ld=lld -Wl,-soname,lasgame.so -L$(B) -lmain -Wl,--allow-multiple-definition
GAMELIB_SHARED := -shared
endif

# GAMELIB_ID: what the game code library is built from (the generator, the headers it is compiled against, the
# flags). A library with another id is rebuilt by the start screen, never loaded.
GAMELIB_SRCS := tools/game_addresses.py runtime/game_addresses/versions.h $(wildcard runtime/game_addresses/patches_v*.h) tools/gen.py tools/lifter.py tools/analyze.py tools/nso.py tools/passes.py tools/patches.py \
                tools/simd_c.py tools/rando/patch_variants.json tools/gamelib_entry.c runtime/hle/hle_symbols.txt \
                runtime/cpu.h runtime/simd.h runtime/simd_fp.h runtime/gamelib.h
GAMELIB_ID := $(shell (cat $(GAMELIB_SRCS); echo '$(GENFLAGS)') | (shasum -a 256 2>/dev/null || sha256sum) | cut -c1-16)
# The library's own id adds the game's main build ID (what `make gen` generated from: $(GEN)/main/main_build_id.txt),
# so game code for another game version is never loaded (runtime/gamelib.h gamelib_full_id)
GAMELIB_BUILD := $(shell cut -c1-16 $(GEN)/main/main_build_id.txt 2>/dev/null)
GAMELIB_FULL_ID := $(GAMELIB_ID)-$(or $(GAMELIB_BUILD),unknown)
# GAMELIB_BASE_ID: what every compiled piece of it depends on (the headers, the flags, the pinned compiler). The same
# base keeps the start screen's earlier build pieces: only the pieces whose generated C changed are compiled again
# (runtime/gamebuild.c). Another base: everything is compiled again.
GAMELIB_BASE_SRCS := runtime/cpu.h runtime/simd.h runtime/simd_fp.h runtime/gamelib.h tools/fetch_zig.sh
GAMELIB_BASE_ID := $(shell (cat $(GAMELIB_BASE_SRCS); echo '$(GENFLAGS) $(GAMELIB_SHARED)') | \
                     (shasum -a 256 2>/dev/null || sha256sum) | cut -c1-16)

RT_OBJ   := $(patsubst runtime/%.cpp,$(B)/rt/%.o,$(patsubst runtime/%.c,$(B)/rt/%.o,$(RT_SRC)))
# no path of this machine in what ships: debug info and __FILE__ name the checkout as "." (Windows: its C:/ form too)
# ABSDIR: la carpeta real. Si lleva espacios, make se llama con CURDIR=. (todo lo demás, relativo) y CURDIR ya no la
# nombra. Siempre entre comillas.
ABSDIR   := $(shell pwd)
PATHMAP  := '-ffile-prefix-map=$(ABSDIR)=.' $(if $(filter windows,$(PLATFORM)),'-ffile-prefix-map=$(shell cygpath -m '$(ABSDIR)')=.')
RTFLAGS  := -O2 -g -fno-strict-aliasing -Wall -Wno-unused-function -Iruntime -Iruntime/nvn $(DEPINC) $(OSFLAGS) $(PATHMAP)
CXXFLAGS := -O2 -g -std=c++17 -I$(IMGUI) -I$(IMGUI)/backends -Iruntime -Iruntime/nvn $(DEPINC) $(PATHMAP)

all: $(EXE) $(if $(GEN_SRC),$(GAMELIB))

$(B)/obj/%.o: $(GEN)/%.c runtime/cpu.h runtime/simd.h runtime/simd_fp.h
	@mkdir -p $(dir $@)
	@$(CC) $(GENFLAGS) -c $< -o $@

$(B)/obj/tools/game_overrides.o: tools/game_overrides.c runtime/cpu.h
	@mkdir -p $(dir $@)
	$(CC) $(GENFLAGS) -c $< -o $@

$(B)/obj/tools/gamelib_entry.o: tools/gamelib_entry.c $(GAMELIB_SRCS) $(wildcard $(GEN)/main/main_build_id.txt)
	@mkdir -p $(dir $@)
	$(CC) $(GENFLAGS) -DGAMELIB_ID='"$(GAMELIB_FULL_ID)"' -c $< -o $@
$(B)/rt/gamelib.o: $(GAMELIB_SRCS)
$(B)/rt/gamelib.o: RTFLAGS += -DGAMELIB_ID='"$(GAMELIB_ID)"'
# the start screen's build of it (runtime/gamebuild.c) uses the same flags
$(B)/rt/gamebuild.o: $(GAMELIB_SRCS) $(GAMELIB_BASE_SRCS) Makefile
$(B)/rt/gamebuild.o: RTFLAGS += -DGAMELIB_ID='"$(GAMELIB_ID)"' -DGAMELIB_BASE_ID='"$(GAMELIB_BASE_ID)"' \
                                -DGAMELIB_CFLAGS='"$(strip $(filter-out -Iruntime,$(GENFLAGS)))"' \
                                -DGAMELIB_SHARED='"$(GAMELIB_SHARED)"'

$(B)/rt/%.o: runtime/%.c $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(RTFLAGS) -c $< -o $@

# SMAA (third_party/smaa, MIT): the reference shader as a C string for runtime/nvn/smaa_data.c (both backends build
# their passes from it); the non-ASCII bytes in its comments become '?', and its CRLF line ends plain LF (a CR left in
# the string literals ends them early on macOS clang)
$(B)/inc/smaa_hlsl.inc: third_party/smaa/SMAA.hlsl
	@mkdir -p $(dir $@)
	LC_ALL=C tr -d '\r' < $< | LC_ALL=C tr '\200-\377' '?' | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' > $@
$(B)/rt/nvn/smaa_data.o: $(B)/inc/smaa_hlsl.inc
$(B)/rt/nvn/smaa_data.o: RTFLAGS += -I$(B)/inc -Ithird_party/smaa/Textures

# The shader preparation's variant list (runtime/nvn/shader_prep.h: no shader code, hashes and pipeline state only)
# as a C string for runtime/nvn/shader_prep.c
$(B)/inc/shader_variants.inc: runtime/nvn/shader_variants.txt
	@mkdir -p $(dir $@)
	(LC_ALL=C tr -d '\r' < $< | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/'; echo '""') > $@
$(B)/rt/nvn/shader_prep.o: $(B)/inc/shader_variants.inc
$(B)/rt/nvn/shader_prep.o: RTFLAGS += -I$(B)/inc
# the shader translator's id: part of the shader caches' tag (shader_cache_tag), so a changed translator rebuilds them
TRANSLATOR_SRCS := runtime/nvn/maxwell/translate.c runtime/nvn/maxwell/shader.h
TRANSLATOR_ID := $(shell cat $(TRANSLATOR_SRCS) | (shasum -a 256 2>/dev/null || sha256sum) | cut -c1-12)
$(B)/rt/nvn/shader_prep.o: $(TRANSLATOR_SRCS)
$(B)/rt/nvn/shader_prep.o: RTFLAGS += -DSHADER_TRANSLATOR_ID='"$(TRANSLATOR_ID)"'

$(B)/rt/%.o: runtime/%.cpp $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(OSFLAGS) -c $< -o $@

$(B)/rt/%.o: runtime/%.m $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(RTFLAGS) -fobjc-arc -c $< -o $@

# linked to a temp file, then renamed over: a copy of the game still running keeps its (old) file - relinking in
# place gets running copies killed by macOS (code signature of the changed pages). The temp file keeps the real name
# (in a folder of its own): on Windows the import library records the name the game code library loads it by.
$(EXE): $(RT_OBJ) $(BACKEND_OBJ) $(UI_OBJ) $(IMGUI_OBJ)
	@mkdir -p $(dir $@)link
	$(CXX) -o $(dir $@)link/$(notdir $@) $^ $(LDFLAGS)
	mv -f $(dir $@)link/$(notdir $@) $@

# its id file (<library>.id) says what it was built from without loading it (runtime/gamelib.c)
$(GAMELIB): $(GEN_OBJ) | $(EXE)
	$(file >$@.rsp,$^)
	$(CC) $(GAMELIB_LDFLAGS) -o $@.tmp @$@.rsp  # (los ~1.056 objetos por un archivo: en Windows una orden no pasa de 32 KB)
	mv -f $@.tmp $@
	echo $(GAMELIB_FULL_ID) > $@.id
	$(if $(GAMELIB_CPU),echo $(GAMELIB_CPU) > $@.cpu)

$(B)/rt/ui/%.o: runtime/ui/%.mm $(RT_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(UIFLAGS) -c $< -o $@

$(B)/imgui/%.o: $(IMGUI)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(B)/imgui/imgui_impl_%.o: $(IMGUI)/backends/imgui_impl_%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(B)/imgui/imgui_impl_metal.o: $(IMGUI)/backends/imgui_impl_metal.mm
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -fobjc-arc -c $< -o $@

# Python: tools/venv (macOS, Linux: bin/python, Windows: Scripts/python.exe)
PY := $(firstword $(wildcard tools/venv/bin/python) $(wildcard tools/venv/Scripts/python.exe))

# (SMRPG Android arm64: el juego movido GUEST_SHIFT bytes más abajo —runtime/guest_addr.h—: el código se genera para
# las bases movidas y las funciones conservan sus nombres de siempre, gen.py --name-shift; en el resto, 0)
GUEST_SHIFT ?= 0
GEN_SDK_BASE  := $(shell printf '0x%x' $$((0x7005000000 - $(GUEST_SHIFT))))
GEN_MAIN_BASE := $(shell printf '0x%x' $$((0x7000000000 - $(GUEST_SHIFT))))
GEN_SHIFT_ARG := $(if $(filter-out 0,$(GUEST_SHIFT)),--name-shift $(GUEST_SHIFT))
gen:
	cd tools && ../$(PY) gen.py sdk ../extracted/exefs/sdk $(GEN_SDK_BASE) ../$(GEN)/sdk --hle ../runtime/hle/hle_symbols.txt $(GENSIMD) $(GEN_SHIFT_ARG)
	cd tools && ../$(PY) gen.py main ../extracted/exefs/main $(GEN_MAIN_BASE) ../$(GEN)/main $(GENSIMD) $(GEN_SHIFT_ARG)

gen-objs: $(GEN_OBJ)

# Runtime objects only (no generated code, no link): quick compile check
runtime-objs: $(RT_OBJ) $(BACKEND_OBJ) $(UI_OBJ)

# (SMRPG) la versión del recurso VERSIONINFO: el número de la primera entrada de VERSIONES.txt
SMRPG_VERSION := $(shell sed -n 's/^Versi[^ ]* \([0-9][0-9]*\) .*/\1/p' VERSIONES.txt | head -1)
$(B)/rt/platform/windows_rc.o: runtime/platform/windows.rc docs/icon/icon.ico runtime/platform/windows.manifest VERSIONES.txt
	@mkdir -p $(dir $@)
	$(WINDRES) -I docs/icon -I runtime/platform -DSMRPG_VERSION=$(or $(SMRPG_VERSION),0) -O coff $< -o $@

VERSION  := $(shell sed -n 's/^\#define LARECOMP_VERSION "\(.*\)"/\1/p' runtime/ui/launcher.mm)

# macOS: build/LAS Recomp.app, runs on macOS $(APP_MIN)+: the game rebuilt for that in build/mac15 against build/deps
# (`make deps`: SDL3, LGPL FFmpeg, keystone, Python, from pinned sources), the generator helper when built
# (`make rando-helper` after `make deps`), packaged by tools/bundle_macos.sh. No game files or fonts.
APP_MIN  := 15.0
APP_B    := build/mac15
deps:
	MACOS_MIN=$(APP_MIN) tools/build_deps_macos.sh
app:
	@[ -f build/deps/lib/libSDL3.0.dylib ] || { echo "make deps first"; exit 1; }
	$(MAKE) B=$(APP_B) MACOS_MIN=$(APP_MIN) DEPS=$(CURDIR)/build/deps $(APP_B)/lasrecomp
	MACOS_MIN=$(APP_MIN) tools/bundle_macos.sh $(APP_B)/lasrecomp

# macOS release disk image (the .app + an Applications shortcut to drag it onto): build/dist/LAS Recomp-<v>-macOS.dmg
dmg: app
	tools/make_dmg.sh

# Windows: build/dist/LAS Recomp-<version>-windows.zip, top folder "LAS Recomp/": the exe, the seed generator
# helper (make rando-helper), what the start screen builds the game code with on the player's machine (make gen-helper,
# tools/fetch_zig.sh slimmed by tools/zig_slim.sh: its runtime built ahead of time, gamelib-src/, the exe's import library)
# and the DLLs they load (runs without MSYS2). No game code. The exe is a release build in $(WIN_B):
# linked against the LGPL FFmpeg from `make deps-windows` (MSYS2's is a GPLv3 build) and without a console window
# (RELEASE=1: the log goes next to the exe - lasrecomp.log is the latest run, plus up to five recent
# lasrecomp-backup-*.log - and fatal errors show a message box). Its DLLs are looked up
# with build/deps-win/bin first (the FFmpeg DLL names are the same as MSYS2's GPL ones). vulkan-1.dll is left
# out: every Vulkan driver installs the loader in System32. Saves, seeds and game files go next to the exe at run time.
# Needs: Windows 10+ x64, a Vulkan 1.3 driver, an AVX2 CPU (GENARCH x86-64-v3; checked at startup).
DIST     := build/dist/LAS Recomp
WIN_B    := build/win-release
deps-windows:
	tools/build_deps_windows.sh
dist:
	@[ -f build/deps-win/lib/libavcodec.dll.a ] || { echo "make deps-windows first (LGPL FFmpeg)"; exit 1; }
	@[ -d build/helper/rando-gen ] || { echo "make rando-helper first (the seed generator)"; exit 1; }
	@[ -d build/helper/lasgen ] || { echo "make gen-helper first (the game code generator)"; exit 1; }
	@[ -x build/zig/zig.exe ] || { echo "tools/fetch_zig.sh first (the compiler)"; exit 1; }
	$(MAKE) B=$(WIN_B) DEPS=$(CURDIR)/build/deps-win RELEASE=1 $(WIN_B)/lasrecomp.exe
	! nm $(WIN_B)/lasrecomp.exe | grep ' f_70' >/dev/null  # (no game code inside)
	rm -rf build/dist && mkdir -p "$(DIST)/gamelib-src"
	cp $(WIN_B)/lasrecomp.exe $(WIN_B)/lasrecomp.dll.a "$(DIST)/"
	tools/zig_slim.sh $(strip $(filter-out -Iruntime,$(GENFLAGS)))  # (zig with its runtime built ahead of time)
	cp -R build/helper/lasgen "$(DIST)/"
	cp -R build/zig-slim "$(DIST)/zig"
	cp runtime/cpu.h runtime/simd.h runtime/simd_fp.h runtime/gamelib.h tools/gamelib_entry.c runtime/hle/hle_symbols.txt \
	    "$(DIST)/gamelib-src/"
	cp docs/icon/icon_1024.png "$(DIST)/icon.png"
	cp third_party/fonts/Baloo2/Baloo2-SemiBold.ttf "$(DIST)/"  # (start screen font, OFL)
	cp third_party/fonts/Baloo2/OFL.txt "$(DIST)/Baloo2-OFL.txt"
	cp third_party/fonts/DejaVu/DejaVuSans.ttf third_party/fonts/DejaVu/DejaVuSans-Bold.ttf "$(DIST)/"  # (last-resort fonts, free license)
	cp third_party/fonts/DejaVu/LICENSE "$(DIST)/DejaVu-LICENSE.txt"
	cp README.md LICENSE "$(DIST)/"  # (licenses and source links of the bundled components: README "License")
	cp -R build/helper/rando-gen "$(DIST)/"
	PATH="$(ABSDIR)/build/deps-win/bin:$$PATH" ldd $(WIN_B)/lasrecomp.exe | sed -n 's/^.* => \(.*\) (0x[0-9a-f]*)$$/\1/p' | \
	    grep -E 'ucrt64|deps-win' | grep -v vulkan-1 | sort -u | tr '\n' '\0' | xargs -0 -I{} cp -u {} "$(DIST)/"  # (rutas con espacios)
	! ls "$(DIST)" | grep -iE 'x264|x265|xvid|postproc'  # (no GPL codec libraries in the release)
	# (no path of this machine inside: build/deps-win, debug info, zig's runtime. Not on CI: its paths name no one)
	if [ "$${CI:-}" != true ]; then \
	    case "$$(basename "$$(cygpath -u "$$USERPROFILE")")" in runner|runneradmin) h= ;; *) h=$$(cygpath -m "$$USERPROFILE") ;; esac; \
	    ! grep -rlaiF -e "$(ABSDIR)" -e "$$(cygpath -m '$(ABSDIR)')" -e "$$(cygpath -w '$(ABSDIR)')" \
	        $${h:+-e "$$h" -e "$$(cygpath -w "$$h")"} "$(DIST)"; fi
	cd build/dist && if command -v zip >/dev/null; then zip -qr "LAS Recomp-$(VERSION)-windows.zip" "LAS Recomp"; \
	    else /c/Windows/System32/tar.exe -a -cf "LAS Recomp-$(VERSION)-windows.zip" "LAS Recomp"; fi
	@echo "build/dist/LAS Recomp-$(VERSION)-windows.zip"

# (SMRPG) El instalador para Windows: build/dist/SMRPG-Recomp-Instalador-<versión>.exe (tools/instalador). Una ventana
# que pide la copia del juego ya extraída (la carpeta con exefs y romfs) y la carpeta, y que lleva dentro
# el programa tal como se reparte, build/dist/SMRPG Recomp/: el ejecutable release (sin consola, con el FFmpeg
# LGPL de `make deps-windows`), gamelib-src (con tools/game_overrides.c), las DLL que carga, las de AMD FSR (build/,
# fuera del repositorio: third_party/amd-fsr/LEEME.txt), las fuentes, las licencias y construir\: construir.dll
# (tools/instalador/construir.cpp: el generador en un Python embebido, clang y lld de MSYS2 como bibliotecas) con todo
# lo que usa (tools/instalador/herramientas.sh). Nada del juego: el instalador pone allí la copia ya extraída de quien
# instala y construye su código, DENTRO de su propio proceso (sin procesos hijos). Necesita en MSYS2 UCRT64 los
# paquetes clang, lld y llvm (sus cabeceras y bibliotecas: pacman -S mingw-w64-ucrt-x86_64-llvm) y build/gen-venv
# (`make build/gen-venv/.ok`: capstone, unicorn y lz4 para el generador).
APP_VER    := $(shell sed -n 's/^\#define APP_VERSION "\(.*\)"/\1/p' runtime/app_name.h)
APP_VERNUM := $(shell echo '$(APP_VER)' | sed 's/-.*//; s/\./,/g'),0
SMRPG_DIST := build/dist/SMRPG Recomp
INST_EXE   := build/dist/SMRPG-Recomp-Instalador-$(APP_VER).exe
INST_LIBS  := -static -lzstd -lcomctl32 -lcomdlg32 -lole32 -luuid -lshell32 -luser32 -lgdi32
CONSTRUIR  := build/construir
# (-fno-exceptions y NDEBUG, como LLVM release: su red de seguridad salta con longjmp, sin destructores por medio)
$(CONSTRUIR)/construir.dll: tools/instalador/construir.cpp $(GAMELIB_SRCS) Makefile
	@mkdir -p $(dir $@) build/tmp-construir  # (los temporales, fuera de lo que se reparte)
	TMP="$$(cygpath -w build/tmp-construir)" TEMP="$$(cygpath -w build/tmp-construir)" \
	$(CXX) -O2 -std=c++17 -fno-exceptions -DNDEBUG -Wall -Wno-unused-function -shared $(PATHMAP) \
	    '-DGAMELIB_ID="$(GAMELIB_ID)"' $< -o $@ -llldMinGW -llldCOFF -llldCommon -lclang-cpp -lLLVM-23 -lbcrypt -lpsapi
instalador: build/gen-venv/.ok $(CONSTRUIR)/construir.dll
	@[ -f build/deps-win/lib/libavcodec.dll.a ] || { echo "make deps-windows primero (el FFmpeg LGPL)"; exit 1; }
	@for d in loader upscaler framegeneration; do [ -f build/amd_fidelityfx_$${d}_vk.dll ] || \
	    { echo "faltan las DLL de AMD FSR en build/ (third_party/amd-fsr/LEEME.txt)"; exit 1; }; done
	$(MAKE) B=$(WIN_B) DEPS=$(CURDIR)/build/deps-win RELEASE=1 $(WIN_B)/smrpg-recomp.exe
	TMP="$$(cygpath -w build/tmp-construir)" TEMP="$$(cygpath -w build/tmp-construir)" tools/instalador/herramientas.sh $(CONSTRUIR)
	rm -rf "$(SMRPG_DIST)" && mkdir -p "$(SMRPG_DIST)/gamelib-src" "$(SMRPG_DIST)/licencias"
	cp $(WIN_B)/smrpg-recomp.exe $(WIN_B)/smrpg-recomp.dll.a "$(SMRPG_DIST)/"
	cp -R $(CONSTRUIR) "$(SMRPG_DIST)/construir"
	cp runtime/cpu.h runtime/simd.h runtime/simd_fp.h runtime/gamelib.h tools/gamelib_entry.c runtime/hle/hle_symbols.txt \
	    tools/game_overrides.c "$(SMRPG_DIST)/gamelib-src/"
	cp docs/icon/icon_512.png "$(SMRPG_DIST)/icon.png"
	cp third_party/fonts/Baloo2/Baloo2-SemiBold.ttf third_party/fonts/BIZUDPGothic/BIZUDPGothic-Regular.ttf \
	    third_party/fonts/DejaVu/DejaVuSans.ttf third_party/fonts/DejaVu/DejaVuSans-Bold.ttf "$(SMRPG_DIST)/"
	cp build/amd_fidelityfx_loader_vk.dll build/amd_fidelityfx_upscaler_vk.dll build/amd_fidelityfx_framegeneration_vk.dll \
	    "$(SMRPG_DIST)/"
	PATH="$(ABSDIR)/build/deps-win/bin:$$PATH" ldd $(WIN_B)/smrpg-recomp.exe | sed -n 's/^.* => \(.*\) (0x[0-9a-f]*)$$/\1/p' | \
	    grep -E 'ucrt64|deps-win' | grep -v vulkan-1 | sort -u | tr '\n' '\0' | xargs -0 -I{} cp -u {} "$(SMRPG_DIST)/"  # (rutas con espacios)
	tools/instalador/licencias.sh "$(SMRPG_DIST)"  # (los textos de todo lo que va con él, y las fuentes que piden darse)
	sed 's/@VERSION@/$(APP_VER)/' tools/instalador/LEEME-instalado.txt > "$(SMRPG_DIST)/LEEME.txt"
	! ls "$(SMRPG_DIST)" | grep -iE 'x264|x265|xvid|postproc'  # (sin bibliotecas de códecs GPL)
	! find "$(SMRPG_DIST)" -iname 'lasgame*' -o -iname '*.keys' -o -iname 'extracted' -o -iname 'romfs' -o -iname 'main.npdm' -o -iname 'Lossless*.dll' | grep .
	mkdir -p build/instalador  # (ninguna ruta de este PC; sin patrones vacíos: uno vacío coincide con todo)
	{ echo "$(ABSDIR)"; cygpath -m '$(ABSDIR)'; cygpath -w '$(ABSDIR)'; \
	  if [ -n "$${USERPROFILE:-}" ]; then cygpath -m "$$USERPROFILE"; cygpath -w "$$USERPROFILE"; \
	  else echo "(sin USERPROFILE: la carpeta del usuario no se comprueba)" >&2; fi; } | grep . > build/instalador/rutas.txt
	! grep -rlaiF -f build/instalador/rutas.txt "$(SMRPG_DIST)"
	! grep -laiF -f build/instalador/rutas.txt Makefile LICENSE tools/instalador/*  # (lo de licencias/fuentes/instalador-fuente.zip, que va comprimido)
	mkdir -p build/instalador && cp docs/icon/icon.ico build/instalador/
	$(WINDRES) -I build/instalador -I tools/instalador -DINSTALADOR_VERSION_NUM=$(APP_VERNUM) \
	    '-DINSTALADOR_VERSION_STR=\"$(APP_VER)\"' -O coff tools/instalador/instalador.rc -o build/instalador/instalador_res.o
	TMP="$$(cygpath -w build/instalador)" TEMP="$$(cygpath -w build/instalador)" \
	$(CC) -O2 -Wall -Wno-unused-function -municode -mwindows $(PATHMAP) '-DINSTALADOR_VERSION="$(APP_VER)"' \
	    tools/instalador/instalador.c build/instalador/instalador_res.o -o build/instalador/instalador.exe $(INST_LIBS)  # (TMP: el de fuera puede llegar mal)
	rm -f "$(INST_EXE)"
	build/instalador/instalador.exe --empaquetar "$$(cygpath -w '$(SMRPG_DIST)')" "$$(cygpath -w '$(INST_EXE)')"
	@ls -la "$(INST_EXE)"

# Seed generator as a standalone helper, so players need no Python: build/helper/rando-gen/ (rando-gen[.exe] +
# _internal/), frozen by PyInstaller from tools/rando/rando_gen.spec in its own venv (build/rando-venv). Ship the
# rando-gen folder next to the exe (Windows) or as "LAS Recomp.app"/Contents/Resources/rando-gen (runtime/ui/rando_tab.mm
# generator_cmd). PyInstaller only builds for the OS it runs on.
# macOS arm64: the relocatable Python 3.12 and keystone from `make deps` (macOS 11+ / 15+: the helper runs where the
#   app does); without them Homebrew's python@3.12 and keystone (then only on the macOS they were built for). The
#   keystone wheel has no dylib: KEYSTONE_LIB tells rando_gen.spec which one to take.
# Windows x86-64 (untested): from the MSYS2 shell with a python.org CPython 3.12 (MSYS2's Python has no oead wheel),
#   e.g. make rando-helper RANDO_PY="/c/Users/<you>/AppData/Local/Programs/Python/Python312/python.exe"
ifeq ($(PLATFORM),macos)
RANDO_PY  ?= $(firstword $(wildcard build/deps/python/bin/python3.12) python3.12)
export KEYSTONE_LIB ?= $(wildcard $(CURDIR)/build/deps/lib/libkeystone.dylib)
RANDO_VPY := build/rando-venv/bin/python
else
RANDO_PY  ?= py -3.12
RANDO_VPY := build/rando-venv/Scripts/python.exe
endif

rando-helper: build/rando-venv/.ok
	$(if $(filter windows,$(PLATFORM)),USERPROFILE="$$(cygpath -w ~ 2>/dev/null || echo $$HOME)") \
	$(RANDO_VPY) -m PyInstaller --noconfirm --clean --log-level WARN --distpath build/helper --workpath build/pyinstaller \
	    tools/rando/rando_gen.spec
	@du -sh build/helper/rando-gen

# The game code generator, the same way (tools/lasgen.spec): build/helper/lasgen/, shipped as lasgen/ next to the exe or
# in Contents/Resources. The start screen's setup runs it on the player's own game files (runtime/gamebuild.c).
# (SMRPG) en su propio entorno (build/gen-venv): solo lo que pide el generador, sin el aleatorizador de LAS
GEN_VPY := $(if $(filter windows,$(PLATFORM)),build/gen-venv/Scripts/python.exe,build/gen-venv/bin/python)
gen-helper: build/gen-venv/.ok
	$(if $(filter windows,$(PLATFORM)),USERPROFILE="$$(cygpath -w ~ 2>/dev/null || echo $$HOME)") \
	$(GEN_VPY) -m PyInstaller --noconfirm --clean --log-level WARN --distpath build/helper --workpath build/pyinstaller \
	    tools/lasgen.spec
	$(if $(filter windows,$(PLATFORM)),$(GEN_VPY) tools/pe_no_cfg.py build/helper/lasgen/lasgen.exe)
	echo $(GAMELIB_ID) > build/helper/lasgen.gamelib_id  # (de qué fuentes es: `make instalador` lo comprueba)
	@du -sh build/helper/lasgen

build/gen-venv/.ok:
	$(RANDO_PY) -m venv build/gen-venv
	$(GEN_VPY) -m pip install -q capstone==5.0.9 unicorn==2.1.4 lz4==4.4.5 pyinstaller==6.22.3
	touch $@

build/rando-venv/.gen-ok: build/rando-venv/.ok
	$(RANDO_VPY) -m pip install -q capstone==5.0.9 unicorn==2.1.4 lz4==4.4.5
	touch $@

build/rando-venv/.ok: third_party/LAS-Randomizer/requirements.txt
	$(RANDO_PY) -m venv build/rando-venv
	$(RANDO_VPY) -m pip install -q -r third_party/LAS-Randomizer/requirements.txt pyinstaller==6.22.3
	touch $@

# Linux: build/dist/LAS Recomp-<version>-linux-<arch>.AppImage, built in Ubuntu 22.04 containers (podman) with SDL3, LGPL
# FFmpeg, shaderc and the seed generator helper inside; the Vulkan loader and GPU drivers are the system's.
# `make appimage` (ARCH: this machine's, or aarch64 cross-compiled), `make appimages` both. See docs/linux.md.
appimage:
	tools/linux/appimage.sh $(ARCH)
appimages:
	tools/linux/appimage.sh x86_64
	tools/linux/appimage.sh aarch64
# release check of the x86-64 AppImage: start screen, into the game, warps to the overworld and every dungeon
smoketest:
	tools/linux/smoketest.sh

.PHONY: dmg all gen gen-helper gen-objs runtime-objs deps deps-windows app dist instalador rando-helper appimage appimages smoketest

build/test_shaders: tools/test_shaders.m runtime/nvn/maxwell/translate.c runtime/nvn/maxwell/shader.h
	clang -O1 -fobjc-arc -framework Metal -framework Foundation -o $@ tools/test_shaders.m runtime/nvn/maxwell/translate.c
