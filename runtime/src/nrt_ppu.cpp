// nrt_ppu.cpp - the 2C02 as seen by the CPU, plus nes1's lazy line renderer (parity spec O-19 .. O-40).
//
// Every visible line is drawn lazily: the CPU time of each PPU register access is converted to a dot position
// in the line and the line is brought up to date with the register state from before the access. Output is one
// palette-index byte per pixel (bits 0-5 NES colour, bits 6-7 an emphasis flag), written when a line is
// finished; colour conversion is the host's business.
#include "nrt_internal.hpp"

namespace nrt {

static inline uint8_t fb_index_byte(uint8_t nes_color, unsigned emph_idx)
{
    const uint8_t flag = (emph_idx == 7) ? 0xC0 : (emph_idx ? 0x40 : 0x80);
    return (uint8_t)((nes_color & 0x3F) | flag);
}

void Ppu::init(const uint8_t* chr_rom, bool vertical, uint8_t* frame, Cpu* c)
{
    memset(this, 0, sizeof(*this));
    chr = chr_rom;
    mirror_vertical = vertical;
    fb = frame;
    cpu = c;
    sphitx = 0x100;
    nxt_sphitx = 0x100;
}

// FCEUPPU_Reset(): registers, latches and address/toggle state are cleared; VRAM, OAM, palette and fine X stay (O-41).
void Ppu::reset_registers()
{
    reg_ctrl = reg_mask = reg_status = reg_oam_addr = 0;
    io_latch = 0;
    oam_spl = 0;
    v = t = 0;
    w = 0;
    data_buf = 0;
}

void Ppu::power()
{
    memset(nt, 0, sizeof(nt));
    memset(pal, 0, sizeof(pal));
    memset(oam, 0, sizeof(oam));
    reset_registers();
}

// Physical nametable offset of a nametable address ($2000-$2FFF; $3000-$3EFF mirror it).
uint16_t Ppu::nt_index(uint16_t addr) const
{
    addr &= 0x2FFF;
    const unsigned nt_sel = (unsigned)(addr - 0x2000) / 0x400;
    const unsigned off = (unsigned)(addr - 0x2000) % 0x400;
    return (uint16_t)((mirror_vertical ? (nt_sel & 1) : ((nt_sel >> 1) & 1)) * 0x400 + off);
}

static inline unsigned pal_index(uint16_t addr)
{
    unsigned i = addr & 0x1F;
    if ((i & 0x13) == 0x10) i &= 0x0F;   // $3F10/14/18/1C alias $3F00/04/08/0C
    return i;
}

uint8_t Ppu::mem_read(uint16_t addr) const
{
    addr &= 0x3FFF;
    if (addr >= 0x3F00) return pal[pal_index(addr)];
    if (addr >= 0x2000) return nt[nt_index(addr)];
    return chr[addr];
}

void Ppu::mem_write(uint16_t addr, uint8_t val)
{
    addr &= 0x3FFF;
    if (addr >= 0x3F00) { pal[pal_index(addr)] = val; return; }
    if (addr >= 0x2000) { nt[nt_index(addr)] = val; return; }
    // $0000-$1FFF is CHR ROM: PPU writes are ignored (spec O-32)
}

uint8_t Ppu::cpu_read(uint16_t addr)
{
    addr = (uint16_t)(0x2000 + (addr & 0x0007));
    sync_from_cpu();   // the picture up to this point is drawn before the register is read

    switch (addr)
    {
    case 0x2002:  // PPUSTATUS: bits 7-5 are status, bits 4-0 come from the I/O latch
    {
        const uint8_t result = (uint8_t)((reg_status & 0xE0) | (io_latch & 0x1F));
        reg_status &= 0x7F;
        w = 0;
        io_latch = result;
        return result;
    }
    case 0x2007:  // PPUDATA
    {
        uint8_t result = data_buf;
        if ((v & 0x3FFF) >= 0x3F00) {
            // Palette reads are not buffered and leave the I/O latch alone; the buffer is loaded from the
            // nametable underneath the palette, and greyscale masks the value read (spec O-42).
            result = (uint8_t)(mem_read(v) & ((reg_mask & 1) ? 0x30 : 0xFF));
            data_buf = mem_read((uint16_t)((v & 0x3FFF) - 0x1000));
        } else {
            data_buf = mem_read(v);
            io_latch = result;
        }
        if ((reg_mask & 0x18) && vis_scanline)
        {
            // A read during rendering steps V like a Y increment instead of by 1 or 32 (spec O-34).
            uint16_t rad = v;
            if ((rad & 0x7000) == 0x7000) {
                rad ^= 0x7000;
                if ((rad & 0x3E0) == 0x3A0)      rad ^= 0xBA0;
                else if ((rad & 0x3E0) == 0x3E0) rad ^= 0x3E0;
                else                             rad = (uint16_t)(rad + 0x20);
            } else
                rad = (uint16_t)(rad + 0x1000);
            v = rad;
        }
        else
            v = (uint16_t)(v + ((reg_ctrl & 0x04) ? 32 : 1));
        return result;
    }
    default:      // write-only registers read back the I/O latch
        return io_latch;
    }
}

void Ppu::cpu_write(uint16_t addr, uint8_t val)
{
    addr = (uint16_t)(0x2000 + (addr & 0x0007));
    // The picture up to this point is drawn with the old state before a control, mask, scroll or address
    // write. Data writes ($2007) and OAM writes do not trigger a catch-up (spec O-19).
    if (addr == 0x2000 || addr == 0x2001 || addr == 0x2005 || addr == 0x2006)
        sync_from_cpu();
    io_latch = val;   // every write to a PPU register drives the latch

    switch (addr)
    {
    case 0x2000:  // PPUCTRL
        // Turning NMI on while the vblank flag is set raises an NMI right away (spec O-38).
        if (!(reg_ctrl & 0x80) && (val & 0x80) && (reg_status & 0x80) && cpu)
            cpu->arm_delayed_nmi();
        reg_ctrl = val;
        t = (uint16_t)((t & 0xF3FF) | ((val & 0x03) << 10));
        break;
    case 0x2001:  // PPUMASK
        reg_mask = val;
        break;
    case 0x2003:  // OAMADDR
        reg_oam_addr = val;
        oam_spl = val & 7;
        break;
    case 0x2004:  // OAMDATA
        write_oam_byte(val);
        break;
    case 0x2005:  // PPUSCROLL (2 writes)
        if (w == 0)
        {
            t = (uint16_t)((t & 0xFFE0) | (val >> 3));
            fine_x = val & 0x07;
            w = 1;
        }
        else
        {
            t = (uint16_t)((t & 0x8FFF) | ((val & 0x07) << 12));
            t = (uint16_t)((t & 0xFC1F) | ((val >> 3) << 5));
            w = 0;
        }
        break;
    case 0x2006:  // PPUADDR (2 writes)
        if (w == 0)
        {
            t = (uint16_t)((t & 0x00FF) | ((val & 0x3F) << 8));
            w = 1;
        }
        else
        {
            t = (uint16_t)((t & 0xFF00) | val);
            v = t;
            w = 0;
        }
        break;
    case 0x2007:  // PPUDATA
        mem_write(v, val);
        v = (uint16_t)(v + ((reg_ctrl & 0x04) ? 32 : 1));
        break;
    default:
        break;
    }
}

// Vblank starts: the flag is raised and OAMADDR and the OAM write counter are cleared (spec O-15).
void Ppu::raise_vblank()
{
    reg_status |= 0x80;
    reg_oam_addr = 0;
    oam_spl = 0;
}

// End of vblank: the VBL, sprite-0 and sprite-overflow flags are cleared together.
void Ppu::end_vblank()
{
    reg_status &= 0x1F;
}

// $2004 write as FCEUX performs it (spec O-10): while the write counter is below 8 the byte goes to
// OAM[counter] regardless of OAMADDR; afterwards it goes to OAM[OAMADDR] only if OAMADDR >= 8.
void Ppu::write_oam_byte(uint8_t val)
{
    io_latch = val;   // DMA writes go through $2004 and drive the I/O latch too (spec O-16)
    if (oam_spl >= 8) {
        if (reg_oam_addr >= 8)
            oam[reg_oam_addr] = val;
    } else {
        oam[oam_spl] = val;
    }
    ++reg_oam_addr;
    ++oam_spl;
}

void Ppu::copy_horizontal_bits()
{
    if ((reg_mask & 0x18) == 0) return;
    v = (uint16_t)((v & ~0x041F) | (t & 0x041F));
}

void Ppu::copy_t_to_v()
{
    if ((reg_mask & 0x18) == 0) return;
    v = t;
}

// Y increment of the VRAM address, as the real PPU does at the end of a rendered line.
void Ppu::y_increment()
{
    if ((reg_mask & 0x18) == 0) return;   // nothing happens while rendering is off
    uint16_t a = v;
    if ((a & 0x7000) != 0x7000) {
        a = (uint16_t)(a + 0x1000);       // fine Y
    } else {
        a &= (uint16_t)~0x7000;
        int y = (a & 0x03E0) >> 5;        // coarse Y
        if (y == 29)      { y = 0; a ^= 0x0800; }
        else if (y == 31) { y = 0; }
        else              { ++y; }
        a = (uint16_t)((a & ~0x03E0) | (y << 5));
    }
    v = a;
}

// Line 0 begins at dot 325 of the pre-render line: no sprites are drawn on it.
void Ppu::line_start_frame(int64_t origin_dots, int overshoot)
{
    memset(spr_flag, 0, sizeof(spr_flag));
    memset(nxt_flag, 0, sizeof(nxt_flag));
    // sphitx/sphitdata are left alone: sprite 0 of line 239 can still hit on line 0 (spec O-37).
    line_begin(0, origin_dots, overshoot);
}

// The next visible line begins at the end of the previous line's third slice: the sprites prepared at dot 256
// get their colours now and become the sprites of this line.
void Ppu::line_next(int sl, int64_t origin_dots, int overshoot)
{
    resolve_sprites();
    line_begin(sl, origin_dots, overshoot);
}

void Ppu::line_begin(int sl, int64_t origin_dots, int lastpixel)
{
    rl_on     = true;
    rl_sl     = sl;
    rl_first  = 0;
    rl_px     = 0;
    rl_tofix  = false;
    rl_origin = origin_dots;
    memset(ln_col, 0, sizeof(ln_col));
    memset(ln_op, 0, sizeof(ln_op));
    line_sync(lastpixel);   // the dots already consumed when the line starts count as well
    rl_tofix  = true;       // the Y increment is pending from here on
}

void Ppu::sync_from_cpu()
{
    if (!rl_on || !cpu) return;
    const int64_t now = (int64_t)(cpu->cycle + cpu->pending_instruction_cycles) * 3;
    const int lp = (int)(now - rl_origin);
    line_sync(lp < 0 ? 0 : lp);
}

void Ppu::line_sync(int lastpixel)
{
    if (!rl_on) return;
    int lasttile = lastpixel >> 3;
    // One more tile is fetched when sprite 0's hit pixel is about to be output (FCEUX quirk, spec O-37).
    if (sphitx != 0x100 && !(reg_status & 0x40) && sphitx < lastpixel - 16 && !(sphitx < (lasttile - 2) * 8))
        ++lasttile;
    if (lasttile > 34) lasttile = 34;
    if (lasttile - rl_first <= 0) return;
    // Background enabled: tile i is fetched in iteration i (advancing V, colours resolved now) and the pixels of
    // the tile fetched two iterations earlier are written at the write pointer. Background disabled: one
    // backdrop tile per iteration is written at the write pointer, so the pointer lags the fetch count by two
    // tiles only if enabled tiles were written before.
    const int first_before = rl_first;
    for (int i = rl_first; i < lasttile; ++i) {
        if (reg_mask & 0x18) {
            // Sprites alone also keep the tile pipeline (and V) running; their background pixels are replaced
            // by the backdrop below (spec O-35).
            if (i >= 2) { bg_output_tile(rl_px); ++rl_px; }
            bg_fetch_tile();
        } else {
            bg_fill_tile(rl_px);
            ++rl_px;
        }
    }
    if (reg_mask & 0x18) {
        const uint8_t backdrop = (uint8_t)(mem_read(0x3F00) & 0x3F);
        // Left 8 pixels masked in the background: the catch-up that reaches the first visible tile paints them.
        if (first_before <= 2 && 2 < lasttile && !(reg_mask & 0x02))
            for (int x = 0; x < 8; ++x) { ln_col[x] = backdrop; ln_op[x] = 0; }
        if (!(reg_mask & 0x08)) {
            int tstart = first_before - 2, tcount = lasttile - first_before;
            if (tstart < 0) { tcount += tstart; tstart = 0; }
            for (int x = tstart * 8; x < (tstart + tcount) * 8 && x < NRT_SCREEN_WIDTH; ++x) { ln_col[x] = backdrop; ln_op[x] = 0; }
        }
    }
    rl_first = lasttile;
    // A catch-up late in the line (spec O-21) performs the pending Y increment now, with the register state of
    // this moment, so the end-of-line step finds nothing left to do.
    if (rl_tofix && lastpixel >= 268) {
        y_increment();
        rl_tofix = false;
    }
    check_sprite_hit(lastpixel);
}

// Fetch pattern/palette indices with the live register state and advance V's coarse X. The two-tile pipeline
// survives disabled rendering; palette colours are resolved at output.
void Ppu::bg_fetch_tile()
{
    memcpy(bg_pipeline[0], bg_pipeline[1], sizeof(bg_pipeline[0]));
    uint16_t a = v;
    const int cx = a & 31, cy = (a >> 5) & 31, ntsel = (a >> 10) & 3, fy = (a >> 12) & 7;
    const uint16_t ntaddr = (uint16_t)(0x2000 | (ntsel << 10) | (cy << 5) | cx);
    const uint8_t name = nt[nt_index(ntaddr)];
    const uint16_t ataddr = (uint16_t)(0x23C0 | (ntsel << 10) | ((cy >> 2) << 3) | (cx >> 2));
    const uint8_t att = nt[nt_index(ataddr)];
    const int sh = ((cy & 2) << 1) | (cx & 2);
    const uint8_t hi = (uint8_t)((att >> sh) & 3);
    const unsigned base = (reg_ctrl & 0x10) ? 0x1000u : 0u;
    const uint8_t p0 = chr[base + name * 16u + fy];
    const uint8_t p1 = chr[base + name * 16u + fy + 8u];
    for (int i = 0; i < 8; ++i) {
        const uint8_t c2 = (uint8_t)(((p0 >> (7 - i)) & 1) | (((p1 >> (7 - i)) & 1) << 1));
        bg_pipeline[1][i] = c2 ? (uint8_t)((hi << 2) | c2) : 0;
    }
    if ((a & 0x1F) == 0x1F) a ^= 0x041F; else ++a;
    v = a;
}

// Fine X selects across adjacent fetched tiles; the output pointer still advances in eight-pixel chunks.
void Ppu::bg_output_tile(int pos)
{
    const int fx = fine_x;
    for (int i = 0; i < 8; ++i) {
        const int x = pos * 8 + i;
        if (x < 0 || x >= NRT_SCREEN_WIDTH) continue;
        const int source = (i + fx) >> 3;
        const int pixel = (i + fx) & 7;
        const uint8_t palette_index = bg_pipeline[source][pixel];
        ln_col[x] = (uint8_t)(pal[palette_index] & 0x3F);
        ln_op[x] = palette_index != 0;
    }
}

// Background disabled: the tile position gets the backdrop colour.
void Ppu::bg_fill_tile(int pos)
{
    const uint8_t backdrop = (uint8_t)(mem_read(0x3F00) & 0x3F);
    for (int i = 0; i < 8; ++i) {
        const int x = pos * 8 + i;
        if (x < 0 || x >= NRT_SCREEN_WIDTH) continue;
        ln_col[x] = backdrop;
        ln_op[x]  = 0;
    }
}

// Sprite-0 hit becomes visible once the pixel has been "output": the hit pixel must lie more than 16 dots
// before the current position (dots are counted from the line origin).
void Ppu::check_sprite_hit(int lastpixel)
{
    if (sphitx == 0x100) return;
    const int l = lastpixel - 16;
    for (int x = sphitx; x < sphitx + 8 && x < l; ++x) {
        if ((sphitdata & (0x80 >> (x - sphitx))) && ln_op[x] && x < 255) {
            reg_status |= 0x40;
            sphitx = 0x100;
            break;
        }
    }
}

// Dot 256 of visible line sl.
void Ppu::line_end256(int sl)
{
    line_sync(272);
    if (rl_tofix) {
        y_increment();
        rl_tofix = false;
    }
    // line_sync(272) already outputs the full visible row; fetched tail tiles remain in the pipeline.
    check_sprite_hit(272);
    rl_on = false;

    // Composite this row: background, then the sprites prepared during the previous line.
    const unsigned emph = (reg_mask >> 5) & 7;
    const bool spr_on = (reg_mask & 0x10) != 0;
    if (fb) {
        uint8_t* row = fb + (size_t)sl * NRT_SCREEN_WIDTH;
        for (int x = 0; x < NRT_SCREEN_WIDTH; ++x) {
            uint8_t col = ln_col[x];
            if (spr_on && (spr_flag[x] & 1) && !((spr_flag[x] & 2) && ln_op[x]) && (x >= 8 || (reg_mask & 0x04)))
                col = spr_col[x];
            if (reg_mask & 0x01) col &= 0x30;
            row[x] = fb_index_byte(col, emph);
        }
    }
    prepare_sprites(sl);
}

// Sprite evaluation at dot 256 of line sl for line sl+1: the first 8 sprites with 0 <= sl - Y < height, a 9th
// sets the overflow flag. Pattern data is read now; colours are resolved at the end of the line.
void Ppu::prepare_sprites(int sl)
{
    memset(nxt_flag, 0, sizeof(nxt_flag));
    nxt_sphitx = 0x100;
    nxt_sphitdata = 0;
    if ((reg_mask & 0x18) == 0) return;

    const bool tall = (reg_ctrl & 0x20) != 0;     // 8x16 sprites
    const unsigned height = tall ? 16u : 8u;
    int kept[8];
    int ns = 0;
    for (int n = 0; n < 64; ++n) {
        if ((unsigned)(sl - (int)oam[n * 4]) >= height) continue;
        if (ns < 8) kept[ns++] = n;
        else { reg_status |= 0x20; break; }
    }

    for (int k = ns - 1; k >= 0; --k) {            // lowest sprite number is drawn last, so it wins
        const int n = kept[k];
        const uint8_t* sp = oam + n * 4;
        const int row = sl - (int)sp[0];
        const uint8_t attr = sp[2];
        // Pattern address: 8x16 sprites take the table from bit 0 of the tile number and fetch the bottom tile
        // for rows 8-15 (spec O-36); flipping vertically mirrors all 16 rows.
        unsigned vadr = tall ? (unsigned)(((sp[1] & 1) << 12) + ((sp[1] & 0xFE) << 4))
                             : (unsigned)((sp[1] << 4) + ((reg_ctrl & 0x08) ? 0x1000 : 0));
        if (attr & 0x80) vadr = vadr + 7 - row + (tall ? 16 : 0) - (row & 8);
        else             vadr = vadr + row + (row & 8);
        const uint8_t p0 = chr[vadr & 0x1FFF];
        const uint8_t p1 = chr[(vadr + 8) & 0x1FFF];
        uint8_t mask = 0;
        for (int i = 0; i < 8; ++i) {
            const int b = (attr & 0x40) ? i : 7 - i;
            const uint8_t c2 = (uint8_t)(((p0 >> b) & 1) | (((p1 >> b) & 1) << 1));
            if (!c2) continue;
            mask |= (uint8_t)(0x80 >> i);
            const int x = sp[3] + i;
            if (x >= NRT_SCREEN_WIDTH) continue;
            nxt_flag[x] = (uint8_t)(1 | ((attr & 0x20) ? 2 : 0));
            nxt_c2[x]   = c2;
            nxt_pal[x]  = (uint8_t)(0x10 | ((attr & 3) << 2));
        }
        if (n == 0 && mask) { nxt_sphitx = sp[3]; nxt_sphitdata = mask; }
    }
}

void Ppu::resolve_sprites()
{
    // Sprites only reach the next line while they are enabled at this moment (spec O-37).
    const bool on = (reg_mask & 0x10) != 0;
    for (int x = 0; x < NRT_SCREEN_WIDTH; ++x) {
        spr_flag[x] = on ? nxt_flag[x] : 0;
        if (spr_flag[x])
            spr_col[x] = (uint8_t)(pal[nxt_pal[x] + nxt_c2[x]] & 0x3F);
    }
    sphitx = (on && !(reg_status & 0x40)) ? nxt_sphitx : 0x100;   // no new hit is armed once the flag is set
    sphitdata = nxt_sphitdata;
}

}  // namespace nrt
