// LIBRETRO: vendor/sdl_shim.h
//
// SDL replacement shim for the libretro build of the GameTank core.
//
// Upstream's reusable hardware units (blitter.cpp, palette.cpp,
// audio_coprocessor.cpp, emulator_config.cpp, system_state.h ...) all pull in
// "SDL_inc.h" purely for a handful of integer typedefs, a couple of audio
// format enums, and a lightweight 32-bit surface abstraction used by the
// blitter's put_pixel32()/get_pixel32() helpers.
//
// None of the real SDL runtime is needed once the window / main-loop / audio
// device plumbing moves into src/libretro.cpp. This header provides:
//   * the integer typedefs (Uint8/Uint16/Uint32/Sint16 ...)
//   * a minimal SDL_Surface struct that owns a plain uint32_t* pixel buffer,
//     so put_pixel32()/get_pixel32() and Blitter write straight into an
//     XRGB8888 framebuffer with ZERO edits to blitter.cpp
//   * no-op stubs for SDL_LockAudioDevice / SDL_SetColorKey / SDL_MapRGB /
//     SDL_OpenAudio ... and the audio format / device-id types
//
// It is only used when LIBRETRO_BUILD is defined (see SDL_inc.h).
#pragma once

#include <cstdint>
#include <cstddef>

// ---------------------------------------------------------------------------
// Integer typedefs (the SDL_stdinc.h subset the vendored code relies on)
// ---------------------------------------------------------------------------
typedef uint8_t  Uint8;
typedef int8_t   Sint8;
typedef uint16_t Uint16;
typedef int16_t  Sint16;
typedef uint32_t Uint32;
typedef int32_t  Sint32;
typedef uint64_t Uint64;
typedef int64_t  Sint64;

// ---------------------------------------------------------------------------
// Minimal 32-bit surface.
//
// The blitter / VDMA_Write path does:
//     put_pixel32(surface, x, y, Palette::ConvertColor(surface, index));
// where put_pixel32 just indexes surface->pixels as a Uint32*. We give the
// surface a real uint32_t* buffer (allocated by src/libretro.cpp) so the
// pixels land directly in an XRGB8888 framebuffer that video_refresh_cb can
// present. `format` is carried only so Palette::ConvertColor keeps its
// signature; the libretro reimplementation ignores it.
// ---------------------------------------------------------------------------
struct SDL_PixelFormat; // opaque; unused by the libretro palette path

typedef struct SDL_Surface {
    void   *pixels;            // really uint32_t* (XRGB8888), w*h entries
    int     w;
    int     h;
    int     pitch;             // bytes per row (= w * 4)
    struct SDL_PixelFormat *format; // unused in LIBRETRO_BUILD
} SDL_Surface;

// ---------------------------------------------------------------------------
// Audio format / device id types (kept so the ACPState struct + helpers in
// audio_coprocessor.{h,cpp} still compile; values are never sent to hardware).
// ---------------------------------------------------------------------------
typedef uint16_t SDL_AudioFormat;
typedef uint32_t SDL_AudioDeviceID;

#define AUDIO_U8      0x0008
#define AUDIO_S8      0x8008
#define AUDIO_U16LSB  0x0010
#define AUDIO_S16LSB  0x8010
#define AUDIO_U16MSB  0x1010
#define AUDIO_S16MSB  0x9010
#define AUDIO_U16     AUDIO_U16LSB
#define AUDIO_S16     AUDIO_S16LSB
#define AUDIO_S16SYS  AUDIO_S16LSB
#define AUDIO_S32LSB  0x8020
#define AUDIO_S32MSB  0x9020
#define AUDIO_F32LSB  0x8120
#define AUDIO_F32MSB  0x9120

// SDL_INIT_* / renderer flags referenced by emulator_config.{h,cpp}
#define SDL_INIT_AUDIO          0x00000010u
#define SDL_INIT_VIDEO          0x00000020u
#define SDL_INIT_GAMECONTROLLER 0x00002000u
#define SDL_RENDERER_SOFTWARE   0x00000001u
#define SDL_RENDERER_ACCELERATED 0x00000002u

#define SDL_TRUE  1
#define SDL_FALSE 0

// ---------------------------------------------------------------------------
// No-op / trivial stubs for the SDL calls that survive the decoupling.
// audio_coprocessor.cpp still references SDL_LockAudioDevice /
// SDL_UnlockAudioDevice (now harmless no-ops since we are single-threaded and
// drive fill_audio() directly), and the StartAudio() path which is never
// reached when EmulatorConfig::noSound is set true by the libretro core.
// ---------------------------------------------------------------------------
static inline void SDL_LockAudioDevice(SDL_AudioDeviceID)   {}
static inline void SDL_UnlockAudioDevice(SDL_AudioDeviceID) {}
static inline void SDL_PauseAudioDevice(SDL_AudioDeviceID, int) {}
static inline int  SDL_InitSubSystem(uint32_t) { return 0; }
static inline const char *SDL_GetError(void) { return ""; }

// Color-key / pixel-format helpers used only by the blitter colorkey path and
// the (now reimplemented) palette converter. Transparency is handled by the
// blitter's own colorbus!=0 test, so these are inert.
static inline int    SDL_SetColorKey(SDL_Surface *, int, Uint32) { return 0; }
static inline Uint32 SDL_MapRGB(struct SDL_PixelFormat *, Uint8 r, Uint8 g, Uint8 b) {
    return 0xFF000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
}

// SDL_OpenAudio / SDL_OpenAudioDevice are never invoked (noSound==true), but
// keep a declaration-free path: audio_coprocessor.cpp's StartAudio() uses
// SDL_AudioSpec; provide a tiny stand-in so it compiles if ever referenced.
typedef struct SDL_AudioSpec {
    int             freq;
    SDL_AudioFormat format;
    Uint8           channels;
    Uint16          samples;
    void          (*callback)(void *, Uint8 *, int);
    void           *userdata;
} SDL_AudioSpec;

static inline SDL_AudioDeviceID SDL_OpenAudioDevice(
    const char *, int, const SDL_AudioSpec *, SDL_AudioSpec *, int) { return 0; }
static inline int SDL_OpenAudio(SDL_AudioSpec *, SDL_AudioSpec *) { return -1; }
