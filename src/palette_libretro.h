// LIBRETRO: src/palette_libretro.h
// Declaration for the SDL-free XRGB8888 palette lookup (palette_libretro.cpp).
#pragma once
#include <cstdint>

// index -> 0xFFRRGGBB using the active sub-palette (palette_select).
uint32_t gt_palette_xrgb8888(uint8_t index);
