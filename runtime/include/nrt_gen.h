/* nrt_gen.h - names and helpers that generated instruction functions are written in (see tools/gen_insn.py).
 *
 * Generated functions take the canonical CPU state as `c` and use the interpreter's own variable names for the
 * registers and bus helpers; this header maps those names onto nrt_cpu_regs and the runtime services. Inside a
 * generated file never write `c->rpc` etc. directly: the names below are macros.
 */
#ifndef NRT_GEN_H
#define NRT_GEN_H

#include <stdint.h>
#include "nrt_exec.h"

#define rpc  (c->rpc)
#define rsp  (c->rsp)
#define ra   (c->ra)
#define rx   (c->rx)
#define ry   (c->ry)
#define rp   (c->rp)
#define rpi  (c->rpi)
#define jammed     (c->jammed)
#define data_bus   (c->data_bus)
#define cycle      (c->cycle)
#define pending_instruction_cycles (c->pending_instruction_cycles)
#define ram  (c->ram_ptr)

#define mem_read(a)       nrt_x_read(c, (uint16_t)(a))
#define mem_write(a, v)   nrt_x_write(c, (uint16_t)(a), (uint8_t)(v))
#define rd8(a)            nrt_x_peek(c, (uint16_t)(a))
#define rd16(a)           ((uint16_t)(nrt_x_peek(c, (uint16_t)(a)) | (nrt_x_peek(c, (uint16_t)((a) + 1)) << 8)))

#endif
