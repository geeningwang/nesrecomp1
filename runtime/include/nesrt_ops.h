/* nesrt_ops.h - 6502 instruction semantics for the generated code (exact mode).
 *
 * Included by every generated page function.  Pure C99, no libc.  Written clean-room from the 6502 documentation
 * and from the instruction behaviour observed in FCEUX by the nes1 parity work (doc/parity_spec.md: O-2, O-14,
 * O-17, O-31, O-33).  The register file lives in `nrt_cpu`; generated code names it `c`.
 *
 * The flag mask argument F of every operation is a compile-time constant chosen by the code generator's liveness
 * analysis: flags not in F are not computed (their code disappears at compile time).  The trace build passes 0xFF.
 */
#ifndef NESRT_OPS_H
#define NESRT_OPS_H

#include <stdint.h>

#define FN 0x80
#define FV 0x40
#define FD 0x08
#define FI 0x04
#define FZ 0x02
#define FC 0x01
#define FALL 0xCF

typedef struct nrt_cpu {
    uint16_t pc;                 /* where to continue when a page function returns */
    uint8_t  a, x, y, s;
    uint8_t  fn, fv, fd, fi, fz, fc;   /* each 0 or 1 */
    uint8_t  db;                 /* data bus latch (spec O-14) */
    uint8_t  jammed;
    int32_t  timer;              /* CPU cycles left before the runtime has to run; <= 0: return to the runtime */
    uint8_t *ram;                /* 2 KB work RAM */
    uint8_t  miss;               /* set by a page function that was entered at a non-entry address */
    uint8_t  tadj;               /* base cycles of the current block that come after the instruction doing bus I/O */
} nrt_cpu;

/* runtime services (runtime/src/bus.c) */
#ifdef __cplusplus
extern "C" {
#endif
uint8_t nrt_bus_read(nrt_cpu *c, uint16_t addr);
void    nrt_bus_write(nrt_cpu *c, uint16_t addr, uint8_t v);
uint16_t nrt_brk(nrt_cpu *c, uint16_t return_pc);   /* BRK: push, set I, returns the IRQ/BRK vector */
#ifdef __cplusplus
}
#endif

static inline uint8_t nrt_rd(nrt_cpu *c, uint16_t a)
{
    if (a < 0x2000) return c->db = c->ram[a & 0x7FF];
    return nrt_bus_read(c, a);
}
static inline void nrt_wr(nrt_cpu *c, uint16_t a, uint8_t v)
{
    if (a < 0x2000) { c->db = v; c->ram[a & 0x7FF] = v; return; }
    nrt_bus_write(c, a, v);
}
/* absolute,X / absolute,Y / (zp),Y loads: a page cross costs a cycle and a read of the uncorrected address */
static inline uint8_t nrt_rd_idx(nrt_cpu *c, uint16_t base, uint8_t idx)
{
    uint16_t t = (uint16_t)(base + idx);
    if ((t ^ base) & 0x100) { nrt_rd(c, (uint16_t)(t ^ 0x100)); c->timer -= 1; }
    return nrt_rd(c, t);
}
/* stores and read-modify-write: always read the uncorrected address first (spec O-33b); returns the effective address */
static inline uint16_t nrt_ea_idx_w(nrt_cpu *c, uint16_t base, uint8_t idx)
{
    uint16_t t = (uint16_t)(base + idx);
    nrt_rd(c, (uint16_t)((t & 0xFF) | (base & 0xFF00)));
    return t;
}
static inline uint16_t nrt_ptr_zp(nrt_cpu *c, uint8_t z)       /* 16-bit pointer in zero page, wraps inside the page */
{
    uint16_t lo = c->db = c->ram[z];
    uint16_t hi = c->db = c->ram[(uint8_t)(z + 1)];
    return (uint16_t)(lo | (hi << 8));
}

#define RDZP(z)    (c->db = c->ram[(uint8_t)(z)])
#define WRZP(z, v) do { uint8_t _v = (v); c->ram[(uint8_t)(z)] = _v; c->db = _v; } while (0)
#define RD(a)      nrt_rd(c, (uint16_t)(a))
#define WR(a, v)   nrt_wr(c, (uint16_t)(a), (v))
#define CYC(n)     (c->timer -= (n))

/* the per-instruction hook: nothing happens until the runtime's deadline is reached; then the page function
 * returns the address of the instruction that has not run yet. */
#define STEP(PC, CY) do { if (c->timer <= 0) { c->pc = (PC); return (PC); } c->timer -= (CY); } while (0)

/* block-level hook: the deadline is checked once per basic block against the block's worst-case cost (base plus
 * every possible page-cross / taken-branch extra).  When it does not fit, the page function returns *before* the
 * block and the runtime single-steps the block with the interpreter, so every event lands on the exact same
 * instruction boundary as with a per-instruction check.  Bulk-charging the base cycles is safe because nothing
 * inside the block can observe the time except bus I/O, which gets `tadj` (see nrt_bus_*). */
#define BSTEP(PC, MAXCY, BASECY) do { if (c->timer <= (MAXCY)) { c->pc = (PC); return (PC); } c->timer -= (BASECY); } while (0)

#define A c->a
#define X c->x
#define Y c->y

#define NZ(v, F) do { if ((F) & (FN | FZ)) { uint8_t _t = (v); if ((F) & FN) c->fn = _t >> 7; if ((F) & FZ) c->fz = (_t == 0); } } while (0)
#define SETC(v, F) do { if ((F) & FC) c->fc = (v) ? 1 : 0; } while (0)

#define PUSH(v)  do { uint8_t _p = (v); c->ram[0x100 + c->s] = _p; c->db = _p; c->s--; } while (0)
#define POP()    (c->db = c->ram[0x100 + (uint8_t)(++c->s)])
#define PACK_P(B) ((uint8_t)(0x20 | (B) | (c->fn << 7) | (c->fv << 6) | (c->fd << 3) | (c->fi << 2) | (c->fz << 1) | c->fc))
#define UNPACK_P(p) do { uint8_t _q = (p); c->fn = _q >> 7; c->fv = (_q >> 6) & 1; c->fd = (_q >> 3) & 1; c->fi = (_q >> 2) & 1; c->fz = (_q >> 1) & 1; c->fc = _q & 1; } while (0)

/* ---- loads / transfers ---- */
#define LDA(m, F) do { A = (m); NZ(A, F); } while (0)
#define LDX(m, F) do { X = (m); NZ(X, F); } while (0)
#define LDY(m, F) do { Y = (m); NZ(Y, F); } while (0)
#define LAX(m, F) do { A = X = (m); NZ(A, F); } while (0)
#define TAX(F) do { X = A; NZ(X, F); } while (0)
#define TAY(F) do { Y = A; NZ(Y, F); } while (0)
#define TXA(F) do { A = X; NZ(A, F); } while (0)
#define TYA(F) do { A = Y; NZ(A, F); } while (0)
#define TSX(F) do { X = c->s; NZ(X, F); } while (0)
#define TXS()  do { c->s = X; } while (0)

/* ---- arithmetic / logic ---- */
#define ADC(m, F) do { uint8_t _m = (m); unsigned _s = (unsigned)A + _m + c->fc; \
    if ((F) & FV) c->fv = ((~(A ^ _m) & (A ^ _s) & 0x80) != 0); \
    if ((F) & FC) c->fc = _s > 0xFF; A = (uint8_t)_s; NZ(A, F); } while (0)
#define SBC(m, F) do { uint8_t _m = (uint8_t)~(m); unsigned _s = (unsigned)A + _m + c->fc; \
    if ((F) & FV) c->fv = ((~(A ^ _m) & (A ^ _s) & 0x80) != 0); \
    if ((F) & FC) c->fc = _s > 0xFF; A = (uint8_t)_s; NZ(A, F); } while (0)
#define AND(m, F) do { A &= (m); NZ(A, F); } while (0)
#define ORA(m, F) do { A |= (m); NZ(A, F); } while (0)
#define EOR(m, F) do { A ^= (m); NZ(A, F); } while (0)
#define CMPR(r, m, F) do { uint8_t _m = (m); unsigned _d = (unsigned)(r) - _m; SETC((r) >= _m, F); NZ((uint8_t)_d, F); } while (0)
#define CMP(m, F) CMPR(A, m, F)
#define CPX(m, F) CMPR(X, m, F)
#define CPY(m, F) CMPR(Y, m, F)
#define BIT(m, F) do { uint8_t _m = (m); if ((F) & FN) c->fn = _m >> 7; if ((F) & FV) c->fv = (_m >> 6) & 1; \
    if ((F) & FZ) c->fz = ((A & _m) == 0); } while (0)

/* ---- shifts / rotates / inc / dec on a variable m (accumulator or memory byte) ---- */
#define ASL(m, F) do { SETC((m) & 0x80, F); (m) = (uint8_t)((m) << 1); NZ(m, F); } while (0)
#define LSR(m, F) do { SETC((m) & 1, F); (m) = (uint8_t)((m) >> 1); NZ(m, F); } while (0)
#define ROL(m, F) do { uint8_t _o = c->fc; SETC((m) & 0x80, F); (m) = (uint8_t)(((m) << 1) | _o); NZ(m, F); } while (0)
#define ROR(m, F) do { uint8_t _o = c->fc; SETC((m) & 1, F); (m) = (uint8_t)(((m) >> 1) | (_o << 7)); NZ(m, F); } while (0)
#define INC(m, F) do { (m) = (uint8_t)((m) + 1); NZ(m, F); } while (0)
#define DEC(m, F) do { (m) = (uint8_t)((m) - 1); NZ(m, F); } while (0)
#define INX(F) do { X++; NZ(X, F); } while (0)
#define INY(F) do { Y++; NZ(Y, F); } while (0)
#define DEX(F) do { X--; NZ(X, F); } while (0)
#define DEY(F) do { Y--; NZ(Y, F); } while (0)

/* ---- flags ---- */
#define CLC() (c->fc = 0)
#define SEC() (c->fc = 1)
#define CLI() (c->fi = 0)
#define SEI() (c->fi = 1)
#define CLD() (c->fd = 0)
#define SED() (c->fd = 1)
#define CLV() (c->fv = 0)

/* ---- stack ---- */
#define PHA() PUSH(A)
#define PHP() PUSH(PACK_P(0x10))
#define PLA(F) do { A = POP(); NZ(A, F); } while (0)
#define PLP() do { UNPACK_P(POP()); } while (0)

/* ---- undocumented read-modify-write combos, on a memory byte variable m ---- */
#define SLO(m, F) do { ASL(m, FC | FN | FZ); A |= (m); NZ(A, F); } while (0)
#define RLA(m, F) do { ROL(m, FC | FN | FZ); A &= (m); NZ(A, F); } while (0)
#define SRE(m, F) do { LSR(m, FC | FN | FZ); A ^= (m); NZ(A, F); } while (0)
#define RRA(m, F) do { ROR(m, FC | FN | FZ); ADC(m, F); } while (0)
#define DCP(m, F) do { (m) = (uint8_t)((m) - 1); CMP(m, F); } while (0)
#define ISB(m, F) do { (m) = (uint8_t)((m) + 1); SBC(m, F); } while (0)
/* ---- undocumented immediates ---- */
#define ANC(m, F) do { A &= (m); NZ(A, F); SETC(A & 0x80, F); } while (0)
#define ALR(m, F) do { A &= (m); LSR(A, F); } while (0)
#define ARR(m, F) do { uint8_t _o = c->fc; A &= (m); A = (uint8_t)((A >> 1) | (_o << 7)); NZ(A, F); \
    if ((F) & FC) c->fc = (A >> 6) & 1; if ((F) & FV) c->fv = ((A >> 6) ^ (A >> 5)) & 1; } while (0)
#define AXS(m, F) do { uint8_t _m = (m); uint8_t _a = A & X; SETC(_a >= _m, F); X = (uint8_t)(_a - _m); NZ(X, F); } while (0)
#define XAA(m, F) do { A = (uint8_t)((A | 0xEE) & X & (m)); NZ(A, F); }  while (0)    /* spec O-31 */
#define LXA(m, F) do { A = X = (m); NZ(A, F); } while (0)                              /* spec O-31 */
#define LAS(m, F) do { c->s &= (m); A = X = c->s; NZ(A, F); } while (0)                /* RMW in FCEUX, spec O-31 */

/* ---- unstable stores (spec O-31) ---- */
/* AHX ($93,$9F): A & X & (H+1), H = high byte of the base address, written to base+idx.
 * TAS ($9B): S = A & X, then S & (H+1) is stored.
 * SHY ($9C) / SHX ($9E): reg & (H'+1) with H' = high byte of base+idx, always written to (value << 8 | low(base+idx)). */
static inline void nrt_unstable_store(nrt_cpu *c, int kind, uint16_t base, uint8_t idx)
{
    uint16_t ea = (uint16_t)(base + idx);
    uint8_t v;
    nrt_rd(c, (uint16_t)((ea & 0xFF) | (base & 0xFF00)));
    switch (kind) {
    case 0: v = (uint8_t)(c->a & c->x & ((base >> 8) + 1)); break;                                  /* AHX */
    case 1: c->s = (uint8_t)(c->a & c->x); v = (uint8_t)(c->s & ((base >> 8) + 1)); break;      /* TAS */
    case 2: v = (uint8_t)(c->y & ((ea >> 8) + 1)); ea = (uint16_t)((ea & 0xFF) | (v << 8)); break;  /* SHY */
    default: v = (uint8_t)(c->x & ((ea >> 8) + 1)); ea = (uint16_t)((ea & 0xFF) | (v << 8)); break;  /* SHX */
    }
    nrt_wr(c, ea, v);
}

/* ---- control flow helpers used by the generator ---- */
#define BRANCH_COND_N  (c->fn)
#define BRANCH_COND_V  (c->fv)
#define BRANCH_COND_Z  (c->fz)
#define BRANCH_COND_C  (c->fc)

#endif /* NESRT_OPS_H */
