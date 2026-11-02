/* nrt.h - public C API of the nesrecomp1 runtime core (NTSC NES, mapper 0).
 *
 * One nrt_machine is the whole console: CPU, PPU, APU, bus and the frame scheduler. All state of a machine
 * lives inside the storage block given to nrt_machine_init(), so machines can be interleaved freely and a
 * machine can be re-initialised deterministically. The core never allocates, opens files or prints; hosts
 * (desktop, web, STM32, the nrt_test harness) own allocation, ROM images, the frame buffer and diagnostics.
 *
 * Behaviour is bit-for-bit the behaviour verified against FCEUX 2.6.6 by nes1 (see doc/parity_spec.md in nes1,
 * observations O-1 .. O-43). Audio synthesis is not part of this milestone: the APU is modelled only as far
 * as the CPU can observe it (frame counter, length counters, DMC fetches and IRQs).
 */
#ifndef NRT_H
#define NRT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NRT_SCREEN_WIDTH  256
#define NRT_SCREEN_HEIGHT 240
#define NRT_FRAME_BYTES   (NRT_SCREEN_WIDTH * NRT_SCREEN_HEIGHT)

typedef struct nrt_machine nrt_machine;

/* Immutable ROM views. The memory must outlive the machine. */
typedef struct nrt_cart {
    const uint8_t *prg;       /* 16384 or 32768 bytes (a 16 KB image is mirrored at $C000) */
    uint32_t       prg_size;
    const uint8_t *chr;       /* exactly 8192 bytes of CHR ROM */
    uint32_t       chr_size;
    uint8_t        vertical_mirroring;
} nrt_cart;

/* Attached controllers. Pad bits: A=1 B=2 Select=4 Start=8 Up=0x10 Down=0x20 Left=0x40 Right=0x80. */
typedef struct nrt_ports {
    uint8_t port_enabled[2];  /* standard pad plugged into $4016 / $4017 */
    uint8_t four_score;       /* Four Score adapter */
    uint8_t famicom_4player;  /* Famicom four-player adapter on the expansion port */
} nrt_ports;

/* Everything a debugger or the parity harness may look at. Pure observation: filling it has no side effects. */
typedef struct nrt_state {
    uint16_t pc;
    uint8_t  a, x, y, s, p;           /* p is architectural: bit 5 set, bit 4 clear */
    uint64_t cycles;                  /* CPU cycles since power-on, before the instruction at pc */
    uint8_t  ctrl, mask, status, oamaddr;
    uint16_t v, t;
    uint8_t  fine_x, w, readbuf;
    uint8_t  vertical_mirroring;
    uint8_t  data_bus, ppu_latch;
    uint8_t  apu_regs[0x18];          /* last value written to $4000-$4017 */
    uint8_t  apu_internal[0x20];      /* CPU-visible APU internals, layout in nes1 doc/parity_spec.md "APUX layout" */
    uint8_t  io[0x10];                /* controller serial state, nes1 doc/state_dump.md "IO layout" */
    uint8_t  irq[8];                  /* pending interrupt state, nes1 doc/state_dump.md "IRQ layout" */
} nrt_state;

/* Observation points, called from inside the scheduler. Any pointer may be NULL. */
typedef struct nrt_hooks {
    void *ctx;
    /* Before every instruction executes (after interrupt entry): the same point nes1 and FCEUX observe. */
    void (*on_instruction)(void *ctx, nrt_machine *m);
    /* After all work of visible scanline sl (0-239) is done. */
    void (*on_scanline_end)(void *ctx, nrt_machine *m, int sl);
} nrt_hooks;

/* Bytes of storage one machine needs, and its alignment. */
size_t nrt_machine_size(void);
size_t nrt_machine_align(void);

/* Build a machine in caller storage and power it on. Returns NULL for an unsupported cart or bad storage.
 * frame: NRT_FRAME_BYTES of palette-index pixels written line by line (bits 0-5 NES colour, bits 6-7 emphasis
 * flag), or NULL to skip pixel output. */
nrt_machine *nrt_machine_init(void *storage, size_t storage_size, const nrt_cart *cart, uint8_t *frame);

void nrt_set_ports(nrt_machine *m, const nrt_ports *ports);
void nrt_set_pads(nrt_machine *m, const uint8_t pads[4]);
void nrt_set_hooks(nrt_machine *m, const nrt_hooks *hooks);

/* The RESET button (RAM kept) and a full power cycle (RAM pattern restored). Both take effect at once. */
void nrt_soft_reset(nrt_machine *m);
void nrt_power_cycle(nrt_machine *m);

/* Emulate one frame: two "dead" frames without vblank follow every reset or power cycle. */
void nrt_run_frame(nrt_machine *m);

/* Test hook: continue at pc with the power-on registers of the nestest start state (P=$24, S=$FD). */
void nrt_debug_set_pc(nrt_machine *m, uint16_t pc);

/* Observation. */
void nrt_get_state(const nrt_machine *m, nrt_state *out);
int  nrt_current_scanline(const nrt_machine *m);            /* visible line being emulated, or -1 */
const uint8_t *nrt_ram(const nrt_machine *m);               /* 0x800 bytes */
const uint8_t *nrt_wram(const nrt_machine *m);              /* 0x2000 bytes, CPU $6000-$7FFF */
const uint8_t *nrt_nametable_ram(const nrt_machine *m);     /* 0x800 bytes, physical */
const uint8_t *nrt_palette_ram(const nrt_machine *m);       /* 0x20 bytes, mirrors resolved */
const uint8_t *nrt_oam(const nrt_machine *m);               /* 0x100 bytes */
void nrt_logical_nametables(const nrt_machine *m, uint8_t out[0x1000]);   /* $2000-$2FFF as the PPU addresses them */
uint8_t nrt_cpu_peek(const nrt_machine *m, uint16_t addr);  /* side-effect free CPU-visible byte (RAM, WRAM, PRG; else 0) */

#ifdef __cplusplus
}
#endif

#endif
