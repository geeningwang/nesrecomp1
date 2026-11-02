#include "nesrt_ops.h"

uint8_t nrt_test_adc(nrt_cpu* c, uint8_t lhs, uint8_t rhs)
{
    A = lhs;
    ADC(rhs, FALL);
    return PACK_P(0);
}

uint8_t nrt_test_sbc(nrt_cpu* c, uint8_t lhs, uint8_t rhs)
{
    A = lhs;
    SBC(rhs, FALL);
    return PACK_P(0);
}

uint16_t nrt_test_page(nrt_cpu* c)
{
    STEP(0xC000, 2);
    LDA(0x7F, FALL);
    STEP(0xC002, 2);
    ADC(1, FALL);
    PUSH(A);
    WR(0x6000, POP());
    return RD(0x6000);
}
