# GameTank → libretro port notes

The integration boundary between the vendored upstream emulation core and the
libretro API. Read this before touching `src/libretro.cpp`.

## Source split

- `vendor/` — reusable hardware units copied verbatim from upstream
  (`mos6502/`, `blitter`, `audio_coprocessor`, `palette`, `timekeeper`,
  `system_state`, `emulator_config`, `game_config`, `gametank_palette`).
  See `vendor/PROVENANCE.txt` for the exact upstream commit.
- `src/libretro.cpp` — **our** code. Reimplements the bus + frame loop that
  upstream tangles into `gte.cpp` globals, minus SDL/imgui/devtools.
- `src/libretro.h` — vendored canonical libretro API header.

## The SDL decoupling problem (the crux of the port)

Upstream leans on SDL in three core places. The port must sever each:

1. **Framebuffer.** Blitter and `VDMA_Write` write pixels into `SDL_Surface*
   vRAM_Surface` / `gRAM_Surface` via `put_pixel32(...Palette::ConvertColor...)`.
   - Replace surfaces with plain `uint32_t fb[128*256]` (VRAM is double-height:
     two 128×128 pages, page-flipped by `DMA_VID_OUT_PAGE_BIT`). GRAM is
     128×(128*32) sprite memory.
   - `Palette::ConvertColor` currently needs an `SDL_Surface*` for its pixel
     format. Reimplement as a direct `uint8_t index -> 0xXXRRGGBB` lookup using
     `gametank_palette.h` (the palette tables are plain RGB data). Provide a
     drop-in `uint32_t gt_palette_xrgb8888(uint8_t index)`.
   - `SDL_SetColorKey` (transparency, DMA_TRANSPARENCY_BIT) → track a bool; the
     blitter already special-cases transparent copies, so colorkey only matters
     for the *displayed* present, which we composite ourselves.

2. **Audio.** `AudioCoprocessor::fill_audio(udata, stream, len)` is ALREADY a
   pull callback producing S16 mono samples. Perfect for libretro.
   - Per `retro_run`: compute samples-for-this-frame (`samples_per_frame` ≈
     `315000000/(88*256*60)` ≈ 233 @ the GameTank's native ~14.9 kHz DAC rate),
     call `fill_audio(&acpState, buf, n*2)`, duplicate mono→stereo, and either
     (a) set `RETRO_PIXEL`-side AV sample_rate to the real DAC rate and pass
     through, or (b) resample to 44100/48000. Start with (a): report the real
     `sample_rate` in `retro_get_system_av_info` and let the frontend resample.
   - Drop `SDL_LockAudioDevice` / `SDL_UnlockAudioDevice` — single-threaded here,
     they become no-ops. `SDL_OpenAudio`/`StartAudio` → not called; we drive
     `fill_audio` directly.
   - `state.device`, `SDL_AudioDeviceID`, `SDL_AudioFormat` fields stay in the
     struct but go unused; keep them to avoid editing the vendored header, or
     `#ifdef LIBRETRO_BUILD` them out.

3. **Window / main loop / timing.** `gte.cpp`'s `mainloop()` +
   `SDL_Delay`/time-scaling self-pacing is replaced entirely by `retro_run()`.
   One `retro_run` = one 60 Hz frame = run `timekeeper.cycles_per_vsync` main-CPU
   cycles, then if `DMA_VSYNC_NMI_BIT` fire `cpu_core->NMI()`, then present the
   active framebuffer page. NO `SDL_Delay`, NO time_scaling — the frontend paces.

## CPU wiring (from gte.cpp:1363)

```cpp
cpu_core = new mos6502(MemoryRead, MemoryWrite, CPUStopped, MemorySync);
```

`mos6502` takes C function pointers (`BusRead = uint8_t(*)(uint16_t)`,
`BusWrite = void(*)(uint16_t,uint8_t)`). Because they're plain function pointers,
the bus handlers must reach state via **file-scope globals** in `libretro.cpp`
(same pattern as upstream). Port these bus functions from `gte.cpp` ~verbatim,
stripping the devtools calls (`Breakpoints::checkBreakpoint`, `profiler.*`,
`Disassembler::Decode`, `SourceMap`) — replace `MemorySync` with a plain
`MemoryReadResolve(addr,false)` (no profiling/breakpoints) or just pass `NULL` for
the sync arg.

Bus functions to port from `gte.cpp`: `VDMA_Write`, `VDMA_Read`,
`UpdateFlashShiftRegister`, `MemoryRead_Flash2M`, `MemoryRead_Unknown`, `GetRAM`,
`MemoryReadResolve`, `MemoryRead`, `MemoryWrite`. Globals they need:
`SystemState system_state`, `CartridgeState cartridge_state`, `Blitter* blitter`,
`AudioCoprocessor* soundcard`, `JoystickAdapter* joysticks` (or a minimal input
shim — see below), `RomType loadedRomType`, `mos6502* cpu_core`.

`FULL_RAM_ADDRESS(x)` macro and `open_bus()` live in gte.cpp — copy them.

## Input

Upstream reads controllers at `$2008`/`$2009` through `JoystickAdapter` which is
SDL-gamepad bound. For libretro, DON'T vendor the SDL joystick adapter — write a
tiny shim: two `uint8_t` latches updated each `retro_run` from
`input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, id)`. The GameTank controller is an
NES-style pad; map: Up/Down/Left/Right/B(A)/C(B)/Start/Select. Confirm the exact
active-low bit order and the `$2009` second-controller / latch semantics by
reading upstream `joystick_adapter.cpp` `read()` — replicate its bit layout, just
fed from libretro instead of SDL.

## ROM load (port of LoadRomFile, gte.cpp ~599)

`.gtr` size → RomType:
- 8192 → EEPROM8K
- 32768 → EEPROM32K
- 2097152 → FLASH2M (or FLASH2M_RAM32K if the game DB / a heuristic says so;
  upstream uses `game_config` / a known-hash table — start with FLASH2M, expose
  a core option to force RAM32K)
- else → UNKNOWN

Allocate `cartridge_state.rom` (2MB for flash, exact size otherwise), memcpy the
loaded data, set `cartridge_state.size`, init `bank_mask`/`bank_shifter`, reset
the CPU (read reset vector at `$FFFC`).

## Save / state

- `RETRO_MEMORY_SAVE_RAM` → `cartridge_state.save_ram[CARTRAMSIZE]` for
  FLASH2M_RAM32K carts (battery SRAM). Return NULL/0 for non-SRAM carts.
- `retro_serialize` / `retro_unserialize`: the live state is `SystemState` +
  `CartridgeState` (minus the `rom*` pointer — serialize the modifiable flash
  contents separately or mark flash carts non-deterministic for now) + the two
  CPU register sets (main + audio `mos6502`) + blitter counters + timekeeper
  cycle accumulators. Define a versioned blob; simplest first cut: dump
  `system_state`, both cpu register structs, `acpState.ram`, `cartridge_state`
  scalars, and (for FLASH2M) the full 2MB rom if `write_mode` was ever used.
  Start by getting non-flash-write games deterministic.

## Memory descriptors (for cheats + romdev later)

Expose via `retro_get_memory_data`/`_size` and `RETRO_ENVIRONMENT_SET_MEMORY_MAPS`:
- system RAM (`system_state.ram`, 32 KB)
- VRAM (`system_state.vram`)
- GRAM / sprite RAM (`system_state.gram`)
- audio RAM (`acpState.ram`, 4 KB)
This is what makes the core valuable to romdev's `memory`/`inspectSprites` tools.

## Build targets

`Makefile`:
- native: `clang++ -shared -fPIC` → `gametank_libretro.so` (`.dylib` macOS,
  `.dll` Windows), `-std=c++20`, defines `-D CPU_6502_STATIC
  -D CPU_6502_USE_LOCAL_HEADER -D CMOS_INDIRECT_JMP_FIX` (required by mos6502,
  from upstream Makefile line 110) and `-D LIBRETRO_BUILD`.
- emscripten (`platform=emscripten` or `wasm`): `emcc -O2 -s SIDE_MODULE=1` (or
  the retroemu-compatible MODULARIZE pattern — match how retroemu loads its
  cores). No `USE_SDL` (we removed SDL). Output `gametank_libretro.{js,wasm}`.

Compile units: `src/libretro.cpp` + everything in `vendor/` EXCEPT files that
still hard-require SDL after the decoupling. After patching `palette.cpp` and
`audio_coprocessor.cpp` to not need a live SDL surface/device, only the
`<cstdint>`/`Uint32` typedefs remain — provide a tiny `vendor/sdl_shim.h` that
typedefs `Uint8/Uint16/Uint32/Sint16` and stubs `SDL_AudioFormat`,
`SDL_AudioDeviceID`, and the `SDL_LockAudioDevice`/`SDL_SetColorKey` macros to
no-ops, then point `SDL_inc.h` at it under `-D LIBRETRO_BUILD`. This avoids
forking the vendored .cpp files heavily.

## Open items / known risks

- **Audio sample rate.** GameTank DAC is ~14.9 kHz, non-standard. Reporting it
  raw is cleanest; verify RetroArch resamples acceptably. Fallback: integer
  upsample in the core.
- **Flash write persistence.** `MemoryWrite` can erase/program the 2MB flash
  (self-modifying carts / save systems). Route those writes to `SAVE_RAM` so the
  frontend persists them, OR snapshot dirty flash sectors into the serialize blob.
- **FLASH2M vs FLASH2M_RAM32K detection.** Upstream uses a per-game config; we
  need a core option or a small hash table.
- **Cycle exactness of audio CPU vs main CPU.** Upstream runs the audio CPU
  inside `fill_audio` and on `ACP_NMI` register writes. Preserve that ordering.

### Implementation status (ported 2026-06-29)

**Builds.** `make` → `gametank_libretro.so` (Linux, clang++ -std=c++20, the four
mos6502 + LIBRETRO_BUILD defines). `make platform=emscripten` → a bare
SIDE_MODULE `gametank_libretro.wasm`. macOS `.dylib` / Windows `.dll` branches
present but unverified here. All 25 `retro_*` entry points export with C
linkage; the only undefined symbols are libc/libstdc++ (zero SDL refs).
Verified against the upstream `roms/` set: EEPROM8K (hello), EEPROM32K (tetris),
and FLASH2M (colortest/badapple) all load, render real palette colors into the
128×128 XRGB8888 framebuffer, emit 233 audio samples/frame, and round-trip
serialize. `test/harness.c` is the dlopen smoke test (`make test`).

**SDL decoupling — final shape.** `vendor/sdl_shim.h` gives `SDL_Surface` a real
`uint32_t*` pixel buffer, so `put_pixel32`/`get_pixel32` and **all of
`blitter.cpp` compile UNEDITED** — the only blitter-path change is the palette
converter. `vendor/palette.cpp` is **not compiled**; `src/palette_libretro.cpp`
replaces it (SDL-free `Palette::ConvertColor` + the standalone
`gt_palette_xrgb8888`). `vendor/SDL_inc.h` routes to the shim under
`LIBRETRO_BUILD`. `vendor/game_config.cpp` is **not compiled** (toml + devtools;
part of the standalone shell, not the core).

**Resolved open items:**
- *FLASH2M_RAM32K detection* — kept upstream's `"SAVE"` magic at `0x1FFFF0`,
  plus a `gametank_force_ram32k` core option to force it. No hash table needed.
- *Audio rate* — report the real DAC rate `315000000/(88*256)` ≈ 13967.6 Hz in
  `retro_get_system_av_info`; frontend resamples. `samples_per_frame` (233) comes
  from the ACP rate register; mono is duplicated to stereo S16 per frame.

**Resolved in state v2 (post-v0.1.0):**
- **Save-state IRQ state** — `mos6502.h` gained marked LIBRETRO accessors
  (`LR_GetIrqTimer/LR_GetIrqLine/LR_SetIrqState`); the blob now captures each
  CPU's private `irq_timer` + `irq_line`. `irq_gate` is a pointer into the
  blitter/ACP and is re-wired by those devices on load, so it is intentionally
  not serialized. (This is the project's first marked vendor delta — re-apply it
  after any `scripts/update-vendor.sh` bump.)
- **Flash write persistence** — a `flash_dirty` flag is set on any FLASH2M
  program/erase. When set, `retro_serialize` appends the full 2 MB flash image to
  the blob (and `retro_serialize_size` accounts for it); restored on load. The
  common read-only cart pays nothing — the tail only exists once a cart has
  self-modified. FLASH2M_RAM32K battery SRAM is still also exposed via
  `RETRO_MEMORY_SAVE_RAM` for cross-session persistence.
- **Memory descriptors** — `retro_load_game` now calls
  `RETRO_ENVIRONMENT_SET_MEMORY_MAPS` with four descriptors: RAM (0x000000),
  VRAM (0x010000), GRAM/sprite RAM (0x100000), ACP audio RAM (0x200000). So
  cheats + romdev's `memory`/`inspectSprites`/audio tools see all four regions,
  not just the three reachable via `retro_get_memory_data`.

**Not a bug (noted for completeness):**
- **RNG seed** — `retro_init` seeds `srand(0x6502)` *on purpose*: `open_bus()` and
  the uninitialized-RAM fill are then deterministic across runs, which is the
  correct behavior for a reproducible emulator (real hardware boots with
  arbitrary RAM, but determinism makes save states / testing reliable). Nothing
  to change.
