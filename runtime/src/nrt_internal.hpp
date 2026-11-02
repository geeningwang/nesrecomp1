// nrt_internal.hpp - machine state shared by the runtime's components. Not part of the public API.
//
// Derived from the exercised paths of nes1 (MIT) - the lazy line renderer, the interpreter and the CPU-visible
// APU - restructured so that a machine owns all of its state (no globals, no allocation, no host headers).
#pragma once

#include <stdint.h>
#include <string.h>

#include "nrt.h"

namespace nrt {

struct Cpu;
struct Ppu;
struct Apu;

// One length-counter channel: pulse 1, pulse 2, triangle, noise. Envelope, sweep and timer state only
// matter for audio synthesis, which this milestone does not provide.
struct Channel {
    bool    enabled;
    bool    halt;      // length counter halt ($4000/$4004/$400C bit 5, $4008 bit 7)
    uint8_t length;
};

struct Apu {
    uint8_t  reg_shadow[0x18];
    Channel  ch[4];
    int32_t  fhcnt;            // time to the next frame-sequencer step in 1/48 CPU cycle units
    uint8_t  fcnt;
    uint8_t  irq_mode;         // $4017 >> 6
    uint8_t  ch_enable;        // $4015 bits 0-4 as last written
    uint8_t  sirq;             // bit 6 frame IRQ flag, bit 7 DMC IRQ flag
    uint8_t  dmc_format, dmc_dac, dmc_addr_latch, dmc_size_latch;
    uint32_t dmc_addr;
    int32_t  dmc_size;
    uint8_t  dmc_shift, dmc_bits, dmc_buf;
    bool     dmc_have_sample, dmc_have_dma;
    int32_t  dmc_acc, dmc_period;

    const uint8_t* prg;        // DMC sample fetches read CPU $8000-$FFFF through this view
    uint32_t       prg_mask;

    static const int32_t kFrameStepUnits = 14915 * 24;

    void init(const uint8_t* prg_view, uint32_t mask);
    void reset();
    void power();
    void write(uint16_t addr, uint8_t val);
    uint8_t read_status();
    bool irq_line() const { return (sirq & 0xC0) != 0; }
    uint32_t advance(uint32_t cycles, uint8_t& bus);

private:
    void dmc_prepare();
    void frame_update();
    void clock_half_frame();
};

struct Ppu {
    uint8_t  reg_ctrl, reg_mask, reg_status, reg_oam_addr, io_latch, oam_spl;
    uint16_t v, t;
    uint8_t  fine_x, w, data_buf;
    bool     mirror_vertical;
    bool     vis_scanline;     // FCEUX's `scanline < 240`, set by the frame scheduler (parity spec O-34)

    uint8_t  nt[0x800];        // physical nametable RAM
    uint8_t  pal[0x20];        // palette RAM with the $3F10/14/18/1C mirrors resolved at write time
    uint8_t  oam[0x100];
    const uint8_t* chr;        // 8 KB CHR ROM

    // Lazy line renderer state (parity spec O-19)
    bool     rl_on;
    int      rl_sl, rl_first, rl_px;
    uint8_t  bg_pipeline[2][8];
    bool     rl_tofix;
    int64_t  rl_origin;
    uint8_t  ln_col[NRT_SCREEN_WIDTH];
    uint8_t  ln_op[NRT_SCREEN_WIDTH];
    uint8_t  spr_flag[NRT_SCREEN_WIDTH], spr_col[NRT_SCREEN_WIDTH];
    uint8_t  nxt_flag[NRT_SCREEN_WIDTH], nxt_c2[NRT_SCREEN_WIDTH], nxt_pal[NRT_SCREEN_WIDTH];
    int      sphitx;
    uint8_t  sphitdata;
    int      nxt_sphitx;
    uint8_t  nxt_sphitdata;

    uint8_t* fb;               // host frame buffer or NULL
    Cpu*     cpu;

    void init(const uint8_t* chr_rom, bool vertical, uint8_t* frame, Cpu* c);
    void reset_registers();
    void power();

    uint8_t cpu_read(uint16_t addr);
    void    cpu_write(uint16_t addr, uint8_t val);
    bool    nmi_enabled() const { return (reg_ctrl & 0x80) != 0; }
    void    raise_vblank();
    void    end_vblank();      // vblank, sprite-0 and overflow flags cleared together
    void    begin_frame() { reg_status &= (uint8_t)~0x40; }
    void    write_oam_byte(uint8_t val);
    void    oam_dma_byte(uint8_t val) { write_oam_byte(val); }

    uint16_t nt_index(uint16_t addr) const;
    uint8_t  mem_read(uint16_t addr) const;
    void     mem_write(uint16_t addr, uint8_t val);

    void copy_horizontal_bits();
    void copy_t_to_v();
    void line_start_frame(int64_t origin_dots, int overshoot);
    void line_next(int sl, int64_t origin_dots, int overshoot);
    void carry_sprite0() { resolve_sprites(); }
    void line_end256(int sl);
    void sync_from_cpu();

private:
    void line_begin(int sl, int64_t origin_dots, int lastpixel);
    void line_sync(int lastpixel);
    void bg_fetch_tile();
    void bg_output_tile(int pos);
    void bg_fill_tile(int pos);
    void check_sprite_hit(int lastpixel);
    void prepare_sprites(int sl);
    void resolve_sprites();
    void y_increment();
};

struct Cpu {
    uint16_t rpc;
    uint8_t  rsp, ra, rx, ry, rp;
    uint8_t  ram[0x800];
    uint8_t  wram[0x2000];
    const uint8_t* prg;
    uint32_t prg_mask;
    uint8_t  data_bus;
    uint8_t  pending_instruction_cycles;
    uint8_t  rpi;              // P as of the previous instruction start: the I flag the IRQ check sees
    bool     jammed;
    uint8_t  nmi_state;        // 0 none, 1 armed by a $2000 write, 2 due at the next instruction boundary
    uint64_t cycle;
    uint64_t apu_cycle;

    uint8_t  pads[4];
    bool     four_score, famicom4;
    bool     port_enabled[2];
    uint8_t  joy_readbit[2], joy_fc_readbit[2];
    uint8_t  last_strobe;

    Ppu* ppu;
    Apu* apu;

    void init(const uint8_t* prg_view, uint32_t mask, Ppu* p, Apu* a);
    void reset();              // power-on reset
    void hot_reset();          // the RESET button
    void power();
    bool step();               // false for an undefined opcode
    bool nmi();
    void arm_delayed_nmi() { if (nmi_state == 0) nmi_state = 1; }
    bool take_pending_nmi();
    bool irq_pending() const { return apu->irq_line() && !(rpi & 0x04) && !jammed; }
    void irq();

    uint8_t rd8(uint16_t a) const;      // side-effect free view of RAM / WRAM / PRG, 0 elsewhere
    uint16_t rd16(uint16_t a) const { return (uint16_t)(rd8(a) | (rd8((uint16_t)(a + 1)) << 8)); }
    uint8_t mem_read(uint16_t addr);
    void    mem_write(uint16_t addr, uint8_t val);
};

}  // namespace nrt

struct nrt_machine {
    nrt::Cpu  cpu;
    nrt::Ppu  ppu;
    nrt::Apu  apu;
    nrt_hooks hooks;
    uint8_t*  frame;
    int       ppu_budget;      // CPU budget in PPU dots; negative = overshoot debt carried to the next slice (O-9)
    int       kook;            // alternating pre-render length (0 -> 16 dots, 1 -> 15 dots)
    int       ppudead;         // frames still to run without vblank after power-on or reset
    int       cur_scanline;    // visible line being emulated, or -1

    void run_ppu(int dots);
    void run_frame();
};
