// LIBRETRO: src/palette_libretro.cpp
//
// SDL-free reimplementation of the GameTank palette converter.
//
// This REPLACES vendor/palette.cpp in the libretro build (vendor/palette.cpp
// is NOT compiled). Upstream's Palette::ConvertColor() needed an SDL_Surface*
// only to discover the surface's pixel format for SDL_MapRGB(); under libretro
// every framebuffer is fixed XRGB8888, so the surface argument is ignored and
// we pack the palette RGB straight into 0xAARRGGBB (A forced opaque).
//
// `gt_palette_vals` (the raw RGB table, 4 sub-palettes * 768 bytes) lives in
// vendor/gametank_palette.h and is defined here, in exactly one translation
// unit, so there is no duplicate-symbol clash with vendor/palette.cpp.

#include "palette.h"
#include "gametank_palette.h"

// Matches upstream: which 256-entry sub-table is active.
//   PALETTE_SELECT_OLD / _CAPTURE / _SCALED / _HDMI
int palette_select = PALETTE_SELECT_CAPTURE;

// Standalone XRGB8888 lookup, per the port spec. Indexes the active sub-table
// (palette_select offset) and returns 0xFFRRGGBB.
uint32_t gt_palette_xrgb8888(uint8_t index) {
    const RGB_Color c = ((const RGB_Color*)gt_palette_vals)[index + palette_select];
    return 0xFF000000u
         | (uint32_t(c.r) << 16)
         | (uint32_t(c.g) << 8)
         |  uint32_t(c.b);
}

// Drop-in for the vendored blitter / VDMA_Write call sites:
//     put_pixel32(surface, x, y, Palette::ConvertColor(surface, index));
// The SDL_Surface* is unused (kept only to preserve the upstream signature so
// blitter.cpp needs no edits). Mirrors upstream's "never emit pure black"
// quirk: an index that resolves to 0x000000 is nudged to 0x010101 so colorkey
// transparency (black == transparent) does not eat opaque pixels.
Uint32 Palette::ConvertColor(SDL_Surface* /*target*/, uint8_t index) {
    uint32_t res = gt_palette_xrgb8888(index);
    if ((res & 0x00FFFFFFu) == 0) {
        return 0xFF010101u;
    }
    return res;
}
