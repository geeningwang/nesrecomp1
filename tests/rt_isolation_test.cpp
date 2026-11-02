// rt_isolation_test - API contract of the runtime core: machines own all of their state, so repeated runs and
// interleaved machines must agree with a clean solo run, and bad carts or storage are rejected.
//   rt_isolation_test ROM.nes
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "nrt.h"

namespace {

struct Rom {
    std::vector<uint8_t> prg, chr;
    bool vertical = false;
};

bool load(const char* path, Rom& rom)
{
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    std::vector<uint8_t> d;
    uint8_t b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) != 0) d.insert(d.end(), b, b + n);
    fclose(f);
    if (d.size() < 16 || memcmp(d.data(), "NES\x1a", 4) != 0 || d[5] != 1) return false;
    const size_t prg = (size_t)d[4] * 16384;
    if (d.size() != 16 + prg + 8192) return false;
    rom.prg.assign(d.begin() + 16, d.begin() + 16 + (long)prg);
    rom.chr.assign(d.begin() + 16 + (long)prg, d.end());
    rom.vertical = (d[6] & 1) != 0;
    return true;
}

uint64_t mix(uint64_t h, const void* data, size_t n)
{
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

struct Instance {
    std::vector<uint8_t> storage = std::vector<uint8_t>(nrt_machine_size() + nrt_machine_align());
    std::vector<uint8_t> frame = std::vector<uint8_t>(NRT_FRAME_BYTES);
    nrt_machine* m = nullptr;
    uint64_t hash = 14695981039346656037ULL;
    int instructions = 0;

    static void count(void* ctx, nrt_machine*) { ++static_cast<Instance*>(ctx)->instructions; }

    bool init(const Rom& rom)
    {
        uintptr_t a = ((uintptr_t)storage.data() + nrt_machine_align() - 1) & ~(uintptr_t)(nrt_machine_align() - 1);
        nrt_cart cart = { rom.prg.data(), (uint32_t)rom.prg.size(), rom.chr.data(), (uint32_t)rom.chr.size(), rom.vertical };
        m = nrt_machine_init((void*)a, storage.size() - (size_t)(a - (uintptr_t)storage.data()), &cart, frame.data());
        if (!m) return false;
        nrt_hooks hooks = { this, count, nullptr };
        nrt_set_hooks(m, &hooks);
        return true;
    }

    void step(uint8_t pad)
    {
        const uint8_t pads[4] = { pad, (uint8_t)~pad, 0, 0 };
        nrt_set_pads(m, pads);
        nrt_run_frame(m);
        nrt_state s;
        nrt_get_state(m, &s);
        hash = mix(hash, &s, sizeof(s));
        hash = mix(hash, nrt_ram(m), 0x800);
        hash = mix(hash, nrt_nametable_ram(m), 0x800);
        hash = mix(hash, nrt_palette_ram(m), 0x20);
        hash = mix(hash, nrt_oam(m), 0x100);
        hash = mix(hash, frame.data(), frame.size());
    }
};

uint8_t pad_for(int pattern, int frame) { return (uint8_t)((frame * (pattern + 3) + pattern * 17) >> 2); }

int fail(const char* what)
{
    fprintf(stderr, "FAILED: %s\n", what);
    return 1;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2) { fprintf(stderr, "usage: rt_isolation_test ROM.nes\n"); return 2; }
    Rom rom;
    if (!load(argv[1], rom)) { fprintf(stderr, "cannot load %s\n", argv[1]); return 2; }
    const int kFrames = 90;

    uint64_t solo[2];
    for (int pattern = 0; pattern < 2; ++pattern) {
        Instance a;
        if (!a.init(rom)) return fail("init");
        for (int f = 0; f < kFrames; ++f) a.step(pad_for(pattern, f));
        solo[pattern] = a.hash;
        if (a.instructions == 0) return fail("no instructions observed");
    }
    if (solo[0] == solo[1]) return fail("different input must change the result");

    {   // a second run from fresh storage repeats exactly
        Instance a;
        a.init(rom);
        for (int f = 0; f < kFrames; ++f) a.step(pad_for(0, f));
        if (a.hash != solo[0]) return fail("repeated run differs");
    }
    {   // two machines interleaved frame by frame, in the opposite order every other frame
        Instance a, b;
        a.init(rom);
        b.init(rom);
        for (int f = 0; f < kFrames; ++f) {
            if (f & 1) { b.step(pad_for(1, f)); a.step(pad_for(0, f)); }
            else       { a.step(pad_for(0, f)); b.step(pad_for(1, f)); }
        }
        if (a.hash != solo[0] || b.hash != solo[1]) return fail("interleaved machines interfere");
    }
    {   // re-initialising used storage gives a clean machine
        Instance a;
        a.init(rom);
        for (int f = 0; f < 20; ++f) a.step(pad_for(1, f));
        a.hash = 14695981039346656037ULL;
        if (!a.init(rom)) return fail("re-init");
        for (int f = 0; f < kFrames; ++f) a.step(pad_for(0, f));
        if (a.hash != solo[0]) return fail("re-initialised machine differs");
    }
    {   // a power cycle returns to the power-on state while the cart stays loaded
        Instance a, b;
        a.init(rom);
        b.init(rom);
        for (int f = 0; f < 30; ++f) a.step(pad_for(1, f));
        nrt_power_cycle(a.m);
        a.hash = b.hash = 14695981039346656037ULL;
        for (int f = 0; f < 30; ++f) { a.step(pad_for(0, f)); b.step(pad_for(0, f)); }
        if (a.hash != b.hash) return fail("power cycle differs from power-on");
    }

    // Rejections: no storage, misaligned or short storage, bad carts.
    std::vector<uint8_t> big(nrt_machine_size() + 64);
    uintptr_t aligned = ((uintptr_t)big.data() + nrt_machine_align() - 1) & ~(uintptr_t)(nrt_machine_align() - 1);
    nrt_cart cart = { rom.prg.data(), (uint32_t)rom.prg.size(), rom.chr.data(), (uint32_t)rom.chr.size(), 0 };
    if (nrt_machine_init(nullptr, big.size(), &cart, nullptr)) return fail("null storage accepted");
    if (nrt_machine_init((void*)aligned, nrt_machine_size() - 1, &cart, nullptr)) return fail("short storage accepted");
    if (nrt_machine_align() > 1 && nrt_machine_init((void*)(aligned + 1), nrt_machine_size(), &cart, nullptr)) return fail("misaligned storage accepted");
    nrt_cart bad = cart;
    bad.prg_size = 8192;
    if (nrt_machine_init((void*)aligned, nrt_machine_size(), &bad, nullptr)) return fail("bad PRG size accepted");
    bad = cart;
    bad.chr_size = 4096;
    if (nrt_machine_init((void*)aligned, nrt_machine_size(), &bad, nullptr)) return fail("bad CHR size accepted");
    bad = cart;
    bad.chr = nullptr;
    if (nrt_machine_init((void*)aligned, nrt_machine_size(), &bad, nullptr)) return fail("null CHR accepted");
    // No frame buffer is allowed: the machine runs without producing pixels.
    nrt_machine* headless = nrt_machine_init((void*)aligned, nrt_machine_size(), &cart, nullptr);
    if (!headless) return fail("headless init");
    nrt_run_frame(headless);
    nrt_run_frame(headless);
    nrt_run_frame(headless);

    printf("runtime isolation contract ok (%d frames, solo/repeat/interleaved/re-init/power-cycle)\n", kFrames);
    return 0;
}
