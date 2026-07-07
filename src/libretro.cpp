// LIBRETRO: src/libretro.cpp
//
// The GameTank libretro core. Reimplements the bus + frame loop that upstream
// tangles into gte.cpp's globals, minus SDL / imgui / devtools. See
// doc/PORT_NOTES.md for the integration spec.
//
// Bus functions (VDMA_Read/Write, MemoryRead/Write, flash banking, ...) are
// ported ~verbatim from upstream gte.cpp with the devtools calls
// (Breakpoints / Profiler / Disassembler / SourceMap) stripped. The CPU takes
// plain C function pointers, so all bus state lives in file-scope globals here,
// same pattern as upstream.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "libretro.h"

#include "../vendor/system_state.h"
#include "../vendor/timekeeper.h"
#include "../vendor/blitter.h"
#include "../vendor/palette.h"
#include "../vendor/audio_coprocessor.h"
#include "../vendor/emulator_config.h"
#include "../vendor/mos6502/mos6502.h"
#include "palette_libretro.h"

// ===========================================================================
// GameTank constants
// ===========================================================================
static const int GT_WIDTH  = 128;
static const int GT_HEIGHT = 128;

// 65C02 main bus runs at 315000000/88 Hz; one frame = system_clock/60 cycles.
// (Timekeeper computes cycles_per_vsync from these.)

// VIA register file indices (from gte.cpp).
static const uint8_t VIA_ORB    = 0x0;
static const uint8_t VIA_ORA    = 0x1;

// VIA Port A serial pins used for the flash cartridge bank shift register.
static const uint8_t VIA_SPI_BIT_CLK  = 0b00000001;
static const uint8_t VIA_SPI_BIT_MOSI = 0b00000010;
static const uint8_t VIA_SPI_BIT_CS   = 0b00000100;

#define RAM_HIGHBITS_SHIFT 7

// ===========================================================================
// File-scope bus state (the globals the C-pointer bus handlers reach through)
// ===========================================================================
SystemState    system_state;
CartridgeState cartridge_state;
RomType        loadedRomType = RomType::UNKNOWN;

static Timekeeper        timekeeper;
static mos6502          *cpu_core   = nullptr;
static Blitter          *blitter    = nullptr;
static AudioCoprocessor *soundcard  = nullptr;

// The blitter writes pixels into this XRGB8888 framebuffer via put_pixel32().
// VRAM is double-height: two 128x128 pages, selected by DMA_VID_OUT_PAGE_BIT.
static uint32_t  vram_fb[GT_WIDTH * GT_HEIGHT * 2];
// The blitter constructor binds an SDL_Surface*& (our shim surface). It must
// outlive the blitter and wrap vram_fb.
static SDL_Surface vram_surface_obj;
static SDL_Surface *vram_surface = &vram_surface_obj;

// FULL_RAM_ADDRESS macro from gte.cpp (banked 8 KB windows into 32 KB RAM).
#define FULL_RAM_ADDRESS(x) (((system_state.banking & BANK_RAM_MASK) << RAM_HIGHBITS_SHIFT) | (x))

// ---------------------------------------------------------------------------
// Input shim. Replaces the SDL-bound JoystickAdapter. Two GameTank pads; each
// retro_run() latches the libretro button state into pad{1,2}Mask, then the
// bus read at $2008/$2009 replays upstream's active-low select/latch protocol.
// (Bit layout + read() semantics ported from joystick_adapter.{cpp,h}.)
// ---------------------------------------------------------------------------
namespace GTButtons {
    enum {
        UP    = 0b0000100000001000,
        DOWN  = 0b0000010000000100,
        LEFT  = 0b0000001000000000,
        RIGHT = 0b0000000100000000,
        A     = 0b0000000000010000,
        B     = 0b0001000000000000,
        C     = 0b0010000000000000,
        START = 0b0000000000100000,
    };
}

struct InputShim {
    uint16_t pad1Mask = 0;
    uint16_t pad2Mask = 0;
    bool pad1State = false;   // select line toggle, port 0 ($2008)
    bool pad2State = false;   // select line toggle, port 1 ($2009)

    // Verbatim port of JoystickAdapter::read(): returns the active-low byte for
    // the current half (select HIGH vs LOW) of the addressed pad, toggling the
    // select line and clearing the other pad's toggle when stateful.
    uint8_t read(uint8_t portNum, bool stateful) {
        uint8_t outbyte = 0xFF;
        if (portNum % 2) {
            if (pad2State) outbyte = (uint8_t)(pad2Mask >> 8);
            else           outbyte = (uint8_t)pad2Mask;
            if (stateful) { pad1State = false; pad2State = !pad2State; }
        } else {
            if (pad1State) outbyte = (uint8_t)(pad1Mask >> 8);
            else           outbyte = (uint8_t)pad1Mask;
            if (stateful) { pad2State = false; pad1State = !pad1State; }
        }
        return ~outbyte;
    }

    void reset() {
        pad1Mask = pad2Mask = 0;
        pad1State = pad2State = false;
    }
};
static InputShim joysticks_obj;
static InputShim *joysticks = &joysticks_obj;

// ===========================================================================
// libretro callbacks
// ===========================================================================
static retro_environment_t       environ_cb;
static retro_video_refresh_t     video_cb;
#ifdef GT_PROFILE
uint32_t gt_prof[256];
// fine histogram is bank-aware: banks 0-2 collide at $8000-BFFF, so slot
// 0-2 = the flash bank latch for banked PCs, slot 3 = everything else
// (fixed bank, RAM). Same layout for the call counter.
uint32_t gt_prof_fine[4][65536];
uint32_t gt_jsr_cnt[4][65536];
uint8_t  gt_prof_bank;   // updated by the flash bank shifter
uint8_t  gt_prof_last_cycles;
uint16_t gt_pchist[2048];
uint16_t gt_pchist_i;
// wasm builds can't see host env vars (getenv reads the virtual env), so
// the profile window is set through an exported call instead
static int gt_prof_from = 0, gt_prof_at = 400;
static int gt_watch_addr = -1;
static int gt_fine_floor = -1;   /* -1: default total/2000 */
extern "C" RETRO_API void gt_prof_config(int from, int at) {
    gt_prof_from = from;
    gt_prof_at = at;
}
extern "C" RETRO_API void gt_prof_floor(int floor_c) { gt_fine_floor = floor_c; }
extern "C" RETRO_API void gt_watch_config(int addr) { gt_watch_addr = addr; }
#else
// the wasm export list names these unconditionally — keep no-op stubs so
// non-debug builds link
extern "C" RETRO_API void gt_prof_config(int from, int at) { (void)from; (void)at; }
extern "C" RETRO_API void gt_prof_floor(int floor_c) { (void)floor_c; }
extern "C" RETRO_API void gt_watch_config(int addr) { (void)addr; }
#endif
static retro_audio_sample_t      audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t        input_poll_cb;
static retro_input_state_t       input_state_cb;
static retro_log_printf_t        log_cb;

static bool force_ram32k = false;  // core option override (FLASH2M -> FLASH2M_RAM32K)
static bool flash_dirty  = false;  // a FLASH2M cart self-modified its 2 MB image
                                   // -> include the flash in the save state

// ===========================================================================
// Bus functions (ported from gte.cpp, devtools stripped)
// ===========================================================================
static uint8_t open_bus() {
    return rand() % 256;
}

uint8_t VDMA_Read(uint16_t address) {
    blitter->CatchUp();
    if (system_state.dma_control & DMA_COPY_ENABLE_BIT) {
        return open_bus();
    } else {
        uint8_t* bufPtr;
        uint32_t offset = 0;
        if (system_state.dma_control & DMA_CPU_TO_VRAM) {
            bufPtr = system_state.vram;
            if (system_state.banking & BANK_VRAM_MASK) {
                offset = 0x4000;
            }
        } else {
            bufPtr = system_state.gram;
            offset = (((system_state.banking & BANK_GRAM_MASK) << 2) | (blitter->gram_mid_bits)) << 14;
        }
        return bufPtr[(address & 0x3FFF) | offset];
    }
}

void VDMA_Write(uint16_t address, uint8_t value) {
    blitter->CatchUp();
    if (system_state.dma_control & DMA_COPY_ENABLE_BIT) {
        blitter->SetParam(address, value);
    } else {
        uint8_t* bufPtr;
        uint32_t offset = 0;
        // LIBRETRO: only the VRAM surface is presented; GRAM has no surface.
        // We still keep the raw byte writes for both buffers (VDMA_Read + the
        // blitter read those). The displayed-surface put_pixel32 only matters
        // for VRAM (CPU_TO_VRAM); GRAM pixels are never composited directly.
        bool toVram = (system_state.dma_control & DMA_CPU_TO_VRAM) != 0;
        uint32_t yShift = 0;
        if (toVram) {
            bufPtr = system_state.vram;
            if (system_state.banking & BANK_VRAM_MASK) {
                offset = 0x4000;
                yShift = GT_HEIGHT;
            }
        } else {
            bufPtr = system_state.gram;
            offset = (((system_state.banking & BANK_GRAM_MASK) << 2) | (blitter->gram_mid_bits)) << 14;
        }
        bufPtr[(address & 0x3FFF) | offset] = value;

        if (toVram) {
            uint8_t x = address & 127;
            uint8_t y = (address >> 7) & 127;
            put_pixel32(vram_surface, x, y + yShift, Palette::ConvertColor(vram_surface, value));
        }
    }
}

void UpdateFlashShiftRegister(uint8_t nextVal) {
    uint8_t oldVal = system_state.VIA_regs[VIA_ORA];

    uint8_t risingBits = nextVal & ~oldVal;
    if (risingBits & VIA_SPI_BIT_CLK) {
        cartridge_state.bank_shifter = cartridge_state.bank_shifter << 1;
        cartridge_state.bank_shifter &= 0xFE;
        cartridge_state.bank_shifter |= !!(oldVal & VIA_SPI_BIT_MOSI);
    } else if (risingBits & VIA_SPI_BIT_CS) {
        // LIBRETRO: upstream calls SaveNVRAM() on the bank-bit transition; the
        // frontend persists save_ram via RETRO_MEMORY_SAVE_RAM instead, so we
        // just latch the new bank mask.
        cartridge_state.bank_mask = cartridge_state.bank_shifter;
#ifdef GT_PROFILE
        { extern uint8_t gt_prof_bank;
          gt_prof_bank = cartridge_state.bank_shifter & 3; }
#endif
        if (loadedRomType != RomType::FLASH2M_RAM32K) {
            cartridge_state.bank_mask |= 0x80;
        }
    }
}

uint8_t MemoryRead_Flash2M(uint16_t address) {
    if (address & 0x4000) {
        return cartridge_state.rom[0b111111100000000000000 | (address & 0x3FFF)];
    } else {
        if (!(cartridge_state.bank_mask & 0x80))
            return cartridge_state.save_ram[(address & 0x3FFF) | ((cartridge_state.bank_mask & 0x40) << 8)];
        else return cartridge_state.rom[((cartridge_state.bank_mask & 0x7F) << 14) | (address & 0x3FFF)];
    }
}

uint8_t MemoryRead_Unknown(uint16_t address) {
    if (cartridge_state.size <= 32768) {
        return cartridge_state.rom[((address & 0x7FFF) + 32768 - cartridge_state.size) % cartridge_state.size];
    } else {
        return cartridge_state.rom[((address & 0x7FFF) + cartridge_state.size - 32768)];
    }
}

uint8_t* GetRAM(const uint16_t address) {
    return &(system_state.ram[FULL_RAM_ADDRESS(address & 0x1FFF)]);
}

uint8_t MemoryReadResolve(const uint16_t address, bool stateful) {
    if (address & 0x8000) {
        switch (loadedRomType) {
            case RomType::EEPROM8K:
                return cartridge_state.rom[address & 0x1FFF];
            case RomType::EEPROM32K:
                return cartridge_state.rom[address & 0x7FFF];
            case RomType::FLASH2M:
            case RomType::FLASH2M_RAM32K:
                return MemoryRead_Flash2M(address);
            case RomType::UNKNOWN:
                return MemoryRead_Unknown(address);
        }
    } else if (address & 0x4000) {
        return VDMA_Read(address);
    } else if ((address >= 0x3000) && (address <= 0x3FFF)) {
        return soundcard->ram_read(address);
    } else if ((address >= 0x2800) && (address <= 0x2FFF)) {
        return system_state.VIA_regs[address & 0xF];
    } else if (address < 0x2000) {
        return *GetRAM(address);
    } else if ((address == 0x2008) || (address == 0x2009)) {
        return joysticks->read((uint8_t)address, stateful);
    }
    return open_bus();
}

uint8_t MemoryRead(uint16_t address) {
    return MemoryReadResolve(address, true);
}

void MemoryWrite(uint16_t address, uint8_t value) {
#ifdef GT_PROFILE
    // gt_watch_config: trap writes to one RAM address, print the writer's pc
    if ((int)address == gt_watch_addr && cpu_core) {
        static int hits = 0;
        if (hits < 24) {
            hits++;
            fprintf(stderr, "[WATCH] write %02x to %04x from pc~%04x sp=%02x stk:",
                    value, address, cpu_core->pc, cpu_core->sp);
            for (int i = cpu_core->sp + 1; i <= 0xFF && i <= cpu_core->sp + 8; i++)
                fprintf(stderr, " %02x", system_state.ram[0x100 + i]);
            fprintf(stderr, "\n");
        }
    }
#endif
    if (address & 0x8000) {
        if (loadedRomType == RomType::FLASH2M_RAM32K) {
            if (!(address & 0x4000)) {
                if (!(cartridge_state.bank_mask & 0x80)) {
                    cartridge_state.save_ram[(address & 0x3FFF) | ((cartridge_state.bank_mask & 0x40) << 8)] = value;
                }
            }
        }
        if (loadedRomType == RomType::FLASH2M) {
            if (cartridge_state.write_mode) {
                uint8_t* location;
                if (address & 0x4000) {
                    location = &(cartridge_state.rom[0b111111100000000000000 | (address & 0x3FFF)]);
                } else {
                    location = &(cartridge_state.rom[((cartridge_state.bank_mask & 0x7F) << 14) | (address & 0x3FFF)]);
                }
                *location &= value;
                cartridge_state.write_mode = false;
                flash_dirty = true;   // self-modified -> serialize the flash image
            } else {
                if (value == 0x10) {
                    // Chip erase
                    for (int i = 0; i < (1 << 21); ++i) {
                        cartridge_state.rom[i] = 0xFF;
                    }
                    flash_dirty = true;
                } else if (value == 0x30) {
                    // Sector erase
                    uint8_t sectorBits = ((address & (1 << 13)) >> 13) | ((cartridge_state.bank_mask & 0x7F) << 1);
                    uint8_t sectorNum = sectorBits >> 3;
                    if (sectorNum < 31) {
                        uint32_t x = sectorNum << 16;
                        for (uint32_t i = 0; i < (1 << 16); ++i) { cartridge_state.rom[x] = 0xFF; ++x; }
                    } else if ((sectorBits & 4) == 0) {
                        uint32_t x = 0x1F0000;
                        for (uint32_t i = 0; i < (1 << 15); ++i) { cartridge_state.rom[x] = 0xFF; ++x; }
                    } else if (sectorBits == 0b11111100) {
                        uint32_t x = 0x1F8000;
                        for (uint32_t i = 0; i < (1 << 13); ++i) { cartridge_state.rom[x] = 0xFF; ++x; }
                    } else if (sectorBits == 0b11111101) {
                        uint32_t x = 0x1FA000;
                        for (uint32_t i = 0; i < (1 << 13); ++i) { cartridge_state.rom[x] = 0xFF; ++x; }
                    } else if ((sectorBits >> 1) == 0b1111111) {
                        uint32_t x = 0x1FC000;
                        for (uint32_t i = 0; i < (1 << 14); ++i) { cartridge_state.rom[x] = 0xFF; ++x; }
                    }
                    flash_dirty = true;
                } else if (value == 0xA0) {
                    cartridge_state.write_mode = true;
                } else if (value == 0x90) {
                    // LIBRETRO: upstream snapshots dirty flash to a .xor here;
                    // we rely on serialize/SAVE_RAM instead -> no-op.
                }
            }
        }
    } else if (address & 0x4000) {
        VDMA_Write(address, value);
    } else if (address >= 0x3000 && address <= 0x3FFF) {
        soundcard->ram_write(address, value);
    } else if ((address & 0x2000)) {
        if (address & 0x800) {
            if (loadedRomType == RomType::FLASH2M) {
                if ((address & 0xF) == VIA_ORA) {
                    UpdateFlashShiftRegister(value);
                }
            }
            // LIBRETRO: ORB profiler timestamp hook dropped (devtools only).
            (void)VIA_ORB;
            system_state.VIA_regs[address & 0xF] = value;
        } else {
            if ((address & 0x000F) == 0x0007) {
                blitter->CatchUp();
                system_state.dma_control = value;
                system_state.dma_control_irq = (system_state.dma_control & DMA_COPY_IRQ_BIT) != 0;
                // LIBRETRO: transparency colorkey is handled by the blitter's
                // own colorbus!=0 test; nothing to set on a surface here.
            } else if ((address & 0x000F) == 0x0005) {
                blitter->CatchUp();
                system_state.banking = value;
            } else {
                soundcard->register_write(address, value);
            }
        }
    } else if (address < 0x2000) {
        system_state.ram_initialized[FULL_RAM_ADDRESS(address & 0x1FFF)] = true;
        system_state.ram[FULL_RAM_ADDRESS(address & 0x1FFF)] = value;
    }
}

static void CPUStopped() {
    // STP opcode. Upstream pauses + pops a dialog; we just stop stepping. The
    // CPU's freeze flag handles the actual halt.
}

// ===========================================================================
// Init / reset helpers
// ===========================================================================
static void randomize_memory() {
    for (int i = 0; i < RAMSIZE; i++) {
        system_state.ram[i] = rand() % 256;
        system_state.ram_initialized[i] = false;
    }
    for (int i = 0; i < VRAM_BUFFER_SIZE; i++) {
        system_state.vram[i] = rand() % 256;
    }
    for (int i = 0; i < GRAM_BUFFER_SIZE; i++) {
        system_state.gram[i] = rand() % 256;
    }
    system_state.dma_control = rand() % 256;
    system_state.dma_control_irq = (system_state.dma_control & DMA_COPY_IRQ_BIT) != 0;
    system_state.banking = rand() % 256;
    blitter->gram_mid_bits = rand() % 4;
}

static void randomize_vram_surface() {
    // Paint the XRGB surface from the (randomized) raw VRAM bytes so the very
    // first presented frame is not uninitialized host memory.
    for (int i = 0; i < VRAM_BUFFER_SIZE; i++) {
        put_pixel32(vram_surface, i & 127, i >> 7, Palette::ConvertColor(vram_surface, system_state.vram[i]));
    }
}

static void init_emulator() {
    // Allocate the full 2 MB cartridge backing (matches upstream new[1<<21]).
    if (!cartridge_state.rom) {
        cartridge_state.rom = new uint8_t[1 << 21];
        memset(cartridge_state.rom, 0, 1 << 21);
    }
    cartridge_state.size = 8192;
    cartridge_state.bank_shifter = 0;
    cartridge_state.bank_mask = 0;
    cartridge_state.write_mode = false;
    memset(cartridge_state.save_ram, 0, CARTRAMSIZE);

    // Wire the shim surface to the XRGB framebuffer (128 x 256).
    vram_surface_obj.pixels = vram_fb;
    vram_surface_obj.w      = GT_WIDTH;
    vram_surface_obj.h      = GT_HEIGHT * 2;
    vram_surface_obj.pitch  = GT_WIDTH * 4;
    vram_surface_obj.format = nullptr;

    // The libretro core drives fill_audio() directly; never open an SDL device.
    EmulatorConfig::noSound = true;

    soundcard = new AudioCoprocessor();
    cpu_core  = new mos6502(MemoryRead, MemoryWrite, CPUStopped, nullptr);
#ifdef GT_PROFILE
    cpu_core->lr_profile = true;
#endif
    cpu_core->Reset();
    blitter   = new Blitter(cpu_core, &timekeeper, &system_state, vram_surface);

    randomize_memory();
    randomize_vram_surface();
}

// ROM type detection + load (port of LoadRomFile, gte.cpp ~599).
static bool load_rom(const uint8_t* data, size_t size) {
    cartridge_state.size = (int)size;
    cartridge_state.write_mode = false;
    cartridge_state.bank_shifter = 0;
    cartridge_state.bank_mask = 0;
    flash_dirty = false;   // fresh cart: flash matches the loaded image

    switch (size) {
        case 8192:    loadedRomType = RomType::EEPROM8K;  break;
        case 32768:   loadedRomType = RomType::EEPROM32K; break;
        case 2097152: loadedRomType = RomType::FLASH2M;   break;
        default:      loadedRomType = RomType::UNKNOWN;   break;
    }

    // rom backing is the fixed 2 MB buffer; clear then copy actual contents.
    memset(cartridge_state.rom, 0, 1 << 21);
    memcpy(cartridge_state.rom, data, size < (1u << 21) ? size : (1u << 21));

    // Flash carts with a battery-SRAM "SAVE" magic at 0x1FFFF0 become
    // FLASH2M_RAM32K (32 KB persistent SRAM), unless forced by core option.
    if (loadedRomType == RomType::FLASH2M) {
        if (force_ram32k ||
            (cartridge_state.rom[0x1FFFF0] == 'S' &&
             cartridge_state.rom[0x1FFFF1] == 'A' &&
             cartridge_state.rom[0x1FFFF2] == 'V' &&
             cartridge_state.rom[0x1FFFF3] == 'E')) {
            loadedRomType = RomType::FLASH2M_RAM32K;
        }
    }

    cpu_core->Reset();        // load reset vector at $FFFC/$FFFD
    cartridge_state.write_mode = false;
    soundcard->ram_read(0);   // touch (no-op) to keep singleton wired
    AudioCoprocessor::singleton_acp_state->isEmulationPaused = false;
    return loadedRomType != RomType::UNKNOWN || size > 0;
}

// ===========================================================================
// Per-frame input latch
// ===========================================================================
static uint16_t poll_pad(unsigned port) {
    uint16_t mask = 0;
    if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP))     mask |= GTButtons::UP;
    if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN))   mask |= GTButtons::DOWN;
    if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))   mask |= GTButtons::LEFT;
    if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT))  mask |= GTButtons::RIGHT;
    // GameTank A == NES-style B/bottom face; map B(retro) -> A(gt),
    // A(retro) -> C(gt) so the primary face button is the GameTank "A".
    if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B))      mask |= GTButtons::A;
    if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A))      mask |= GTButtons::C;
    if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y))      mask |= GTButtons::B;
    if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START))  mask |= GTButtons::START;
    if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT)) mask |= GTButtons::START; // alias
    return mask;
}

// ===========================================================================
// Core options
// ===========================================================================
static const struct retro_variable core_options[] = {
    { "gametank_force_ram32k", "Force 32K battery SRAM (Flash2M carts); disabled|enabled" },
    { nullptr, nullptr },
};

static void check_variables() {
    struct retro_variable var = { "gametank_force_ram32k", nullptr };
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        force_ram32k = (strcmp(var.value, "enabled") == 0);
    }
}

// ===========================================================================
// libretro API
// ===========================================================================
RETRO_API void retro_set_environment(retro_environment_t cb) {
    environ_cb = cb;
    bool no_game = false;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
    cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)core_options);
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb)        { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb)         { audio_cb = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb)            { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb)          { input_state_cb = cb; }

RETRO_API void retro_init(void) {
    struct retro_log_callback logging;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging)) {
        log_cb = logging.log;
    } else {
        log_cb = nullptr;
    }
    srand(0x6502);  // deterministic-ish; randomize_memory only affects unread RAM
    init_emulator();
}

RETRO_API void retro_deinit(void) {
    delete blitter;   blitter = nullptr;
    delete cpu_core;  cpu_core = nullptr;
    delete soundcard; soundcard = nullptr;
    delete[] cartridge_state.rom; cartridge_state.rom = nullptr;
}

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

RETRO_API void retro_get_system_info(struct retro_system_info *info) {
    memset(info, 0, sizeof(*info));
    info->library_name     = "GameTank";
    info->library_version  = "0.1.0";
    info->valid_extensions = "gtr";
    info->need_fullpath    = false;
    info->block_extract    = false;
}

// GameTank DAC native sample rate (Hz): 315000000 / (88 * 256).
static double dac_sample_rate() {
    return 315000000.0 / (88.0 * 256.0);   // ~13967.6 Hz
}

// 8-bit DAC sample -> S16 gain. The raw DAC byte (0..255) is high-passed below to
// remove DC, so it ends up centered near 0; ~180 maps the ±128 swing to roughly
// ±23000 with headroom.
#define GT_AUDIO_GAIN 180

// DC-blocking high-pass (models the GameTank's output coupling capacitor). The
// raw 8-bit DAC carries a large, game-dependent DC bias (e.g. it can hover near 0
// rather than swinging around mid-scale). On real hardware a series cap removes
// that bias; without it we'd emit a constant DC rail — a loud thump/buzz, which
// is exactly the "horrible sound". One-pole: y[n] = x[n] - x[n-1] + R*y[n-1].
// R≈0.9995 at ~14 kHz → ~2 Hz corner, inaudible yet kills the offset.
static double dc_x1 = 0.0, dc_y1 = 0.0;
static void gt_audio_reset_filter(void) { dc_x1 = 0.0; dc_y1 = 0.0; }

// Correct-math replacement for AudioCoprocessor::fill_audio (see retro_run for
// why the vendored one can't feed libretro's S16 sink). Advances the audio CPU
// with IDENTICAL timing, but emits DC-blocked, properly-signed, sanely-scaled
// interleaved stereo S16 directly. `out` must hold n*2 int16_t.
static void gt_fill_audio(ACPState *state, int16_t *out, uint32_t n) {
    const double R = 0.9995;
    if (state->isEmulationPaused || state->isMuted) {
        for (uint32_t i = 0; i < n * 2; i++) out[i] = 0;
        return;   // original zero-fills + returns without advancing; match it
    }
    for (uint32_t i = 0; i < n; i++) {
        double x = (double)state->dacReg;          // raw 0..255 (carries DC bias)
        double y = x - dc_x1 + R * dc_y1;          // DC-blocking high-pass
        dc_x1 = x; dc_y1 = y;

        int s = (int)(y * GT_AUDIO_GAIN);          // scale the AC-coupled signal
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        out[i * 2]     = (int16_t)s;
        out[i * 2 + 1] = (int16_t)s;

        // --- CPU-advance loop ---
        // Upstream fill_audio() fires at most ONE sample-IRQ per host sample,
        // which is fine when the host rate ~= the DAC rate. Under this shim
        // clksPerHostSample (1024) >> irqRate (255), so the int16 counter
        // free-fell ~769/sample until it wrapped, starving the ACP of IRQs
        // for ~32 of every ~75 host samples (audible 187 Hz chop + low notes
        // reading flat). Let the ACP catch up fully each host sample instead.
        state->irqCounter -= state->clksPerHostSample;
        while (state->irqCounter < 0) {
            if (state->resetting) {
                state->resetting = false;
                state->cpu->Reset();
            }
            state->irqCounter += (state->irqRate ? state->irqRate : 255);
            state->cycle_counter = 0;
            if (state->running) {
                state->cpu->IRQ();
                state->cpu->ClearIRQ();
                state->cpu->Run(state->cycles_per_sample, state->cycle_counter);
            }
        }
    }
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info) {
    memset(info, 0, sizeof(*info));
    info->geometry.base_width   = GT_WIDTH;
    info->geometry.base_height  = GT_HEIGHT;
    info->geometry.max_width    = GT_WIDTH;
    info->geometry.max_height   = GT_HEIGHT;
    info->geometry.aspect_ratio = (float)GT_WIDTH / (float)GT_HEIGHT;
    info->timing.fps            = 60.0;
    info->timing.sample_rate    = dac_sample_rate();
}

RETRO_API void retro_set_controller_port_device(unsigned, unsigned) {}

RETRO_API void retro_reset(void) {
    randomize_memory();
    randomize_vram_surface();
    cpu_core->Reset();
    cartridge_state.write_mode = false;
    joysticks->reset();
    timekeeper.totalCyclesCount = 0;
    timekeeper.cycles_since_vsync = 0;
}

// ---------------------------------------------------------------------------
// Frame loop. One retro_run() == one 60 Hz frame:
//   1. run cycles_per_vsync main-CPU cycles
//   2. blitter->CatchUp()
//   3. on the vsync boundary, if DMA_VSYNC_NMI_BIT -> cpu_core->NMI()
//   4. pull a frame's worth of audio from the ACP and emit mono->stereo S16
//   5. present the active 128x128 VRAM page as XRGB8888
// ---------------------------------------------------------------------------
RETRO_API void retro_run(void) {
    bool options_updated = false;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &options_updated) && options_updated) {
        check_variables();
    }

    // --- input latch -------------------------------------------------------
    if (input_poll_cb) input_poll_cb();
    joysticks->pad1Mask = poll_pad(0);
    joysticks->pad2Mask = poll_pad(1);

    // --- run one frame of CPU ---------------------------------------------
    uint64_t intended_cycles = timekeeper.cycles_per_vsync;
    cpu_core->Run((int32_t)intended_cycles, timekeeper.totalCyclesCount);

    blitter->CatchUp();

    timekeeper.cycles_since_vsync += intended_cycles;
    if (timekeeper.cycles_since_vsync >= timekeeper.cycles_per_vsync) {
        timekeeper.cycles_since_vsync -= timekeeper.cycles_per_vsync;
        if (system_state.dma_control & DMA_VSYNC_NMI_BIT) {
            cpu_core->NMI();
        }
    }
    blitter->pixels_this_frame = 0;

    // --- audio: pull this frame's samples from the ACP --------------------
    // We DON'T call the vendored AudioCoprocessor::fill_audio here: its sample
    // math is broken for libretro's signed S16 sink. It writes through a
    // uint16_t* and does `s -= 128` (unsigned underflow on the bottom half of the
    // 8-bit DAC range → huge positive garbage) then `*= volume` (256 → full-scale
    // ±32768, clips). The original SDL app masked this via its device format; fed
    // raw into audio_batch_cb it produces the harsh/noisy sound.
    //
    // gt_fill_audio() below mirrors fill_audio's CPU-advance timing EXACTLY (so
    // emulation is unchanged) but emits correctly-signed, sanely-scaled S16:
    //   sample = (dacReg - 128) * GT_AUDIO_GAIN   // centered, signed
    // GT_AUDIO_GAIN=128 maps the 8-bit DAC's ±128 to ±16384 — half-scale, with
    // headroom so peaks don't clip.
    {
        ACPState *acp = AudioCoprocessor::singleton_acp_state;
        uint32_t n = acp->samples_per_frame;
        if (n == 0) n = (uint32_t)(dac_sample_rate() / 60.0 + 0.5);  // ~233
        if (n > 2048) n = 2048;

        static int16_t stereo[2048 * 2];
        gt_fill_audio(acp, stereo, n);
        if (audio_batch_cb) audio_batch_cb(stereo, n);
    }

#ifdef GT_PROFILE
    { static int pf = 0;
      ++pf;
      if (pf == gt_prof_from) {
        extern uint32_t gt_prof[256];
        memset(gt_prof, 0, sizeof(gt_prof));
        memset(gt_prof_fine, 0, sizeof(gt_prof_fine));
        memset(gt_jsr_cnt, 0, sizeof(gt_jsr_cnt));
      }
      if (pf == gt_prof_at) {
        extern uint32_t gt_prof[256];
        fprintf(stderr, "[PROF]\n");
        for (int i = 0; i < 256; i++)
            if (gt_prof[i]) fprintf(stderr, "%02x %u\n", i, gt_prof[i]);
        fprintf(stderr, "[PROFEND]\n");
        // fine histogram: bank-tagged lines "b:addr count"
        {
            uint64_t total = 0;
            for (int b = 0; b < 4; b++)
                for (int i = 0; i < 65536; i++) total += gt_prof_fine[b][i];
            /* getenv is useless in wasm (virtual env) — the floor arrives
             * via the exported gt_prof_floor() call */
            uint32_t floor_c = (gt_fine_floor >= 0)
                ? (uint32_t)gt_fine_floor : (uint32_t)(total / 2000);
            fprintf(stderr, "[FINE] total=%llu\n", (unsigned long long)total);
            for (int b = 0; b < 4; b++)
                for (int i = 0; i < 65536; i++)
                    if (gt_prof_fine[b][i] > floor_c)
                        fprintf(stderr, "%d:%04x %u\n", b, i, gt_prof_fine[b][i]);
            fprintf(stderr, "[FINEEND]\n");
            fprintf(stderr, "[CALLS]\n");
            for (int b = 0; b < 4; b++)
                for (int i = 0; i < 65536; i++)
                    if (gt_jsr_cnt[b][i] > 200)
                        fprintf(stderr, "%d:%04x %u\n", b, i, gt_jsr_cnt[b][i]);
            fprintf(stderr, "[CALLSEND]\n");
        }
        // the last 512 control transfers: who jumps where right now
        extern uint16_t gt_cthist[512][2];
        extern uint32_t gt_cthist_i;
        fprintf(stderr, "[CT]\n");
        for (uint32_t k = 0; k < 512; k++) {
            uint32_t idx = (gt_cthist_i + k) & 511;
            fprintf(stderr, "%04x %04x\n", gt_cthist[idx][0], gt_cthist[idx][1]);
        }
        fprintf(stderr, "[CTEND]\n");
        extern uint16_t gt_pchist[2048];
        extern uint16_t gt_pchist_i;
        fprintf(stderr, "[PCH]\n");
        for (uint32_t k = 0; k < 2048; k++)
            fprintf(stderr, "%04x\n", gt_pchist[(gt_pchist_i + k) & 2047]);
        fprintf(stderr, "[PCHEND]\n");
      } }
#endif
    // --- present the active VRAM page -------------------------------------
    // src.y = DMA_VID_OUT_PAGE_BIT ? GT_HEIGHT : 0 (the displayed 128x128 page
    // in the 128x256 framebuffer).
    int page_y = (system_state.dma_control & DMA_VID_OUT_PAGE_BIT) ? GT_HEIGHT : 0;
    const uint32_t *page = vram_fb + (page_y * GT_WIDTH);
    if (video_cb) {
        video_cb(page, GT_WIDTH, GT_HEIGHT, GT_WIDTH * sizeof(uint32_t));
    }
}

// ---------------------------------------------------------------------------
// Input descriptors (NES-style: 2 face buttons + select + start, 2 ports)
// ---------------------------------------------------------------------------
static void set_input_descriptors(void) {
    static const struct retro_input_descriptor desc[] = {
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "Up" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "Down" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "Left" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "Right" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "A" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "B" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "C" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },

        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "Up" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "Down" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "Left" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "Right" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "A" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "B" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "C" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
        { 0, 0, 0, 0, nullptr },
    };
    if (environ_cb) environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void*)desc);
}

// Expose every emulated RAM region to the frontend via SET_MEMORY_MAPS so cheats
// and external tooling (e.g. romdev's memory / inspectSprites / audio inspectors)
// can read GRAM (sprite RAM) and ACP audio RAM — not just the SYSTEM/VIDEO/SAVE
// regions reachable through retro_get_memory_data. The 'start' addresses below are
// flat tool-facing offsets (the real GameTank bus banks these), chosen not to
// overlap; RETRO_MEMDESC_* select=0 leaves the frontend to treat them as opaque.
static void set_memory_maps(void) {
    if (!environ_cb) return;
    static struct retro_memory_descriptor desc[4];
    unsigned n = 0;
    // System RAM (32 KB) at 0x000000.
    desc[n++] = (struct retro_memory_descriptor){
        RETRO_MEMDESC_SYSTEM_RAM, system_state.ram, 0, 0x000000, 0, 0, RAMSIZE, "RAM" };
    // VRAM framebuffer pages (32 KB) at 0x010000.
    desc[n++] = (struct retro_memory_descriptor){
        RETRO_MEMDESC_VIDEO_RAM, system_state.vram, 0, 0x010000, 0, 0, VRAM_BUFFER_SIZE, "VRAM" };
    // GRAM / sprite RAM (512 KB) at 0x100000.
    desc[n++] = (struct retro_memory_descriptor){
        RETRO_MEMDESC_VIDEO_RAM, system_state.gram, 0, 0x100000, 0, 0, GRAM_BUFFER_SIZE, "GRAM" };
    // Audio coprocessor RAM (4 KB) at 0x200000.
    if (AudioCoprocessor::singleton_acp_state)
        desc[n++] = (struct retro_memory_descriptor){
            0, AudioCoprocessor::singleton_acp_state->ram, 0, 0x200000, 0, 0, AUDIO_RAM_SIZE, "ACPRAM" };

    struct retro_memory_map mmap = { desc, n };
    environ_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &mmap);
}

RETRO_API bool retro_load_game(const struct retro_game_info *game) {
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (environ_cb && !environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
        if (log_cb) log_cb(RETRO_LOG_ERROR, "XRGB8888 not supported by frontend\n");
        return false;
    }

    if (!game || !game->data || game->size == 0) {
        return false;
    }

    check_variables();
    set_input_descriptors();

    if (!load_rom((const uint8_t*)game->data, game->size))
        return false;

    set_memory_maps();   // after load_rom — the ACP state exists by now
    return true;
}

RETRO_API bool retro_load_game_special(unsigned, const struct retro_game_info *, size_t) {
    return false;
}

RETRO_API void retro_unload_game(void) {
    loadedRomType = RomType::UNKNOWN;
}

RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

// ===========================================================================
// Memory map (RAM / VRAM / GRAM / audio-RAM for cheats + romdev tools)
// ===========================================================================
RETRO_API void *retro_get_memory_data(unsigned id) {
    switch (id) {
        case RETRO_MEMORY_SAVE_RAM:
            // Battery SRAM only exists on FLASH2M_RAM32K carts.
            return (loadedRomType == RomType::FLASH2M_RAM32K) ? cartridge_state.save_ram : nullptr;
        case RETRO_MEMORY_SYSTEM_RAM:
            return system_state.ram;
        case RETRO_MEMORY_VIDEO_RAM:
            return system_state.vram;
        default:
            return nullptr;
    }
}

RETRO_API size_t retro_get_memory_size(unsigned id) {
    switch (id) {
        case RETRO_MEMORY_SAVE_RAM:
            return (loadedRomType == RomType::FLASH2M_RAM32K) ? CARTRAMSIZE : 0;
        case RETRO_MEMORY_SYSTEM_RAM:
            return RAMSIZE;
        case RETRO_MEMORY_VIDEO_RAM:
            return VRAM_BUFFER_SIZE;
        default:
            return 0;
    }
}

// ===========================================================================
// Save state.
//
// Versioned blob: SystemState + CartridgeState scalars + main/audio CPU public
// registers + audio RAM. The flash 2 MB rom image and the mos6502 *private*
// IRQ-scheduling state (irq_timer/irq_line/irq_gate) are NOT captured (the
// header exposes no accessors); non-flash-write games are deterministic. See
// doc/PORT_NOTES.md "Open items".
// ===========================================================================
#define GT_STATE_MAGIC   0x47544B31u  // "GTK1"
#define GT_STATE_VERSION 3            // v3: + blitter engine state (v2 loads
                                      //     get an idle blitter + rebuilt surface)

struct CpuRegs {
    uint8_t  A, X, Y, sp, status;
    uint16_t pc;
    uint8_t  freeze, illegalOpcode, waiting;
    uint32_t irq_timer;   // private IRQ-scheduling state (via LIBRETRO accessors)
    uint8_t  irq_line;
};

struct SaveBlob {
    uint32_t magic;
    uint32_t version;

    // SystemState scalars + buffers
    uint8_t  dma_control;
    uint8_t  dma_control_irq;
    uint8_t  banking;
    uint8_t  ram[RAMSIZE];
    uint8_t  vram[VRAM_BUFFER_SIZE];
    uint8_t  gram[GRAM_BUFFER_SIZE];
    uint8_t  VIA_regs[16];

    // CartridgeState scalars (not the rom pointer)
    int32_t  cart_size;
    uint8_t  bank_shifter;
    uint32_t bank_mask;
    uint8_t  write_mode;
    uint8_t  save_ram[CARTRAMSIZE];
    uint32_t loadedRomType;

    // CPUs
    CpuRegs  main_cpu;
    CpuRegs  audio_cpu;
    uint8_t  acp_ram[AUDIO_RAM_SIZE];

    // Timekeeper accumulators
    uint64_t total_cycles;
    uint64_t cycles_since_vsync;

    // Blitter engine (v3): params/counters/phase flags/GRAM quadrant latch.
    // Raw bytes of Blitter::LRState (16 bytes, fixed layout).
    uint8_t  blitter_state[16];

    // Set when the 2 MB flash image follows this struct (self-modifying FLASH2M
    // cart). Avoids paying 2 MB on every state for the common read-only cart.
    uint8_t  has_flash;
    // (2 MB flash image, if has_flash, is appended after sizeof(SaveBlob))
};

#define GT_FLASH_SIZE (1u << 21)   // 2 MB

static void grab_cpu_regs(mos6502 *cpu, CpuRegs *r) {
    r->A = cpu->A; r->X = cpu->X; r->Y = cpu->Y;
    r->sp = cpu->sp; r->status = cpu->status; r->pc = cpu->pc;
    r->freeze = cpu->freeze; r->illegalOpcode = cpu->illegalOpcode; r->waiting = cpu->waiting;
    r->irq_timer = cpu->LR_GetIrqTimer();
    r->irq_line  = cpu->LR_GetIrqLine() ? 1 : 0;
}

static void put_cpu_regs(mos6502 *cpu, const CpuRegs *r) {
    cpu->A = r->A; cpu->X = r->X; cpu->Y = r->Y;
    cpu->sp = r->sp; cpu->status = r->status; cpu->pc = r->pc;
    cpu->freeze = r->freeze; cpu->illegalOpcode = r->illegalOpcode; cpu->waiting = r->waiting;
    // irq_gate is re-wired by the blitter/ACP, not serialized — restore only the
    // schedulable timer + line.
    cpu->LR_SetIrqState(r->irq_timer, r->irq_line != 0);
}

RETRO_API size_t retro_serialize_size(void) {
    // Stable per-load size: include the flash tail iff this cart has dirtied its
    // flash (frontends call this once and expect a fixed size, so key it on the
    // cart having self-modified rather than toggling mid-session).
    return sizeof(SaveBlob) + (flash_dirty ? GT_FLASH_SIZE : 0);
}

RETRO_API bool retro_serialize(void *data, size_t size) {
    if (size < sizeof(SaveBlob)) return false;
    SaveBlob *b = (SaveBlob*)data;
    memset(b, 0, sizeof(*b));
    b->magic   = GT_STATE_MAGIC;
    b->version = GT_STATE_VERSION;

    b->dma_control     = system_state.dma_control;
    b->dma_control_irq = system_state.dma_control_irq ? 1 : 0;
    b->banking         = system_state.banking;
    memcpy(b->ram,  system_state.ram,  RAMSIZE);
    memcpy(b->vram, system_state.vram, VRAM_BUFFER_SIZE);
    memcpy(b->gram, system_state.gram, GRAM_BUFFER_SIZE);
    memcpy(b->VIA_regs, system_state.VIA_regs, 16);

    b->cart_size     = cartridge_state.size;
    b->bank_shifter  = cartridge_state.bank_shifter;
    b->bank_mask     = cartridge_state.bank_mask;
    b->write_mode    = cartridge_state.write_mode ? 1 : 0;
    memcpy(b->save_ram, cartridge_state.save_ram, CARTRAMSIZE);
    b->loadedRomType = (uint32_t)loadedRomType;

    grab_cpu_regs(cpu_core, &b->main_cpu);
    if (AudioCoprocessor::singleton_acp_state->cpu) {
        grab_cpu_regs(AudioCoprocessor::singleton_acp_state->cpu, &b->audio_cpu);
    }
    memcpy(b->acp_ram, AudioCoprocessor::singleton_acp_state->ram, AUDIO_RAM_SIZE);

    b->total_cycles       = timekeeper.totalCyclesCount;
    b->cycles_since_vsync = timekeeper.cycles_since_vsync;

    {
        Blitter::LRState bs;
        blitter->LR_GetState(&bs);
        memcpy(b->blitter_state, &bs, sizeof(bs));
    }

    // Append the live 2 MB flash image for self-modifying FLASH2M carts.
    b->has_flash = flash_dirty ? 1 : 0;
    if (flash_dirty) {
        if (size < sizeof(SaveBlob) + GT_FLASH_SIZE) return false;
        memcpy((uint8_t*)data + sizeof(SaveBlob), cartridge_state.rom, GT_FLASH_SIZE);
    }
    return true;
}

RETRO_API bool retro_unserialize(const void *data, size_t size) {
    if (size < sizeof(SaveBlob)) return false;
    const SaveBlob *b = (const SaveBlob*)data;
    if (b->magic != GT_STATE_MAGIC) return false;
    if (b->version != GT_STATE_VERSION && b->version != 2) return false;
    if (b->has_flash && size < sizeof(SaveBlob) + GT_FLASH_SIZE) return false;

    system_state.dma_control     = b->dma_control;
    system_state.dma_control_irq = b->dma_control_irq != 0;
    system_state.banking         = b->banking;
    memcpy(system_state.ram,  b->ram,  RAMSIZE);
    memcpy(system_state.vram, b->vram, VRAM_BUFFER_SIZE);
    memcpy(system_state.gram, b->gram, GRAM_BUFFER_SIZE);
    memcpy(system_state.VIA_regs, b->VIA_regs, 16);

    cartridge_state.size         = b->cart_size;
    cartridge_state.bank_shifter = b->bank_shifter;
    cartridge_state.bank_mask    = b->bank_mask;
    cartridge_state.write_mode   = b->write_mode != 0;
    memcpy(cartridge_state.save_ram, b->save_ram, CARTRAMSIZE);
    loadedRomType = (RomType)b->loadedRomType;

    put_cpu_regs(cpu_core, &b->main_cpu);
    if (AudioCoprocessor::singleton_acp_state->cpu) {
        put_cpu_regs(AudioCoprocessor::singleton_acp_state->cpu, &b->audio_cpu);
    }
    memcpy(AudioCoprocessor::singleton_acp_state->ram, b->acp_ram, AUDIO_RAM_SIZE);

    timekeeper.totalCyclesCount  = b->total_cycles;
    timekeeper.cycles_since_vsync = b->cycles_since_vsync;

    // Blitter engine: v3 restores it exactly (mid-flight blits, phase flags,
    // the GRAM quadrant latch). A v2 state predates this — leave the engine
    // idle rather than half-restored.
    {
        Blitter::LRState bs;
        if (b->version >= 3) {
            memcpy(&bs, b->blitter_state, sizeof(bs));
        } else {
            memset(&bs, 0, sizeof(bs));
        }
        blitter->LR_SetState(&bs, b->total_cycles);
    }

    // Restore the self-modified flash image if the state carried one.
    if (b->has_flash) {
        memcpy(cartridge_state.rom, (const uint8_t*)data + sizeof(SaveBlob), GT_FLASH_SIZE);
        flash_dirty = true;
    }

    // Repaint the XRGB surface from restored VRAM bytes.
    randomize_vram_surface();
    for (int i = 0; i < VRAM_BUFFER_SIZE; i++) {
        put_pixel32(vram_surface, i & 127, i >> 7, Palette::ConvertColor(vram_surface, system_state.vram[i]));
    }
    return true;
}

// ===========================================================================
// Cheats (unused)
// ===========================================================================
RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned, bool, const char *) {}
