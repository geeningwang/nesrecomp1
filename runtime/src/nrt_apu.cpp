// nrt_apu.cpp - the CPU-visible part of the 2A03 APU: registers, frame sequencer, length counters, DMC fetches
// and IRQ flags. Cycle behaviour follows nes1's verified model (parity spec O-25 and O-41).
#include "nrt_internal.hpp"

namespace nrt {

static const uint8_t kLengthTable[32] = {
    10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
    12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30
};

static const int32_t kDmcPeriod[16] = { 428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54 };

void Apu::init(const uint8_t* prg_view, uint32_t mask)
{
    memset(this, 0, sizeof(*this));
    prg = prg_view;
    prg_mask = mask;
    fhcnt = kFrameStepUnits;
    dmc_acc = 1;
    dmc_period = 428;
}

void Apu::reset()
{
    for (int i = 0; i < 4; ++i) { ch[i].enabled = false; ch[i].length = 0; }
    fhcnt = kFrameStepUnits;
    fcnt = 0;
    irq_mode = 0;
    ch_enable = 0;
    sirq = 0;
    dmc_format = 0;
    dmc_dac = 0;
    dmc_addr_latch = dmc_size_latch = 0;
    dmc_addr = 0;
    dmc_size = 0;
    dmc_shift = dmc_bits = 0;
    dmc_have_sample = dmc_have_dma = false;
    dmc_acc = 1;
}

// A power cycle restores everything except the DMC DMA byte, which FCEUX keeps.
void Apu::power()
{
    const uint8_t dma_byte = dmc_buf;
    init(prg, prg_mask);
    dmc_buf = dma_byte;
}

void Apu::write(uint16_t addr, uint8_t val)
{
    if (addr >= 0x4000 && addr <= 0x4017) reg_shadow[addr - 0x4000] = val;
    if (addr <= 0x400F) {
        Channel& c = ch[(addr - 0x4000) >> 2];
        const unsigned reg = addr & 3;
        if (reg == 0)
            c.halt = ((addr - 0x4000) >> 2) == 2 ? ((val >> 7) & 1) != 0 : ((val >> 5) & 1) != 0;
        else if (reg == 3 && c.enabled)
            c.length = kLengthTable[val >> 3];
    }
    else if (addr == 0x4015)
    {
        ch_enable = val & 0x1F;
        for (int i = 0; i < 4; ++i) {
            ch[i].enabled = (val >> i) & 1;
            if (!ch[i].enabled) ch[i].length = 0;
        }
        if (val & 0x10) { if (!dmc_size) dmc_prepare(); }
        else dmc_size = 0;
        sirq &= (uint8_t)~0x80;
    }
    else if (addr == 0x4010)
    {
        dmc_period = kDmcPeriod[val & 0xF];
        if (!(val & 0x80)) sirq &= (uint8_t)~0x80;
        dmc_format = val;
    }
    else if (addr == 0x4011) dmc_dac = val & 0x7F;
    else if (addr == 0x4012) dmc_addr_latch = val;
    else if (addr == 0x4013) dmc_size_latch = val;
    else if (addr == 0x4017)
    {
        const uint8_t mode = (val & 0xC0) >> 6;
        fcnt = 0;
        if (mode & 2)
            frame_update();   // 5-step mode clocks immediately (with the previous mode's IRQ rule)
        fcnt = 1;
        fhcnt = kFrameStepUnits;
        sirq &= (uint8_t)~0x40;
        irq_mode = mode;
    }
}

uint8_t Apu::read_status()
{
    uint8_t s = 0;
    for (int i = 0; i < 4; ++i)
        if (ch[i].length > 0) s |= (uint8_t)(1 << i);
    if (dmc_size > 0) s |= 0x10;
    s |= sirq;
    sirq &= (uint8_t)~0x40;
    return s;
}

void Apu::dmc_prepare()
{
    dmc_addr = 0x4000u + ((uint32_t)dmc_addr_latch << 6);
    dmc_size = ((int32_t)dmc_size_latch << 4) + 1;
}

void Apu::frame_update()
{
    if (fcnt == 0 && !(irq_mode & 3))
        sirq |= 0x40;
    if (fcnt == 3 && (irq_mode & 2))
        fhcnt += kFrameStepUnits;
    if (!(fcnt & 1)) clock_half_frame();
    fcnt = (uint8_t)((fcnt + 1) & 3);
}

void Apu::clock_half_frame()
{
    for (int i = 0; i < 4; ++i)
        if (!ch[i].halt && ch[i].length > 0) --ch[i].length;
}

// Advances the APU by `cycles` CPU cycles before an instruction. Returns the CPU cycles stolen by a DMC fetch.
uint32_t Apu::advance(uint32_t cycles, uint8_t& bus)
{
    uint32_t stall = 0;

    fhcnt -= (int32_t)cycles * 48;
    if (fhcnt <= 0)
    {
        frame_update();
        fhcnt += kFrameStepUnits;
    }

    // DMC sample fetch: one byte per call, stealing 4 CPU cycles.
    if (dmc_size && !dmc_have_dma)
    {
        stall = 4;
        dmc_buf = prg[(0x8000u + dmc_addr - 0x8000u) & prg_mask];
        bus = dmc_buf;
        dmc_have_dma = true;
        dmc_addr = (dmc_addr + 1) & 0x7FFF;
        if (--dmc_size == 0)
        {
            if (dmc_format & 0x40)      dmc_prepare();
            else if (dmc_format & 0x80) sirq |= 0x80;
        }
    }

    dmc_acc -= (int32_t)cycles;
    while (dmc_acc <= 0)
    {
        if (dmc_have_sample)
        {
            const uint8_t old = dmc_dac;
            dmc_dac = (uint8_t)(dmc_dac + (((dmc_shift & 1) << 2) - 2));
            if (dmc_dac & 0x80) dmc_dac = old;
        }
        dmc_acc += dmc_period;
        dmc_bits = (uint8_t)((dmc_bits + 1) & 7);
        dmc_shift >>= 1;
        if (dmc_bits == 0)
        {
            if (!dmc_have_dma) dmc_have_sample = false;
            else { dmc_have_sample = true; dmc_shift = dmc_buf; dmc_have_dma = false; }
        }
    }
    return stall;
}

}  // namespace nrt
