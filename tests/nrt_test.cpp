// nrt_test - headless harness for the runtime core. It takes the command line of nes1's and FCEUX's dump mode
// (`<rom> --dump DIR --frames N --input FILE ...`, see nes1 doc/state_dump.md) and writes the same NESDUMP 3
// files, so tool/compare_dumps.py can compare it with either of them.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <set>
#include <vector>

#include "common/nesdump.h"
#include "nrt.h"

#ifdef NRT_EMBEDDED_GAME
// Built together with the output of tools/gen_insn.py + gen_registry.py: the games' PRG/CHR images and generated
// code are linked in and found by the identity of the ROM given on the command line.
extern "C" const nrt_game_entry nrt_game_registry[];
extern "C" const unsigned nrt_game_registry_count;
#endif

namespace {

struct Harness {
    std::set<uint16_t> interpreted_pcs;   // PCs that ran in the interpreter although generated code is attached
    nrt_machine*      machine = nullptr;
    nesdump::Recorder rec;
    std::vector<uint8_t> prg32, chr;   // dump views: CPU $8000-$FFFF and PPU $0000-$1FFF
    std::vector<uint8_t> frame;
    int frame_no = 0;
    bool track_coverage = false;

    void fill(nesdump::State& s) const
    {
        nrt_state st;
        nrt_get_state(machine, &st);
        s.pc = st.pc; s.a = st.a; s.x = st.x; s.y = st.y; s.s = st.s; s.p = st.p;
        s.cycles = st.cycles;
        s.ctrl = st.ctrl; s.mask = st.mask; s.status = st.status; s.oamaddr = st.oamaddr;
        s.v = st.v; s.t = st.t;
        s.fine_x = st.fine_x; s.w = st.w; s.readbuf = st.readbuf;
        s.mirror = st.vertical_mirroring ? 'V' : 'H';
        s.db = st.data_bus; s.ppu_latch = st.ppu_latch;
        memcpy(s.apu, st.apu_regs, sizeof(s.apu));
        memcpy(s.apuz, st.apu_internal, sizeof(s.apuz));
        memcpy(s.io, st.io, sizeof(s.io));
        memcpy(s.irq, st.irq, sizeof(s.irq));
        memcpy(s.ram, nrt_ram(machine), sizeof(s.ram));
        memcpy(s.wram, nrt_wram(machine), sizeof(s.wram));
        memcpy(s.nt, nrt_nametable_ram(machine), sizeof(s.nt));
        nrt_logical_nametables(machine, s.ntv);
        memcpy(s.pal, nrt_palette_ram(machine), sizeof(s.pal));
        memcpy(s.oam, nrt_oam(machine), sizeof(s.oam));
        s.chr = chr.data();
        s.prg = prg32.data();
        s.fb = frame.data();
    }

    void emit(const char* event, int pc_trigger, int pc_hit, int insn_no)
    {
        nesdump::State s;
        memset(&s, 0, sizeof(s));
        s.event = event;
        s.frame = frame_no;
        s.scanline = nrt_current_scanline(machine);
        s.pc_trigger = pc_trigger;
        s.pc_hit = pc_hit;
        s.insn_no = insn_no;
        fill(s);
        rec.write_event(s);
    }

    static void on_instruction(void* ctx, nrt_machine* m)
    {
        Harness& h = *static_cast<Harness*>(ctx);
        nrt_state st;
        nrt_get_state(m, &st);
        if (h.track_coverage && !nrt_has_code(m, st.pc)) h.interpreted_pcs.insert(st.pc);
        nesdump::Insn i;
        i.pc = st.pc;  i.op = nrt_cpu_peek(m, st.pc);
        i.a = st.a;  i.x = st.x;  i.y = st.y;  i.s = st.s;  i.p = st.p;
        i.db = st.data_bus;  i.pl = st.ppu_latch;  i.cycles = st.cycles;
        i.frame = h.frame_no;
        i.scanline = nrt_current_scanline(m);
        const int r = h.rec.insn(i);
        if (r & 1) h.emit("pc", i.pc, h.rec.pc_hit(i.pc), 0);
        if (r & 2) h.emit("insn", -1, 0, h.rec.insn_number());
    }

    static void on_scanline_end(void* ctx, nrt_machine*, int sl)
    {
        Harness& h = *static_cast<Harness*>(ctx);
        if (h.rec.want_scanline(h.frame_no, sl)) h.emit("scanline", -1, 0, 0);
    }
};

bool load_rom(const char* path, std::vector<uint8_t>& prg, std::vector<uint8_t>& chr, bool& vertical, std::string& err)
{
    FILE* f = fopen(path, "rb");
    if (!f) { err = std::string("cannot open ") + path; return false; }
    std::vector<uint8_t> data;
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) != 0) data.insert(data.end(), buf, buf + n);
    fclose(f);
    if (data.size() < 16 || memcmp(data.data(), "NES\x1a", 4) != 0) { err = "not an iNES ROM"; return false; }
    if (data[4] < 1 || data[4] > 2 || data[5] != 1) { err = "only 1-2 PRG banks and 1 CHR bank are supported"; return false; }
    if ((data[6] & 0xF0) != 0 || (data[7] & 0xF0) != 0 || (data[6] & 4)) { err = "only mapper 0 without a trainer is supported"; return false; }
    const size_t prg_size = (size_t)data[4] * 16384;
    if (data.size() != 16 + prg_size + 8192) { err = "file size does not match the iNES header"; return false; }
    prg.assign(data.begin() + 16, data.begin() + 16 + (long)prg_size);
    chr.assign(data.begin() + 16 + (long)prg_size, data.end());
    vertical = (data[6] & 1) != 0;
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    nesdump::Config cfg;
    std::string err;
    if (!cfg.parse(argc, argv, err)) {
        fprintf(stderr, "error: %s\n%s\n", err.c_str(), nesdump::Config::usage());
        return 2;
    }
    if (!cfg.movie_file.empty()) {
        fprintf(stderr, "--movie is only supported by the FCEUX verification runner; use --input\n");
        return 2;
    }
    std::vector<uint8_t> prg, chr;
    bool vertical = false;
    if (!load_rom(cfg.rom.c_str(), prg, chr, vertical, err)) {
        fprintf(stderr, "error: %s\n", err.c_str());
        return 2;
    }

    Harness h;
    h.chr = chr;
    h.prg32.resize(0x8000);
    for (size_t i = 0; i < h.prg32.size(); ++i) h.prg32[i] = prg[i % prg.size()];
    h.frame.assign(NRT_FRAME_BYTES, 0);

    nrt_cart cart = { prg.data(), (uint32_t)prg.size(), chr.data(), (uint32_t)chr.size(), vertical ? uint8_t(1) : uint8_t(0) };
#ifdef NRT_EMBEDDED_GAME
    // The ROM named on the command line must be one this executable was generated from; run from the linked image.
    const nrt_game_entry* game = nullptr;
    {
        const uint64_t hash = nrt_rom_hash(&cart);
        for (unsigned i = 0; i < nrt_game_registry_count; ++i)
            if (nrt_game_registry[i].module->rom_hash == hash) game = &nrt_game_registry[i];
    }
    if (!game) {
        fprintf(stderr, "error: %s is not a ROM this executable was generated from\n", cfg.rom.c_str());
        return 2;
    }
    cart = *game->cart;
#endif
    std::vector<uint8_t> storage(nrt_machine_size() + nrt_machine_align());
    void* aligned = storage.data();
    size_t space = storage.size();
    uintptr_t p = ((uintptr_t)aligned + nrt_machine_align() - 1) & ~(uintptr_t)(nrt_machine_align() - 1);
    aligned = (void*)p;
    space -= (size_t)(p - (uintptr_t)storage.data());
    h.machine = nrt_machine_init(aligned, space, &cart, h.frame.data());
    if (!h.machine) { fprintf(stderr, "error: the runtime rejected this cart\n"); return 2; }

    nrt_ports ports = {};
    ports.port_enabled[0] = cfg.input.ports[0] == 1;
    ports.port_enabled[1] = cfg.input.ports[1] == 1;
    ports.four_score = cfg.input.fourscore;
    ports.famicom_4player = cfg.input.ports[2] == 3;
    nrt_set_ports(h.machine, &ports);
#ifdef NRT_EMBEDDED_GAME
    if (!getenv("NRT_NO_CODE")) {   // NRT_NO_CODE=1: the same executable, interpreter only
        const int rc = nrt_attach_code(h.machine, game->module);
        if (rc) { fprintf(stderr, "error: generated code rejected (%d)\n", rc); return 2; }
        h.track_coverage = true;
    }
#endif
    nrt_hooks hooks = { &h, Harness::on_instruction, Harness::on_scanline_end };
    nrt_set_hooks(h.machine, &hooks);

    if (!h.rec.open(cfg, "nrt")) return 2;
    {
        nesdump::State s;
        memset(&s, 0, sizeof(s));
        h.fill(s);
        if (!h.rec.write_rom(s)) return 2;
    }
    if (cfg.init_pc >= 0) nrt_debug_set_pc(h.machine, (uint16_t)cfg.init_pc);

    for (int f = 1; f <= cfg.frames; ++f) {
        const uint8_t command = cfg.command_at(f);
        if (command & 2) nrt_power_cycle(h.machine);
        if (command & 1) nrt_soft_reset(h.machine);
        const uint32_t pads = cfg.input_at(f);
        const uint8_t bytes[4] = { (uint8_t)pads, (uint8_t)(pads >> 8), (uint8_t)(pads >> 16), (uint8_t)(pads >> 24) };
        nrt_set_pads(h.machine, bytes);
        h.frame_no = f;
        h.rec.begin_frame(f);
        nrt_run_frame(h.machine);
        if (h.rec.want_frame(f)) h.emit("frame", -1, 0, 0);
        h.rec.end_frame();
    }
    h.rec.close(cfg.frames);
    nrt_exec_stats stats;
    nrt_get_exec_stats(h.machine, &stats);
    {
        const std::string path = cfg.outdir + "/coverage.txt";
        if (FILE* f = fopen(path.c_str(), "w")) {
            fprintf(f, "generated %llu\ninterpreted %llu\n", (unsigned long long)stats.generated, (unsigned long long)stats.interpreted);
            for (uint16_t pc : h.interpreted_pcs) fprintf(f, "missed %04X\n", pc);
            fclose(f);
        }
    }
    printf("nrt dump done: %d frames, output in %s (generated %llu, interpreted %llu instructions)\n", cfg.frames,
           cfg.outdir.c_str(), (unsigned long long)stats.generated, (unsigned long long)stats.interpreted);
    return 0;
}
