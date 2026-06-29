# gametank-libretro

A [libretro](https://www.libretro.com/) core for the **GameTank** — the open-source
8-bit console designed by [Clyde Shaffer](https://clydeshaffer.com/gametank/).
Runs `.gtr` cartridges inside any libretro frontend: RetroArch, RetroDeck,
Batocera, Knulli, and retro handhelds, plus monteslu's own `retroemu` host.

> Core emulation is ported from the upstream
> [GameTankEmulator](https://github.com/clydeshaffer/GameTankEmulator)
> (MIT, © 2020 Clyde Shaffer). This project wraps the reusable hardware-emulation
> units (CPU, blitter, audio coprocessor, bus) behind the libretro API in place of
> the standalone SDL2 application shell.

## The GameTank, briefly

A clean-sheet 8-bit console built from basic logic + RAM chips — no FPGAs, no
microcontrollers.

| Subsystem | Detail |
|---|---|
| **Main CPU** | WDC 65C02 @ 3.5 MHz |
| **Audio CPU** | second 65C02 @ ~14 MHz, 4 KB dual-port RAM → 8-bit DAC (~14 kHz) |
| **Video** | 128×128 framebuffer (≈128×100 visible on TV), 200 colors |
| **Blitter** | DMA copies from Sprite RAM → framebuffer, ~3.5 Mpix/s, transparency |
| **RAM** | 32 KB system RAM, banked 8 KB; 512 KB Sprite/Graphics RAM |
| **VIA** | 6522 — timer IRQs + SPI cart bank control |
| **Cartridge** | `.gtr`, 8 KB / 32 KB / 2 MB flash (some with 32 KB battery SRAM) |

## libretro mapping

| GameTank | libretro |
|---|---|
| `vRAM_Surface` framebuffer (page-flipped by `DMA_VID_OUT_PAGE_BIT`) | `video_refresh_cb`, `RETRO_PIXEL_FORMAT_XRGB8888`, 128×128 |
| audio coprocessor DAC stream | `audio_sample_batch_cb`, stereo S16 (mono duplicated), pulled per frame |
| controller ports `$2008`/`$2009` | `RETRO_DEVICE_JOYPAD` ×2 |
| `cycles_per_vsync` run + vsync NMI | one `retro_run()` = one 60 Hz frame |
| `SystemState` + `CartridgeState` POD structs | `retro_serialize` / `retro_unserialize` |
| Flash2M battery SRAM (`save_ram`) | `RETRO_MEMORY_SAVE_RAM` |
| system RAM / VRAM / GRAM | `retro_get_memory_data` regions (for cheats / romdev) |

### What the core drops vs. upstream

The standalone app's SDL window, imgui devtools (profiler, memory browser, VRAM
viewer, stepper, patcher), `tinyfd` file dialogs, `whereami`, on-disk NVRAM/flash
files, and the manual time-scaling loop are all removed — the frontend owns the
window, timing, input, and save persistence.

## Cartridge / ROM-type detection

`.gtr` size selects the mapper (mirrors upstream `LoadRomFile`):

- **8 KB** → `EEPROM8K` (mirrored into `$8000-$FFFF`)
- **32 KB** → `EEPROM32K`
- **2 MB** → `FLASH2M` (SPI-banked via VIA) or `FLASH2M_RAM32K` (+ 32 KB battery SRAM)
- other → `UNKNOWN` (wrap/align heuristic)

## Building

The core builds two ways from one source tree:

```sh
# Native shared object for desktop RetroArch (.so / .dylib / .dll)
make

# WebAssembly build for retroemu / web hosts (emcc)
emmake make platform=emscripten      # or: make platform=wasm
```

No SDL dependency — the only external is the `libretro.h` API header (vendored
under `src/`). Output: `gametank_libretro.<so|dylib|dll>` or
`gametank_libretro.{js,wasm}`.

## Install

Copy `gametank_libretro.so` to your frontend's cores dir, e.g.
`~/.config/retroarch/cores/` (or RetroDeck/Batocera equivalent), drop the matching
`gametank_libretro.info` alongside the frontend's info files, then load a `.gtr`.

## Status

**v0.1.0 — initial port.** See `doc/PORT_NOTES.md` for the integration boundary
and the open items (audio sample-rate reconciliation, flash-write persistence to
`SAVE_RAM`, memory-descriptor exposure for cheats/romdev).

## License

MIT — same as upstream GameTankEmulator. See `LICENSE`.
