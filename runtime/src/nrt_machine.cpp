// nrt_machine.cpp - the frame scheduler and the C API.
//
// The scheduler is nes1's verified frame loop (parity spec O-9, O-25, O-28, O-34, O-38): time is kept in PPU dots,
// an instruction starts whenever any budget is left, and the overshoot of the last instruction is carried into the
// next slice. Per frame: post-render line, vblank + NMI, the rest of vblank, the pre-render line and 240 visible
// lines in the slices 256 + 6 + 63 + 16 dots.
#include "nrt_internal.hpp"

// Interrupt entry costs 7 CPU cycles of the budget.
static const int kDotsPerCpuCycle = 3;

void nrt_machine::run_ppu(int dots)
{
    ppu_budget += dots;
    while (ppu_budget > 0) {
        // Interrupt entry is checked before every instruction (spec O-25, O-28).
        if (cpu.nmi_state) {
            // An NMI armed by a $2000 write is converted at the next boundary and taken at the one after.
            if (cpu.nmi_state == 1) cpu.nmi_state = 2;
            else if (cpu.take_pending_nmi()) {
                ppu_budget -= kDotsPerCpuCycle * 7;
                if (ppu_budget <= 0) break;
            }
        } else if (cpu.irq_pending()) {
            cpu.irq();
            ppu_budget -= kDotsPerCpuCycle * 7;
            if (ppu_budget <= 0) break;
        }
        if (hooks.on_instruction) hooks.on_instruction(hooks.ctx, this);
        const uint64_t before = cpu.cycle;
        cpu.step();
        ppu_budget -= kDotsPerCpuCycle * (int)(cpu.cycle - before);
    }
}

void nrt_machine::run_frame()
{
    // The first two frames after power-on or reset are "ppudead": no vblank flag and no NMI, so the program stays
    // in its vblank wait loop; the frame buffer holds the all-$80 power-up picture (spec O-4).
    if (ppudead > 0) {
        --ppudead;
        run_ppu(262 * 341);
        if (frame) memset(frame, 0x80, NRT_FRAME_BYTES);
        return;
    }

    run_ppu(341);                 // post-render line
    ppu.raise_vblank();
    run_ppu(12);                  // NMI timing window
    if (ppu.nmi_enabled() && cpu.nmi())
        ppu_budget -= kDotsPerCpuCycle * 7;   // the entry's 7 cycles come out of the vblank budget
    run_ppu(6808);                // rest of vblank: (262-242)*341 - 12
    ppu.end_vblank();

    // Pre-render line: 256 + 69 + (16 - kook) dots; line 0 begins at dot 325, where V is loaded from T.
    run_ppu(256);
    run_ppu(69);
    ppu.copy_t_to_v();
    ppu.line_start_frame((int64_t)cpu.cycle * kDotsPerCpuCycle + ppu_budget, -ppu_budget);
    run_ppu(16 - kook);
    kook ^= 1;
    ppu.begin_frame();

    // 240 visible lines of 341 dots in four slices (256 + 6 + 63 + 16); the line was started at the end of the
    // previous line's third slice and its tiles are drawn lazily as the CPU touches PPU registers (O-19).
    ppu.vis_scanline = true;
    for (int sl = 0; sl < 240; ++sl) {
        cur_scanline = sl;
        run_ppu(256);
        ppu.line_end256(sl);
        run_ppu(6);
        ppu.copy_horizontal_bits();
        run_ppu(63);
        if (sl + 1 < 240)
            ppu.line_next(sl + 1, (int64_t)cpu.cycle * kDotsPerCpuCycle + ppu_budget, -ppu_budget);
        else
            ppu.carry_sprite0();
        if (sl == 239) ppu.vis_scanline = false;   // the tail of line 239 already counts as line 240
        run_ppu(16);
        if (hooks.on_scanline_end) hooks.on_scanline_end(hooks.ctx, this, sl);
        cur_scanline = -1;
    }
}

extern "C" {

size_t nrt_machine_size(void) { return sizeof(nrt_machine); }
size_t nrt_machine_align(void) { return alignof(nrt_machine); }

nrt_machine *nrt_machine_init(void *storage, size_t storage_size, const nrt_cart *cart, uint8_t *frame)
{
    if (!storage || storage_size < sizeof(nrt_machine) || ((uintptr_t)storage % alignof(nrt_machine)) != 0) return NULL;
    if (!cart || !cart->prg || !cart->chr) return NULL;
    if ((cart->prg_size != 16384 && cart->prg_size != 32768) || cart->chr_size != 8192) return NULL;

    nrt_machine *m = (nrt_machine *)storage;
    memset(m, 0, sizeof(*m));
    m->frame = frame;
    m->ppudead = 2;
    m->cur_scanline = -1;
    m->apu.init(cart->prg, cart->prg_size - 1);
    m->ppu.init(cart->chr, cart->vertical_mirroring != 0, frame, &m->cpu);
    m->cpu.init(cart->prg, cart->prg_size - 1, &m->ppu, &m->apu);
    return m;
}

void nrt_set_ports(nrt_machine *m, const nrt_ports *ports)
{
    m->cpu.port_enabled[0] = ports->port_enabled[0] != 0;
    m->cpu.port_enabled[1] = ports->port_enabled[1] != 0;
    m->cpu.four_score = ports->four_score != 0;
    m->cpu.famicom4 = ports->famicom_4player != 0;
}

void nrt_set_pads(nrt_machine *m, const uint8_t pads[4]) { memcpy(m->cpu.pads, pads, 4); }

void nrt_set_hooks(nrt_machine *m, const nrt_hooks *hooks)
{
    if (hooks) m->hooks = *hooks;
    else memset(&m->hooks, 0, sizeof(m->hooks));
}

void nrt_soft_reset(nrt_machine *m)
{
    m->cpu.hot_reset();
    m->ppu.reset_registers();
    m->apu.reset();
    m->kook = 0;
    m->ppudead = 2;
}

void nrt_power_cycle(nrt_machine *m)
{
    m->cpu.power();
    m->ppu.power();
    m->apu.power();
    m->ppu_budget = 0;
    m->kook = 0;
    m->ppudead = 2;
}

void nrt_run_frame(nrt_machine *m) { m->run_frame(); }

void nrt_debug_set_pc(nrt_machine *m, uint16_t pc)
{
    m->cpu.rpc = pc;
    m->cpu.rp = 0x24;
    m->cpu.rsp = 0xFD;
    m->cpu.data_bus = 0;
}

void nrt_get_state(const nrt_machine *m, nrt_state *s)
{
    const nrt::Cpu &c = m->cpu;
    const nrt::Ppu &p = m->ppu;
    const nrt::Apu &a = m->apu;
    memset(s, 0, sizeof(*s));
    s->pc = c.rpc;
    s->a = c.ra; s->x = c.rx; s->y = c.ry; s->s = c.rsp; s->p = c.rp;
    s->cycles = c.cycle;
    s->ctrl = p.reg_ctrl; s->mask = p.reg_mask; s->status = p.reg_status; s->oamaddr = p.reg_oam_addr;
    s->v = p.v; s->t = p.t;
    s->fine_x = p.fine_x; s->w = p.w; s->readbuf = p.data_buf;
    s->vertical_mirroring = p.mirror_vertical;
    s->data_bus = c.data_bus; s->ppu_latch = p.io_latch;

    memcpy(s->apu_regs, a.reg_shadow, sizeof(s->apu_regs));
    uint8_t *z = s->apu_internal;
    for (int i = 0; i < 4; ++i) z[i] = a.ch[i].length;
    z[4] = a.fcnt; z[5] = a.irq_mode; z[6] = a.sirq; z[7] = a.ch_enable;
    for (int k = 0; k < 4; ++k) z[8 + k] = (uint8_t)((uint32_t)a.fhcnt >> (8 * k));
    z[12] = (uint8_t)a.dmc_addr;  z[13] = (uint8_t)(a.dmc_addr >> 8);
    z[14] = (uint8_t)a.dmc_size;  z[15] = (uint8_t)(a.dmc_size >> 8);
    z[16] = a.dmc_shift;  z[17] = a.dmc_bits;
    z[18] = (uint8_t)a.dmc_acc;     z[19] = (uint8_t)(a.dmc_acc >> 8);
    z[20] = (uint8_t)a.dmc_period;  z[21] = (uint8_t)(a.dmc_period >> 8);
    z[22] = a.dmc_have_sample;  z[23] = a.dmc_have_dma;
    z[24] = a.dmc_buf;  z[25] = a.dmc_dac;  z[26] = a.dmc_format;

    s->io[0] = c.joy_readbit[0];  s->io[1] = c.joy_readbit[1];
    s->io[2] = c.joy_fc_readbit[0];  s->io[3] = c.joy_fc_readbit[1];
    s->io[4] = c.last_strobe;
    s->irq[0] = c.nmi_state;
    s->irq[1] = a.irq_line() ? 1 : 0;
    s->irq[2] = c.jammed ? 1 : 0;
}

int nrt_current_scanline(const nrt_machine *m) { return m->cur_scanline; }
const uint8_t *nrt_ram(const nrt_machine *m) { return m->cpu.ram; }
const uint8_t *nrt_wram(const nrt_machine *m) { return m->cpu.wram; }
const uint8_t *nrt_nametable_ram(const nrt_machine *m) { return m->ppu.nt; }
const uint8_t *nrt_palette_ram(const nrt_machine *m) { return m->ppu.pal; }
const uint8_t *nrt_oam(const nrt_machine *m) { return m->ppu.oam; }

void nrt_logical_nametables(const nrt_machine *m, uint8_t out[0x1000])
{
    for (unsigned i = 0; i < 0x1000; ++i)
        out[i] = m->ppu.nt[m->ppu.nt_index((uint16_t)(0x2000 + i))];
}

uint8_t nrt_cpu_peek(const nrt_machine *m, uint16_t addr) { return m->cpu.rd8(addr); }

}  // extern "C"
