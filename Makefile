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

ifeq ($(platform),emscripten)
  # ---- Emscripten / WASM ----
  # Modern emcc emits real wasm object files from -c (not LLVM bitcode). Use a
  # platform-tagged object extension (.em.o) so native (clang) and emscripten
  # object files NEVER collide — building one target must not leave stale objects
  # the other target's linker would mis-detect (wasm-ld: "unknown file type").
  CXX        := emcc
  OBJEXT     := .em.o
  # SIDE_MODULE emits a bare wasm binary to -o (loadable the way retroemu loads
  # its WASM cores); name it .wasm so it is obviously the module, not glue JS.
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
