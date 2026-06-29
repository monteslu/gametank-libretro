// LIBRETRO: vendor/SDL_inc.h
//
// Copied/patched from upstream src/SDL_inc.h. Under the libretro build
// (LIBRETRO_BUILD) we have no real SDL: pull in the lightweight typedef/stub
// shim instead of the SDL2 headers. Outside that define this falls back to the
// original upstream behavior so the file stays a drop-in.
#pragma once

#ifdef LIBRETRO_BUILD
#include "sdl_shim.h"
#else
#ifdef _WIN32
#include <SDL.h>
#elif __APPLE__
#include "SDL.h"
#else
#include <SDL2/SDL.h>
#endif
#endif
