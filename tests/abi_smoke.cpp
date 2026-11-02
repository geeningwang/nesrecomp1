#include <cstdio>
#include "nesrt_ops.h"

extern "C" {
uint8_t nrt_test_adc(nrt_cpu*, uint8_t, uint8_t);
uint8_t nrt_test_sbc(nrt_cpu*, uint8_t, uint8_t);
uint16_t nrt_test_page(nrt_cpu*);
}

static uint8_t bus_byte;

extern "C" uint8_t nrt_bus_read(nrt_cpu* c, uint16_t addr)
{
    if (addr != 0x6000) return c->db = 0;
    return c->db = bus_byte;
}

extern "C" void nrt_bus_write(nrt_cpu* c, uint16_t addr, uint8_t value)
{
    if (addr == 0x6000) bus_byte = value;
    c->db = value;
}

extern "C" uint16_t nrt_brk(nrt_cpu*, uint16_t return_pc)
{
    return return_pc;
}

int main()
{
    uint8_t ram[2048] = {};
    nrt_cpu state = {};
    state.ram = ram;
    state.s = 0xFD;
    state.timer = 20;
    if (nrt_test_page(&state) != 0x80 || state.a != 0x80 || state.s != 0xFD ||
        state.timer != 16 || state.fn != 1 || state.fv != 1 || state.fc != 0 ||
        state.db != 0x80) {
        std::fputs("C page/C++ bus ABI smoke failed\n", stderr);
        return 1;
    }
    state.fc = 0;
    if ((nrt_test_adc(&state, 0xFF, 1) & (FC | FZ | FN | FV)) != (FC | FZ)) {
        std::fputs("ADC flag smoke failed\n", stderr);
        return 1;
    }
    state.fc = 1;
    if ((nrt_test_sbc(&state, 0, 1) & (FC | FZ | FN | FV)) != FN || state.a != 0xFF) {
        std::fputs("SBC flag smoke failed\n", stderr);
        return 1;
    }
    std::puts("C/C++ ABI, memory/stack and arithmetic smoke passed");
    return 0;
}
