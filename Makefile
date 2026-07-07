# gametank-libretro Makefile
#
# Targets:
#   make                       -> native shared object (gametank_libretro.so /
#                                 .dylib on macOS / .dll on Windows)
#   make platform=emscripten   -> gametank_libretro.{js,wasm} (no SDL)
#   make platform=wasm         -> alias for emscripten
#   make test                  -> build the native .so + dlopen harness
#   make clean
#
# The mos6502 core requires the upstream defines (CPU_6502_STATIC,
# CPU_6502_USE_LOCAL_HEADER, CMOS_INDIRECT_JMP_FIX); LIBRETRO_BUILD routes
# SDL_inc.h at the typedef shim instead of real SDL.

CORE      := gametank
TARGET    := $(CORE)_libretro

# ---------------------------------------------------------------------------
# Sources (our code + the SDL-decoupled vendor units + the CPU)
# ---------------------------------------------------------------------------
SRC_DIR    := src
VENDOR_DIR := vendor

SOURCES := \
	$(SRC_DIR)/libretro.cpp \
	$(SRC_DIR)/palette_libretro.cpp \
	$(VENDOR_DIR)/blitter.cpp \
	$(VENDOR_DIR)/audio_coprocessor.cpp \
	$(VENDOR_DIR)/emulator_config.cpp \
	$(VENDOR_DIR)/timekeeper.cpp \
	$(VENDOR_DIR)/mos6502/mos6502.cpp

# NOTE: vendor/palette.cpp is intentionally NOT compiled — src/palette_libretro.cpp
# replaces it (SDL-free XRGB8888). vendor/game_config.cpp pulls in toml/devtools
# and is part of the standalone shell, not the core, so it is omitted too.

DEFINES := \
	-DLIBRETRO_BUILD \
	-DCPU_6502_STATIC \
	-DCPU_6502_USE_LOCAL_HEADER \
	-DCMOS_INDIRECT_JMP_FIX

INCLUDES := -I$(SRC_DIR) -I$(VENDOR_DIR) -I$(VENDOR_DIR)/mos6502

CXXSTD   := -std=c++20
WARN     := -Wall -Wno-unused-parameter
OPTIM    := -O2

# Escape hatches for CI / cross-compiles (e.g. macOS x86_64 cross on an arm64
# runner passes CXXFLAGS_EXTRA=-arch x86_64 LDFLAGS_EXTRA=-arch x86_64). Appended
# to every compile/link below; empty by default so normal builds are unaffected.
CXXFLAGS_EXTRA ?=
LDFLAGS_EXTRA  ?=

# ---------------------------------------------------------------------------
# Platform selection
# ---------------------------------------------------------------------------
platform ?= native
ifeq ($(platform),wasm)
  platform := emscripten
endif

# Default the native compiler to clang++ (per the port spec) unless the user
# explicitly overrides CXX on the command line. `make`'s built-in default for
# CXX is g++, so a plain `?=` would never win; detect that and pin clang++.
ifeq ($(origin CXX),default)
  CXX := clang++
endif

ifeq ($(platform),retroemu)
  # ---- retroemu host (Emscripten MODULARIZE+ES6 factory) ----
  # retroemu (and its retroterm launcher) load a core as an ES6-module factory
  # named create_<core> that exposes the libretro C ABI + a few runtime helpers
  # (addFunction/HEAPU8/...). This matches retroemu/scripts/build-core.sh's flags.
  # Emits BOTH gametank_libretro.js (glue) and .wasm.
  CXX        := emcc
  OBJEXT     := .em.o
  OUTPUT     := $(TARGET).js
  EM_EXPORTS := '["_retro_api_version","_retro_init","_retro_deinit","_retro_set_environment","_retro_set_video_refresh","_retro_set_audio_sample","_retro_set_audio_sample_batch","_retro_set_input_poll","_retro_set_input_state","_retro_get_system_info","_retro_get_system_av_info","_retro_load_game","_retro_unload_game","_retro_run","_retro_reset","_retro_serialize_size","_retro_serialize","_retro_unserialize","_retro_get_memory_data","_retro_get_memory_size","_retro_get_region","_retro_set_controller_port_device","_gt_prof_config","_gt_prof_floor","_gt_watch_config","_malloc","_free"]'
  EM_RUNTIME := '["ccall","cwrap","addFunction","removeFunction","HEAPU8","HEAPU16","HEAPU32","HEAP16","HEAP32","HEAPF32","UTF8ToString","stringToUTF8","lengthBytesUTF8","getValue","setValue"]'
  EMFLAGS    := -s WASM=1 -s MODULARIZE=1 -s EXPORT_ES6=1 -s EXPORT_NAME=create_gametank \
                -s ENVIRONMENT=node -s ALLOW_MEMORY_GROWTH=1 -s INITIAL_MEMORY=33554432 \
                -s MAXIMUM_MEMORY=268435456 -s ALLOW_TABLE_GROWTH=1 -s INVOKE_RUN=0 \
                -s EXPORTED_FUNCTIONS=$(EM_EXPORTS) -s EXPORTED_RUNTIME_METHODS=$(EM_RUNTIME)
  CXXFLAGS   := $(CXXSTD) $(OPTIM) $(WARN) $(DEFINES) $(INCLUDES) -fPIC
  LDFLAGS    := $(OPTIM) $(EMFLAGS)
  LINK       := $(CXX)
else ifeq ($(platform),emscripten)
  # ---- Emscripten / WASM (bare side module) ----
  # Modern emcc emits real wasm object files from -c (not LLVM bitcode). Use a
  # platform-tagged object extension (.em.o) so native (clang) and emscripten
  # object files NEVER collide — building one target must not leave stale objects
  # the other target's linker would mis-detect (wasm-ld: "unknown file type").
  # This is a bare SIDE_MODULE .wasm (no glue JS); for retroemu use platform=retroemu.
  CXX        := emcc
  OBJEXT     := .em.o
  OUTPUT     := $(TARGET).wasm
  EMFLAGS    := -s WASM=1 -s SIDE_MODULE=1
  CXXFLAGS   := $(CXXSTD) $(OPTIM) $(WARN) $(DEFINES) $(INCLUDES) -fPIC
  LDFLAGS    := $(OPTIM) $(EMFLAGS)
  LINK       := $(CXX)
else ifeq ($(platform),android)
  # ---- Android (NDK cross-compile, arm64-v8a) ----
  # Driven by env: NDK (or ANDROID_NDK_ROOT/ANDROID_NDK_HOME) points at the NDK,
  # API sets the minSdk (default 24). The NDK ships its own clang++ with a baked-in
  # sysroot per target triple; we use the standalone toolchain wrapper.
  NDK        ?= $(or $(ANDROID_NDK_ROOT),$(ANDROID_NDK_HOME),$(ANDROID_NDK_LATEST_HOME))
  API        ?= 24
  ANDROID_ABI ?= arm64-v8a
  TRIPLE     := aarch64-linux-android
  HOSTTAG    := linux-x86_64
  TCBIN      := $(NDK)/toolchains/llvm/prebuilt/$(HOSTTAG)/bin
  CXX        := $(TCBIN)/$(TRIPLE)$(API)-clang++
  OBJEXT     := .android.o
  OUTPUT     := $(TARGET).so
  CXXFLAGS   := $(CXXSTD) $(OPTIM) $(WARN) $(DEFINES) $(INCLUDES) -fPIC
  LDFLAGS    := -shared -fPIC -static-libstdc++
  LINK       := $(CXX)
else ifeq ($(shell uname -s),Darwin)
  # ---- macOS native ----
  CXX        ?= clang++
  OBJEXT     := .o
  OUTPUT     := $(TARGET).dylib
  CXXFLAGS   := $(CXXSTD) $(OPTIM) $(WARN) $(DEFINES) $(INCLUDES) -fPIC
  LDFLAGS    := -dynamiclib
  LINK       := $(CXX)
else ifeq ($(OS),Windows_NT)
  # ---- Windows native (mingw/clang) ----
  CXX        ?= clang++
  OBJEXT     := .o
  OUTPUT     := $(TARGET).dll
  CXXFLAGS   := $(CXXSTD) $(OPTIM) $(WARN) $(DEFINES) $(INCLUDES)
  LDFLAGS    := -shared
  LINK       := $(CXX)
else
  # ---- Linux / BSD native ----
  CXX        ?= clang++
  OBJEXT     := .o
  OUTPUT     := $(TARGET).so
  CXXFLAGS   := $(CXXSTD) $(OPTIM) $(WARN) $(DEFINES) $(INCLUDES) -fPIC
  LDFLAGS    := -shared -fPIC
  LINK       := $(CXX)
endif

OBJECTS := $(SOURCES:.cpp=$(OBJEXT))

# ---------------------------------------------------------------------------
# Rules
# ---------------------------------------------------------------------------
.PHONY: all clean test

all: $(OUTPUT)

$(OUTPUT): $(OBJECTS)
	$(LINK) $(LDFLAGS) $(LDFLAGS_EXTRA) -o $@ $(OBJECTS)
	@echo "Built $@"

# One rule per object flavor so native (.o), emscripten (.em.o), and android
# (.android.o) objects are produced by their own compiler and never reused across
# targets. The more-specific .em.o / .android.o patterns are listed first so make
# prefers them over the bare %.o for those names.
%.em.o: %.cpp
	$(CXX) $(CXXFLAGS) $(CXXFLAGS_EXTRA) -c $< -o $@

%.android.o: %.cpp
	$(CXX) $(CXXFLAGS) $(CXXFLAGS_EXTRA) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(CXXFLAGS_EXTRA) -c $< -o $@

clean:
	rm -f $(SOURCES:.cpp=.o) $(SOURCES:.cpp=.em.o) $(SOURCES:.cpp=.android.o) $(SOURCES:.cpp=.bc)
	rm -f $(TARGET).so $(TARGET).dylib $(TARGET).dll $(TARGET).js $(TARGET).wasm
	rm -f test/harness test/harness.o

# Native-only dlopen smoke test (see test/harness.c).
# macOS keeps dlopen/dlsym in libc — no -ldl there (it errors). Linux needs -ldl.
ifeq ($(shell uname -s),Darwin)
  DL_LIB :=
else
  DL_LIB := -ldl
endif
test: all test/harness
	@echo "Run: ./test/harness <rom.gtr>"

test/harness: test/harness.c
	$(CC) -O2 -o $@ $< $(DL_LIB)
