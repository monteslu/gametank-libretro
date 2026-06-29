// LIBRETRO: test/harness.c
//
// Minimal dlopen smoke test for gametank_libretro.so. Not a full frontend —
// it wires the bare callbacks, loads a .gtr, runs a handful of frames, and
// prints a per-frame framebuffer checksum + a final audio/serialize sanity
// check. Build with `make test`, run `./test/harness <rom.gtr>`.
//
//   cc -O2 -o test/harness test/harness.c -ldl
//
// It deliberately depends only on the libretro ABI (no libretro.h needed) so
// it stays decoupled from the core's headers.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <dlfcn.h>

// --- libretro ABI subset ---------------------------------------------------
#define RETRO_DEVICE_JOYPAD 1
#define RETRO_MEMORY_SYSTEM_RAM 2
#define RETRO_ENVIRONMENT_SET_PIXEL_FORMAT 10
#define RETRO_ENVIRONMENT_GET_VARIABLE 15
#define RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE 17
#define RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME 18

struct retro_game_info {
    const char *path;
    const void *data;
    size_t      size;
    const char *meta;
};

typedef bool   (*environ_t)(unsigned, void*);
typedef void   (*video_t)(const void*, unsigned, unsigned, size_t);
typedef void   (*audio_sample_t)(int16_t, int16_t);
typedef size_t (*audio_batch_t)(const int16_t*, size_t);
typedef void   (*poll_t)(void);
typedef int16_t(*input_t)(unsigned, unsigned, unsigned, unsigned);

// --- callback implementations ----------------------------------------------
static uint32_t last_video_checksum = 0;
static unsigned last_w = 0, last_h = 0;
static uint64_t total_audio_frames = 0;
static uint32_t last_frame[256*256];   // saved copy of the most recent framebuffer
static unsigned distinct_colors = 0;    // # of distinct pixel values in last frame

static bool cb_environ(unsigned cmd, void *data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            return *(int*)data == 1; // RETRO_PIXEL_FORMAT_XRGB8888 == 1
        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
            return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            *(bool*)data = false; return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE:
            return false; // use defaults
        default:
            return false; // ignore the rest (input descriptors, variables, ...)
    }
}

static void cb_video(const void *data, unsigned w, unsigned h, size_t pitch) {
    last_w = w; last_h = h;
    if (!data) return;
    uint32_t sum = 2166136261u;  // FNV-ish
    const uint8_t *row = (const uint8_t*)data;
    for (unsigned y = 0; y < h && y < 256; y++) {
        const uint32_t *px = (const uint32_t*)(row + y * pitch);
        for (unsigned x = 0; x < w && x < 256; x++) {
            sum ^= px[x]; sum *= 16777619u;
            last_frame[y*256 + x] = px[x];
        }
    }
    last_video_checksum = sum;
    // Count distinct colors (cheap O(n^2) over a small palette is fine at 128x128).
    uint32_t seen[256]; unsigned ns = 0;
    for (unsigned y = 0; y < h && y < 256; y++)
        for (unsigned x = 0; x < w && x < 256; x++) {
            uint32_t c = last_frame[y*256 + x]; unsigned j;
            for (j = 0; j < ns; j++) if (seen[j] == c) break;
            if (j == ns && ns < 256) seen[ns++] = c;
        }
    distinct_colors = ns;
}

// Dump the last framebuffer as a binary PPM (P6) for visual inspection.
static void dump_ppm(const char *path) {
    FILE *o = fopen(path, "wb");
    if (!o) return;
    fprintf(o, "P6\n%u %u\n255\n", last_w, last_h);
    for (unsigned y = 0; y < last_h; y++)
        for (unsigned x = 0; x < last_w; x++) {
            uint32_t c = last_frame[y*256 + x]; // XRGB8888 (0x00RRGGBB)
            unsigned char rgb[3] = { (c>>16)&0xff, (c>>8)&0xff, c&0xff };
            fwrite(rgb, 1, 3, o);
        }
    fclose(o);
}

static void   cb_audio_sample(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t cb_audio_batch(const int16_t *d, size_t frames) {
    (void)d; total_audio_frames += frames; return frames;
}
static void    cb_poll(void) {}
static int16_t cb_input(unsigned p, unsigned dev, unsigned idx, unsigned id) {
    (void)p; (void)dev; (void)idx; (void)id; return 0; // no buttons held
}

// Function-pointer typedefs for the core's exported entry points.
typedef void   (*set_env_fn)(environ_t);
typedef void   (*set_video_fn)(video_t);
typedef void   (*set_audio_fn)(audio_sample_t);
typedef void   (*set_batch_fn)(audio_batch_t);
typedef void   (*set_poll_fn)(poll_t);
typedef void   (*set_input_fn)(input_t);
typedef void   (*void_fn)(void);
typedef bool   (*load_fn)(const struct retro_game_info*);
typedef size_t (*size_fn)(void);
typedef bool   (*ser_fn)(void*, size_t);
typedef bool   (*unser_fn)(const void*, size_t);
typedef size_t (*memsz_fn)(unsigned);

static void *g_handle;
static void *load_sym(const char *name) {
    void *p = dlsym(g_handle, name);
    if (!p) { fprintf(stderr, "missing symbol %s\n", name); exit(2); }
    return p;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <rom.gtr> [frames]\n", argv[0]); return 1; }
    int frames = (argc > 2) ? atoi(argv[2]) : 10;

    const char *sopath = "./gametank_libretro.so";
    g_handle = dlopen(sopath, RTLD_NOW | RTLD_LOCAL);
    if (!g_handle) { fprintf(stderr, "dlopen %s failed: %s\n", sopath, dlerror()); return 1; }

    set_env_fn   retro_set_environment        = (set_env_fn)  load_sym("retro_set_environment");
    set_video_fn retro_set_video_refresh      = (set_video_fn)load_sym("retro_set_video_refresh");
    set_audio_fn retro_set_audio_sample       = (set_audio_fn)load_sym("retro_set_audio_sample");
    set_batch_fn retro_set_audio_sample_batch = (set_batch_fn)load_sym("retro_set_audio_sample_batch");
    set_poll_fn  retro_set_input_poll         = (set_poll_fn) load_sym("retro_set_input_poll");
    set_input_fn retro_set_input_state        = (set_input_fn)load_sym("retro_set_input_state");
    void_fn      retro_init                   = (void_fn)     load_sym("retro_init");
    void_fn      retro_deinit                 = (void_fn)     load_sym("retro_deinit");
    load_fn      retro_load_game              = (load_fn)     load_sym("retro_load_game");
    void_fn      retro_run                    = (void_fn)     load_sym("retro_run");
    size_fn      retro_serialize_size         = (size_fn)     load_sym("retro_serialize_size");
    ser_fn       retro_serialize              = (ser_fn)      load_sym("retro_serialize");
    unser_fn     retro_unserialize            = (unser_fn)    load_sym("retro_unserialize");
    memsz_fn     retro_get_memory_size        = (memsz_fn)    load_sym("retro_get_memory_size");

    retro_set_environment(cb_environ);
    retro_set_video_refresh(cb_video);
    retro_set_audio_sample(cb_audio_sample);
    retro_set_audio_sample_batch(cb_audio_batch);
    retro_set_input_poll(cb_poll);
    retro_set_input_state(cb_input);

    retro_init();

    // Load ROM file into memory.
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t*)malloc(sz);
    if (fread(buf, 1, sz, f) != (size_t)sz) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);

    struct retro_game_info gi = { argv[1], buf, (size_t)sz, NULL };
    if (!retro_load_game(&gi)) { fprintf(stderr, "retro_load_game failed\n"); return 3; }
    printf("loaded %s (%ld bytes), system RAM = %zu\n",
           argv[1], sz, retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM));

    for (int i = 0; i < frames; i++) {
        retro_run();
        printf("frame %3d: %ux%u fb_checksum=0x%08x colors=%u audio_frames=%llu\n",
               i, last_w, last_h, last_video_checksum, distinct_colors,
               (unsigned long long)total_audio_frames);
    }

    // Dump the final frame for visual inspection if a 3rd arg is given.
    if (argc > 3) { dump_ppm(argv[3]); printf("wrote %s (%ux%u)\n", argv[3], last_w, last_h); }

    // Serialize round-trip sanity.
    size_t ss = retro_serialize_size();
    void *state = malloc(ss);
    bool ok = retro_serialize(state, ss);
    bool ok2 = ok && retro_unserialize(state, ss);
    printf("serialize: size=%zu serialize=%s unserialize=%s\n",
           ss, ok ? "ok" : "FAIL", ok2 ? "ok" : "FAIL");
    free(state);

    retro_deinit();
    free(buf);
    dlclose(g_handle);
    return (ok && ok2) ? 0 : 4;
}
