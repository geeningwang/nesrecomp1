/* nrt_exec.h - what generated game code sees of the runtime (pure C, no libc beyond <stdint.h>).
 *
 * Generated code is "exact mode": one C function per instruction of the game, specialised on the opcode, its
 * operands, addressing mode and cycle counts, executing exactly the steps of the interpreter's step() (parity spec
 * O-1 .. O-43). The runtime finds the function for the current PC; a PC without a function runs in the
 * interpreter, so a gap in the code map costs speed, never correctness.
 *
 * nrt_cpu_regs is the canonical CPU state shared by the interpreter and generated code. All bus traffic, the APU
 * hook before the instruction and the interrupt/trace sequence stay in the runtime.
 */
#ifndef NRT_EXEC_H
#define NRT_EXEC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nrt_cpu_regs {
    uint16_t rpc;                         /* address of the instruction about to execute */
    uint8_t  rsp, ra, rx, ry;
    uint8_t  rp;                          /* architectural P: bit 5 set, bit 4 never stored (spec O-2) */
    uint8_t  data_bus;                    /* last value on the CPU data bus (spec O-14) */
    uint8_t  pending_instruction_cycles;  /* cycles of the running instruction the PPU already counts (spec O-33) */
    uint8_t  rpi;                         /* P at the start of the previous instruction: the I flag IRQ checks see */
    uint8_t  jammed;                      /* executed a KIL opcode */
    uint8_t  nmi_state;                   /* 0 none, 1 armed by a $2000 write, 2 due at the next boundary (O-38) */
    uint64_t cycle;                       /* CPU cycles since power-on */
    uint64_t apu_cycle;                   /* CPU cycle up to which the APU has been advanced */
    uint8_t *ram_ptr;                     /* the 2 KB of CPU RAM, for direct zero-page and stack access */
} nrt_cpu_regs;

/* A generated instruction: runs the instruction at c->rpc completely. Returns nonzero on success. */
typedef int (*nrt_insn_fn)(nrt_cpu_regs *c);

#define NRT_CODE_ABI 2

typedef struct nrt_code_module {
    uint32_t           abi;          /* NRT_CODE_ABI */
    uint32_t           prg_size;     /* 16384 or 32768 */
    uint64_t           rom_hash;     /* nrt_rom_hash() of the cart the code was generated from */
    const uint16_t    *slot;         /* prg_size entries indexed by PRG offset: 0 = interpret, n = fns[n-1] */
    const nrt_insn_fn *fns;
    uint32_t           functions;    /* number of entries in fns (at most 65535) */
    const char        *generator;    /* generator name and version, for reports */
} nrt_code_module;

/* A game linked into an executable: the cart image and its generated code. tools/gen_insn.py writes one per game
 * and tools/gen_registry.py lists them (generated/registry.c). */
struct nrt_cart;
typedef struct nrt_game_entry {
    const char              *name;
    const struct nrt_cart   *cart;
    const nrt_code_module   *module;
} nrt_game_entry;

/* Services for generated code. */
uint8_t nrt_x_begin(nrt_cpu_regs *c, uint8_t opcode, uint8_t apu_cycles);   /* P snapshot + APU hook; returns the opcode-fetch bus value */
uint8_t nrt_x_read(nrt_cpu_regs *c, uint16_t addr);                         /* CPU bus read with side effects */
void    nrt_x_write(nrt_cpu_regs *c, uint16_t addr, uint8_t value);         /* CPU bus write */
uint8_t nrt_x_peek(nrt_cpu_regs *c, uint16_t addr);                         /* side-effect free (RAM, work RAM, PRG) */

#ifdef __cplusplus
}
#endif

#endif
