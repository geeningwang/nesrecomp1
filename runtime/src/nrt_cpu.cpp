// nrt_cpu.cpp - the 6502 interpreter of the runtime core.
//
// The opcode table and the instruction bodies are taken unchanged from nes1's cpu.cpp (MIT), whose behaviour was
// verified against FCEUX 2.6.6 (parity spec O-1 .. O-43); only the memory access macros differ: the CPU address
// space is the 2 KB RAM, the 8 KB work RAM and a read-only PRG view instead of a flat 64 KB array.
#include "nrt_internal.hpp"

namespace nrt {

#define OPERAND8 \
	(rd8((uint16_t)(rpc + 1)))

#define OPERAND16 \
	(rd16((uint16_t)(rpc + 1)))

#define VAR_ZERO_PAGE(operand) \
	(ram[(operand)])

#define OPERAND_ADD_X(operand) \
	((operand + rx) & 0xff)

#define OPERAND_ADD_Y(operand) \
	((operand + ry) & 0xff)

#define VAR_ZERO_PAGE_X(operand) \
	(ram[OPERAND_ADD_X(operand)])

#define VAR_ZERO_PAGE_Y(operand) \
	(ram[OPERAND_ADD_Y(operand)])

#define ADDRESS_ADD_X(address) \
	((address + rx) & 0xffff)

#define ADDRESS_ADD_Y(address) \
	((address + ry) & 0xffff)

#define ADDRESS_ZERO_PAGE_WRAP(address) \
	((address != 0xff) ? (ram[(address)] | (ram[(address) + 1] << 8)) : (ram[0xff] | (ram[0] << 8)))

#define ADDRESS_INDIRECT_PAGE_WRAP(operand16) \
	(((operand16 & 0xff) != 0xff) ? rd16((uint16_t)(operand16)) : (rd8((uint16_t)(operand16)) | (rd8((uint16_t)((operand16) & 0xff00)) << 8)))

#define SET_CARRY(r) \
	if ((r) != 0) { rp |= 0x01; } else { rp &= 0xfe; }

#define GET_CARRY() \
	(rp & 0x01)

#define SET_ZERO(r) \
	if ((r) == 0) { rp |= 0x02; } else { rp &= 0xfd; }

#define GET_ZERO() \
	((rp >> 1) & 0x01)

#define SET_INTERRUPT(r) \
	if ((r) != 0) { rp |= 0x04; } else { rp &= 0xfb; }

#define SET_DECIMAL(r) \
	if ((r) != 0) { rp |= 0x08; } else { rp &= 0xf7; }

#define SET_OVERFLOW(r) \
	if ((r) != 0) { rp |= 0x40; } else { rp &= 0xbf; }

#define SET_OVERFLOW_BY_BIT(r) \
	if (((r) & 0x40) != 0) { rp |= 0x40; } else { rp &= 0xbf; }

#define GET_OVERFLOW() \
	((rp >> 6) & 0x01)

#define SET_NEGATIVE_BY_BIT(r) \
	if (((r) & 0x80) != 0) { rp |= 0x80; } else { rp &= 0x7f; }

#define GET_NEGATIVE() \
	(rp >> 7)

#define STACK_PUSH(r) \
	ram[0x100 + rsp] = (uint8_t)(r); \
	data_bus = ram[0x100 + rsp]; \
	rsp--;

#define STACK_POP(r) \
	rsp ++; \
	(r) = ram[0x100 + rsp]; \
	data_bus = ram[0x100 + rsp];

#define STACK_PUSH_PC() \
	STACK_PUSH(rpc >> 8); \
	STACK_PUSH(rpc & 0xff); \

#define STACK_POP_PC() \
	{ \
	unsigned char _poptempl, _poptemph; \
	STACK_POP(_poptempl); \
	STACK_POP(_poptemph); \
	rpc = (uint16_t)(_poptemph << 8 | _poptempl); \
	}

#define BRANCH_NEW_PAGE() !((rpc & 0xff00) == (temp_rpc & 0xff00))

#define PAGE_CROSSED(address1,address2) ((address1 >> 8) != (address2 >> 8))

#define UNK 0
#define ADC 1
#define AND 2
#define ASL 3
#define BCC 4
#define BCS 5
#define BEQ 6
#define BIT 7
#define BMI 8
#define BNE 9
#define BPL 10
#define BRK 11
#define BVC 12
#define BVS 13
#define CLC 14
#define CLD 15
#define CLI 16
#define CLV 17
#define CMP 18
#define CPX 19
#define CPY 20
#define DEC 21
#define DEX 22
#define DEY 23
#define EOR 24
#define INC 25
#define INX 26
#define INY 27
#define JMP 28
#define JSR 29
#define LDA 30
#define LDX 31
#define LDY 32
#define LSR 33
#define NOP 34
#define ORA 35
#define PHA 36
#define PHP 37
#define PLA 38
#define PLP 39
#define ROL 40
#define ROR 41
#define RTI 42
#define RTS 43
#define SBC 44
#define SEC 45
#define SED 46
#define SEI 47
#define STA 48
#define STX 49
#define STY 50
#define TAX 51
#define TAY 52
#define TSX 53
#define TXA 54
#define TXS 55
#define TYA 56
#define SNOP 57
#define SLAX 58
#define SSAX 59
#define SSBC 60
#define SDCP 61
#define SISB 62
#define SSLO 63
#define SRLA 64
#define SSRE 65
#define SRRA 66
#define SKIL 67
#define SANC 68
#define SALR 69
#define SARR 70
#define SXAA 71
#define SATX 72
#define SAXS 73
#define SLAS 74
#define SAHX 75
#define STAS 76
#define SSHY 77
#define SSHX 78


#define ADDR_NONE			0
#define ADDR_IMPLICIT		1
#define ADDR_ACCUMULATOR	2
#define ADDR_IMMEDIATE		3
#define ADDR_ZERO_PAGE		4
#define ADDR_ZERO_PAGE_X	5
#define ADDR_ZERO_PAGE_Y	6
#define ADDR_RELATIVE		7
#define ADDR_ABSOLUTE		8
#define ADDR_ABSOLUTE_X		9
#define ADDR_ABSOLUTE_Y		10
#define ADDR_INDIRECT_X		11
#define ADDR_INDIRECT_Y		12
#define ADDR_JMP_ABSOLUTE	13
#define ADDR_JMP_INDIRECT	14

struct opcode
{
	int instruction;  // NULL is invalid opcode
	int instruction_size;
	int addressing_mode;
	int cycles;
	int page_crossed_extra_cycles;
};

#define BRANCH_NEW_PAGE_EXTRA_CYCLE 1
#define BRANCH_EXTRA_CYCLE 1

static const opcode opcode_items[256] = 
{
	{ BRK, 1, ADDR_IMPLICIT, 7, 0 },{ ORA, 2, ADDR_INDIRECT_X, 6, 0 },{ SKIL, 0, ADDR_IMPLICIT, 257, 0 },{ SSLO, 2, ADDR_INDIRECT_X, 8, 0 },{ SNOP, 2, ADDR_ZERO_PAGE, 3, 0 },{ ORA, 2, ADDR_ZERO_PAGE, 3, 0 },{ ASL, 2, ADDR_ZERO_PAGE, 5, 0 },{ SSLO, 2, ADDR_ZERO_PAGE, 5, 0 },
	{ PHP, 1, ADDR_IMPLICIT, 3, 0 },{ ORA, 2, ADDR_IMMEDIATE, 2, 0 },{ ASL, 1, ADDR_ACCUMULATOR, 2, 0 },{ SANC, 2, ADDR_IMMEDIATE, 2, 0 },{ SNOP, 3, ADDR_ABSOLUTE, 4, 0 },{ ORA, 3, ADDR_ABSOLUTE, 4, 0 },{ ASL, 3, ADDR_ABSOLUTE, 6, 0 },{ SSLO, 3, ADDR_ABSOLUTE, 6, 0 },
	{ BPL, 2, ADDR_RELATIVE, 2, 2 },{ ORA, 2, ADDR_INDIRECT_Y, 5, 1 },{ SKIL, 0, ADDR_IMPLICIT, 257, 0 },{ SSLO, 2, ADDR_INDIRECT_Y, 8, 0 },{ SNOP, 2, ADDR_ZERO_PAGE_X, 4, 0 },{ ORA, 2, ADDR_ZERO_PAGE_X, 4, 0 },{ ASL, 2, ADDR_ZERO_PAGE_X, 6, 0 },{ SSLO, 2, ADDR_ZERO_PAGE_X, 6, 0 },
	{ CLC, 1, ADDR_IMPLICIT, 2, 0 },{ ORA, 3, ADDR_ABSOLUTE_Y, 4, 1 },{ SNOP, 1, ADDR_IMPLICIT, 2, 0 },{ SSLO, 3, ADDR_ABSOLUTE_Y, 7, 0 },{ SNOP, 3, ADDR_ABSOLUTE_X, 4, 1 },{ ORA, 3, ADDR_ABSOLUTE_X, 4, 1 },{ ASL, 3, ADDR_ABSOLUTE_X, 7, 0 },{ SSLO, 3, ADDR_ABSOLUTE_X, 7, 0 },
	{ JSR, 3, ADDR_JMP_ABSOLUTE, 6, 0 },{ AND, 2, ADDR_INDIRECT_X, 6, 0 },{ SKIL, 0, ADDR_IMPLICIT, 257, 0 },{ SRLA, 2, ADDR_INDIRECT_X, 8, 0 },{ BIT, 2, ADDR_ZERO_PAGE, 3, 0 },{ AND, 2, ADDR_ZERO_PAGE, 3, 0 },{ ROL, 2, ADDR_ZERO_PAGE, 5, 0 },{ SRLA, 2, ADDR_ZERO_PAGE, 5, 0 },
	{ PLP, 1, ADDR_IMPLICIT, 4, 0 },{ AND, 2, ADDR_IMMEDIATE, 2, 0 },{ ROL, 1, ADDR_ACCUMULATOR, 2, 0 },{ SANC, 2, ADDR_IMMEDIATE, 2, 0 },{ BIT, 3, ADDR_ABSOLUTE, 4, 0 },{ AND, 3, ADDR_ABSOLUTE, 4, 0 },{ ROL, 3, ADDR_ABSOLUTE, 6, 0 },{ SRLA, 3, ADDR_ABSOLUTE, 6, 0 },
	{ BMI, 2, ADDR_RELATIVE, 2, 2 },{ AND, 2, ADDR_INDIRECT_Y, 5, 1 },{ SKIL, 0, ADDR_IMPLICIT, 257, 0 },{ SRLA, 2, ADDR_INDIRECT_Y, 8, 0 },{ SNOP, 2, ADDR_ZERO_PAGE_X, 4, 0 },{ AND, 2, ADDR_ZERO_PAGE_X, 4, 0 },{ ROL, 2, ADDR_ZERO_PAGE_X, 6, 0 },{ SRLA, 2, ADDR_ZERO_PAGE_X, 6, 0 },
	{ SEC, 1, ADDR_IMPLICIT, 2, 0 },{ AND, 3, ADDR_ABSOLUTE_Y, 4, 1 },{ SNOP, 1, ADDR_IMPLICIT, 2, 0 },{ SRLA, 3, ADDR_ABSOLUTE_Y, 7, 0 },{ SNOP, 3, ADDR_ABSOLUTE_X, 4, 1 },{ AND, 3, ADDR_ABSOLUTE_X, 4, 1 },{ ROL, 3, ADDR_ABSOLUTE_X, 7, 0 },{ SRLA, 3, ADDR_ABSOLUTE_X, 7, 0 },
	{ RTI, 1, ADDR_IMPLICIT, 6, 0 },{ EOR, 2, ADDR_INDIRECT_X, 6, 0 },{ SKIL, 0, ADDR_IMPLICIT, 257, 0 },{ SSRE, 2, ADDR_INDIRECT_X, 8, 0 },{ SNOP, 2, ADDR_ZERO_PAGE, 3, 0 },{ EOR, 2, ADDR_ZERO_PAGE, 3, 0 },{ LSR, 2, ADDR_ZERO_PAGE, 5, 0 },{ SSRE, 2, ADDR_ZERO_PAGE, 5, 0 },
	{ PHA, 1, ADDR_IMPLICIT, 3, 0 },{ EOR, 2, ADDR_IMMEDIATE, 2, 0 },{ LSR, 1, ADDR_ACCUMULATOR, 2, 0 },{ SALR, 2, ADDR_IMMEDIATE, 2, 0 },{ JMP, 3, ADDR_JMP_ABSOLUTE, 3, 0 },{ EOR, 3, ADDR_ABSOLUTE, 4, 0 },{ LSR, 3, ADDR_ABSOLUTE, 6, 0 },{ SSRE, 3, ADDR_ABSOLUTE, 6, 0 },
	{ BVC, 2, ADDR_RELATIVE, 2, 2 },{ EOR, 2, ADDR_INDIRECT_Y, 5, 1 },{ SKIL, 0, ADDR_IMPLICIT, 257, 0 },{ SSRE, 2, ADDR_INDIRECT_Y, 8, 0 },{ SNOP, 2, ADDR_ZERO_PAGE_X, 4, 0 },{ EOR, 2, ADDR_ZERO_PAGE_X, 4, 0 },{ LSR, 2, ADDR_ZERO_PAGE_X, 6, 0 },{ SSRE, 2, ADDR_ZERO_PAGE_X, 6, 0 },
	{ CLI, 1, ADDR_IMPLICIT, 2, 0 },{ EOR, 3, ADDR_ABSOLUTE_Y, 4, 1 },{ SNOP, 1, ADDR_IMPLICIT, 2, 0 },{ SSRE, 3, ADDR_ABSOLUTE_Y, 7, 0 },{ SNOP, 3, ADDR_ABSOLUTE_X, 4, 1 },{ EOR, 3, ADDR_ABSOLUTE_X, 4, 1 },{ LSR, 3, ADDR_ABSOLUTE_X, 7, 0 },{ SSRE, 3, ADDR_ABSOLUTE_X, 7, 0 },
	{ RTS, 1, ADDR_IMPLICIT, 6, 0 },{ ADC, 2, ADDR_INDIRECT_X, 6, 0 },{ SKIL, 0, ADDR_IMPLICIT, 257, 0 },{ SRRA, 2, ADDR_INDIRECT_X, 8, 0 },{ SNOP, 2, ADDR_ZERO_PAGE, 3, 0 },{ ADC, 2, ADDR_ZERO_PAGE, 3, 0 },{ ROR, 2, ADDR_ZERO_PAGE, 5, 0 },{ SRRA, 2, ADDR_ZERO_PAGE, 5, 0 },
	{ PLA, 1, ADDR_IMPLICIT, 4, 0 },{ ADC, 2, ADDR_IMMEDIATE, 2, 0 },{ ROR, 1, ADDR_ACCUMULATOR, 2, 0 },{ SARR, 2, ADDR_IMMEDIATE, 2, 0 },{ JMP, 3, ADDR_JMP_INDIRECT, 5, 0 },{ ADC, 3, ADDR_ABSOLUTE, 4, 0 },{ ROR, 3, ADDR_ABSOLUTE, 6, 0 },{ SRRA, 3, ADDR_ABSOLUTE, 6, 0 },
	{ BVS, 2, ADDR_RELATIVE, 2, 2 },{ ADC, 2, ADDR_INDIRECT_Y, 5, 1 },{ SKIL, 0, ADDR_IMPLICIT, 257, 0 },{ SRRA, 2, ADDR_INDIRECT_Y, 8, 0 },{ SNOP, 2, ADDR_ZERO_PAGE_X, 4, 0 },{ ADC, 2, ADDR_ZERO_PAGE_X, 4, 0 },{ ROR, 2, ADDR_ZERO_PAGE_X, 6, 0 },{ SRRA, 2, ADDR_ZERO_PAGE_X, 6, 0 },
	{ SEI, 1, ADDR_IMPLICIT, 2, 0 },{ ADC, 3, ADDR_ABSOLUTE_Y, 4, 1 },{ SNOP, 1, ADDR_IMPLICIT, 2, 0 },{ SRRA, 3, ADDR_ABSOLUTE_Y, 7, 0 },{ SNOP, 3, ADDR_ABSOLUTE_X, 4, 1 },{ ADC, 3, ADDR_ABSOLUTE_X, 4, 1 },{ ROR, 3, ADDR_ABSOLUTE_X, 7, 0 },{ SRRA, 3, ADDR_ABSOLUTE_X, 7, 0 },
	{ SNOP, 2, ADDR_IMMEDIATE, 2, 0 },{ STA, 2, ADDR_INDIRECT_X, 6, 0 },{ SNOP, 2, ADDR_IMMEDIATE, 2, 0 },{ SSAX, 2, ADDR_INDIRECT_X, 6, 0 },{ STY, 2, ADDR_ZERO_PAGE, 3, 0 },{ STA, 2, ADDR_ZERO_PAGE, 3, 0 },{ STX, 2, ADDR_ZERO_PAGE, 3, 0 },{ SSAX, 2, ADDR_ZERO_PAGE, 3, 0 },
	{ DEY, 1, ADDR_IMPLICIT, 2, 0 },{ SNOP, 2, ADDR_IMMEDIATE, 2, 0 },{ TXA, 1, ADDR_IMPLICIT, 2, 0 },{ SXAA, 2, ADDR_IMMEDIATE, 2, 0 },{ STY, 3, ADDR_ABSOLUTE, 4, 0 },{ STA, 3, ADDR_ABSOLUTE, 4, 0 },{ STX, 3, ADDR_ABSOLUTE, 4, 0 },{ SSAX, 3, ADDR_ABSOLUTE, 4, 0 },
	{ BCC, 2, ADDR_RELATIVE, 2, 2 },{ STA, 2, ADDR_INDIRECT_Y, 6, 0 },{ SKIL, 0, ADDR_IMPLICIT, 257, 0 },{ SAHX, 2, ADDR_INDIRECT_Y, 6, 0 },{ STY, 2, ADDR_ZERO_PAGE_X, 4, 0 },{ STA, 2, ADDR_ZERO_PAGE_X, 4, 0 },{ STX, 2, ADDR_ZERO_PAGE_Y, 4, 0 },{ SSAX, 2, ADDR_ZERO_PAGE_Y, 4, 0 },
	{ TYA, 1, ADDR_IMPLICIT, 2, 0 },{ STA, 3, ADDR_ABSOLUTE_Y, 5, 0 },{ TXS, 1, ADDR_IMPLICIT, 2, 0 },{ STAS, 3, ADDR_ABSOLUTE_Y, 5, 0 },{ SSHY, 3, ADDR_ABSOLUTE_X, 5, 0 },{ STA, 3, ADDR_ABSOLUTE_X, 5, 0 },{ SSHX, 3, ADDR_ABSOLUTE_Y, 5, 0 },{ SAHX, 3, ADDR_ABSOLUTE_Y, 5, 0 },
	{ LDY, 2, ADDR_IMMEDIATE, 2, 0 },{ LDA, 2, ADDR_INDIRECT_X, 6, 0 },{ LDX, 2, ADDR_IMMEDIATE, 2, 0 },{ SLAX, 2, ADDR_INDIRECT_X, 6, 0 },{ LDY, 2, ADDR_ZERO_PAGE, 3, 0 },{ LDA, 2, ADDR_ZERO_PAGE, 3, 0 },{ LDX, 2, ADDR_ZERO_PAGE, 3, 0 },{ SLAX, 2, ADDR_ZERO_PAGE, 3, 0 },
	{ TAY, 1, ADDR_IMPLICIT, 2, 0 },{ LDA, 2, ADDR_IMMEDIATE, 2, 0 },{ TAX, 1, ADDR_IMPLICIT, 2, 0 },{ SATX, 2, ADDR_IMMEDIATE, 2, 0 },{ LDY, 3, ADDR_ABSOLUTE, 4, 0 },{ LDA, 3, ADDR_ABSOLUTE, 4, 0 },{ LDX, 3, ADDR_ABSOLUTE, 4, 0 },{ SLAX, 3, ADDR_ABSOLUTE, 4, 0 },
	{ BCS, 2, ADDR_RELATIVE, 2, 2 },{ LDA, 2, ADDR_INDIRECT_Y, 5, 1 },{ SKIL, 0, ADDR_IMPLICIT, 257, 0 },{ SLAX, 2, ADDR_INDIRECT_Y, 5, 1 },{ LDY, 2, ADDR_ZERO_PAGE_X, 4, 0 },{ LDA, 2, ADDR_ZERO_PAGE_X, 4, 0 },{ LDX, 2, ADDR_ZERO_PAGE_Y, 4, 0 },{ SLAX, 2, ADDR_ZERO_PAGE_Y, 4, 0 },
	{ CLV, 1, ADDR_IMPLICIT, 2, 0 },{ LDA, 3, ADDR_ABSOLUTE_Y, 4, 1 },{ TSX, 1, ADDR_IMPLICIT, 2, 0 },{ SLAS, 3, ADDR_ABSOLUTE_Y, 4, 0 },{ LDY, 3, ADDR_ABSOLUTE_X, 4, 1 },{ LDA, 3, ADDR_ABSOLUTE_X, 4, 1 },{ LDX, 3, ADDR_ABSOLUTE_Y, 4, 1 },{ SLAX, 3, ADDR_ABSOLUTE_Y, 4, 1 },
	{ CPY, 2, ADDR_IMMEDIATE, 2, 0 },{ CMP, 2, ADDR_INDIRECT_X, 6, 0 },{ SNOP, 2, ADDR_IMMEDIATE, 2, 0 },{ SDCP, 2, ADDR_INDIRECT_X, 8, 0 },{ CPY, 2, ADDR_ZERO_PAGE, 3, 0 },{ CMP, 2, ADDR_ZERO_PAGE, 3, 0 },{ DEC, 2, ADDR_ZERO_PAGE, 5, 0 },{ SDCP, 2, ADDR_ZERO_PAGE, 5, 0 },
	{ INY, 1, ADDR_IMPLICIT, 2, 0 },{ CMP, 2, ADDR_IMMEDIATE, 2, 0 },{ DEX, 1, ADDR_IMPLICIT, 2, 0 },{ SAXS, 2, ADDR_IMMEDIATE, 2, 0 },{ CPY, 3, ADDR_ABSOLUTE, 4, 0 },{ CMP, 3, ADDR_ABSOLUTE, 4, 0 },{ DEC, 3, ADDR_ABSOLUTE, 6, 0 },{ SDCP, 3, ADDR_ABSOLUTE, 6, 0 },
	{ BNE, 2, ADDR_RELATIVE, 2, 2 },{ CMP, 2, ADDR_INDIRECT_Y, 5, 1 },{ SKIL, 0, ADDR_IMPLICIT, 257, 0 },{ SDCP, 2, ADDR_INDIRECT_Y, 8, 0 },{ SNOP, 2, ADDR_ZERO_PAGE_X, 4, 0 },{ CMP, 2, ADDR_ZERO_PAGE_X, 4, 0 },{ DEC, 2, ADDR_ZERO_PAGE_X, 6, 0 },{ SDCP, 2, ADDR_ZERO_PAGE_X, 6, 0 },
	{ CLD, 1, ADDR_IMPLICIT, 2, 0 },{ CMP, 3, ADDR_ABSOLUTE_Y, 4, 1 },{ SNOP, 1, ADDR_IMPLICIT, 2, 0 },{ SDCP, 3, ADDR_ABSOLUTE_Y, 7, 0 },{ SNOP, 3, ADDR_ABSOLUTE_X, 4, 1 },{ CMP, 3, ADDR_ABSOLUTE_X, 4, 1 },{ DEC, 3, ADDR_ABSOLUTE_X, 7, 0 },{ SDCP, 3, ADDR_ABSOLUTE_X, 7, 0 },
	{ CPX, 2, ADDR_IMMEDIATE, 2, 0 },{ SBC, 2, ADDR_INDIRECT_X, 6, 0 },{ SNOP, 2, ADDR_IMMEDIATE, 3, 0 },{ SISB, 2, ADDR_INDIRECT_X, 8, 0 },{ CPX, 2, ADDR_ZERO_PAGE, 3, 0 },{ SBC, 2, ADDR_ZERO_PAGE, 3, 0 },{ INC, 2, ADDR_ZERO_PAGE, 5, 0 },{ SISB, 2, ADDR_ZERO_PAGE, 5, 0 },
	{ INX, 1, ADDR_IMPLICIT, 2, 0 },{ SBC, 2, ADDR_IMMEDIATE, 2, 0 },{ NOP, 1, ADDR_IMPLICIT, 2, 0 },{ SSBC, 2, ADDR_IMMEDIATE, 2, 0 },{ CPX, 3, ADDR_ABSOLUTE, 4, 0 },{ SBC, 3, ADDR_ABSOLUTE, 4, 0 },{ INC, 3, ADDR_ABSOLUTE, 6, 0 },{ SISB, 3, ADDR_ABSOLUTE, 6, 0 },
	{ BEQ, 2, ADDR_RELATIVE, 2, 2 },{ SBC, 2, ADDR_INDIRECT_Y, 5, 1 },{ SKIL, 0, ADDR_IMPLICIT, 257, 0 },{ SISB, 2, ADDR_INDIRECT_Y, 8, 0 },{ SNOP, 2, ADDR_ZERO_PAGE_X, 4, 0 },{ SBC, 2, ADDR_ZERO_PAGE_X, 4, 0 },{ INC, 2, ADDR_ZERO_PAGE_X, 6, 0 },{ SISB, 2, ADDR_ZERO_PAGE_X, 6, 0 },
	{ SED, 1, ADDR_IMPLICIT, 2, 0 },{ SBC, 3, ADDR_ABSOLUTE_Y, 4, 1 },{ SNOP, 1, ADDR_IMPLICIT, 2, 0 },{ SISB, 3, ADDR_ABSOLUTE_Y, 7, 0 },{ SNOP, 3, ADDR_ABSOLUTE_X, 4, 1 },{ SBC, 3, ADDR_ABSOLUTE_X, 4, 1 },{ INC, 3, ADDR_ABSOLUTE_X, 7, 0 },{ SISB, 3, ADDR_ABSOLUTE_X, 7, 0 },
};

uint8_t Cpu::rd8(uint16_t a) const
{
	if (a < 0x2000) return ram[a & 0x7FF];
	if (a >= 0x8000) return prg[(uint32_t)(a - 0x8000) & prg_mask];
	if (a >= 0x6000) return wram[a - 0x6000];
	return 0;
}

void Cpu::init(const uint8_t* prg_view, uint32_t mask, Ppu* p, Apu* a)
{
	memset(this, 0, sizeof(*this));
	ram_ptr = ram;
	prg = prg_view;
	prg_mask = mask;
	ppu = p;
	apu = a;
	port_enabled[0] = port_enabled[1] = true;
	for (int i = 0; i < 0x800; ++i)
		ram[i] = (i & 0x04) ? 0xFF : 0x00;   // FCEUX power-on RAM pattern: 00 00 00 00 FF FF FF FF
	reset();
}

void Cpu::reset()
{
	rp = 0x24;		// P is kept architectural: bit 5 always 1, bit 4 never stored (spec O-2)
	rsp = 0xFD;		// FCEUX has S=$FD at the first instruction (spec O-3)
	rpi = rp;
	jammed = false;
	rpc = rd16(0xFFFC);
	data_bus = (uint8_t)(rpc >> 8);   // the reset vector's high byte was the last byte read (spec O-14)
	// No cycles are charged: FCEUX processes the reset vector fetch inside X6502_Run() at zero extra cost.
	pending_instruction_cycles = 0;
	nmi_state = 0;
}

// FCEUX: X6502_Reset() raises the reset line, which the next X6502_Run() services at no cost by loading the
// vector, clearing the jam and setting P=I. A/X/Y/S, RAM and the cycle count are untouched, any armed NMI
// request is dropped (spec O-41).
void Cpu::hot_reset()
{
	rp = 0x24;
	rpi = rp;
	jammed = false;
	nmi_state = 0;
	rpc = rd16(0xFFFC);
	data_bus = (uint8_t)(rpc >> 8);
}

// FCEUX: PowerNES() = the RAM pattern, InitializeInput(), X6502_Power() and timestampbase=0 (spec O-41).
void Cpu::power()
{
	ra = rx = ry = 0;
	for (int i = 0; i < 0x800; ++i)
		ram[i] = (i & 0x04) ? 0xFF : 0x00;
	cycle = 0;
	apu_cycle = 0;
	memset(pads, 0, sizeof(pads));
	joy_readbit[0] = joy_readbit[1] = 0;
	joy_fc_readbit[0] = joy_fc_readbit[1] = 0;
	last_strobe = 0;
	reset();
}

uint8_t Cpu::mem_read(uint16_t addr)
{
	uint8_t v;
	if (addr < 0x2000)                       // CPU RAM, mirrored every 2 KB
		v = ram[addr & 0x07FF];
	else if (addr < 0x4000)                  // PPU registers, mirrored every 8 bytes
		v = ppu->cpu_read(addr);
	else if (addr == 0x4015)                 // APU status
		v = apu->read_status();
	else if (addr == 0x4016 || addr == 0x4017)   // joypads: bit 0 from the pad, bits 6-7 open bus (spec O-8, O-39)
	{
		const int w = addr & 1;
		const uint8_t pad = w ? pads[1] : pads[0];
		const uint8_t extra = w ? pads[3] : pads[2];
		const unsigned pos = joy_readbit[w];
		uint8_t bit = 0;
		if (port_enabled[w]) {
			bit = pos < 8 ? (uint8_t)((pad >> pos) & 1) :
				pos < 16 ? (uint8_t)((extra >> (pos & 7)) & 1) : 0;
			if (!four_score && pos >= 8) bit |= 1;
			if (four_score && pos == (unsigned)(19 - w)) bit |= 1;
			++joy_readbit[w];
		}
		if (famicom4) {
			const unsigned fp = joy_fc_readbit[w];
			bit = (uint8_t)((bit & 1) | (fp >= 8 ? 2 : ((extra >> fp) & 1) << 1));
			if (fp < 8) ++joy_fc_readbit[w];
		}
		v = (uint8_t)((data_bus & 0xC0) | bit);
	}
	else if (addr >= 0x6000)                 // $6000-$7FFF work RAM, $8000-$FFFF PRG ROM
		v = rd8(addr);
	else                                     // $4000-$4014, $4018-$5FFF: nothing drives the bus
		v = data_bus;
	data_bus = v;
	return v;
}

void Cpu::mem_write(uint16_t addr, uint8_t val)
{
	const uint8_t prev_bus = data_bus;
	data_bus = val;
	if (addr < 0x2000)                       // CPU RAM: $0000-$07FF, mirrored at $0800-$1FFF
	{
		ram[addr & 0x07FF] = val;
		return;
	}
	if (addr <= 0x3FFF)                      // PPU registers: $2000-$2007 mirrored throughout $2000-$3FFF
	{
		ppu->cpu_write(addr, val);
		return;
	}
	// OAM DMA - FCEUX charges 512 cycles (256 DMR + 256 DMW, 1 cycle each). Real hardware charges 513/514 but we
	// match FCEUX for frame-accurate output.
	if (addr == 0x4014)
	{
		// The 256 bytes are read through the CPU bus (any page, side effects included) and written to $2004 one
		// by one (spec O-33).
		data_bus = prev_bus;   // the bus still holds the previous value while the write handler runs
		for (unsigned int i = 0; i < 256; ++i)
		{
			++cycle;   // one cycle per access, so reads of PPU registers see the advancing time
			const uint8_t b = mem_read((uint16_t)((val << 8) | i));
			++cycle;
			ppu->cpu_write(0x2004, b);
			data_bus = b;
		}
		data_bus = val;
		return;
	}
	if ((addr >= 0x4000 && addr <= 0x4013) || addr == 0x4015 || addr == 0x4017)   // APU registers
	{
		apu->write(addr, val);
		return;
	}
	// Joypad strobe: the report restarts on the 1 -> 0 edge of bit 0 (spec O-39)
	if (addr == 0x4016)
	{
		if ((last_strobe & 1) && !(val & 1)) {
			joy_readbit[0] = joy_readbit[1] = 0;
			joy_fc_readbit[0] = joy_fc_readbit[1] = 0;
		}
		last_strobe = val & 1;
		return;
	}
	// $6000-$7FFF work RAM; writes to PRG ROM ($8000-$FFFF) and unmapped space are ignored
	if (addr >= 0x6000 && addr < 0x8000)
		wram[addr - 0x6000] = val;
}

// Runs the instruction at rpc: the generated function for it when the code map has one, otherwise the interpreter.
void Cpu::run_one()
{
	if (code_slot && rpc >= 0x8000) {
		const unsigned slot = code_slot[(uint32_t)(rpc - 0x8000) & prg_mask];
		if (slot) {
			++gen_count;
			code_fns[slot - 1](this);
			return;
		}
	}
	++interp_count;
	step();
}

bool Cpu::step()
{
	const uint8_t opcode = rd8(rpc);
	const int instruction = opcode_items[opcode].instruction;
	const int instruction_size = opcode_items[opcode].instruction_size;
	const int addressing_mode = opcode_items[opcode].addressing_mode;
	int instruction_cycles = opcode_items[opcode].cycles;
	const int page_crossed_extra_cycles = opcode_items[opcode].page_crossed_extra_cycles;

	rpi = rp;
	uint8_t fetched_bus = opcode;   // what the bus holds if the instruction reads nothing else
	{
		// The APU runs up to the end of the instruction's base cost; page-crossing and other extras are only
		// counted by the next call (spec O-25).
		const unsigned int base = (instruction == SKIL) ? 2 : (unsigned)instruction_cycles;
		const uint64_t target = cycle + base;
		const uint32_t stall = apu->advance((uint32_t)(target - apu_cycle), data_bus);
		if (stall) fetched_bus = data_bus;   // a DMC fetch leaves its byte on the bus (spec O-25)
		cycle += stall;
		apu_cycle = target;
	}

	if (instruction == UNK)
		return false;

	// The last byte fetched for this instruction is on the data bus before any data access.
	data_bus = instruction_size > 1 ? rd8((unsigned short)(rpc + instruction_size - 1)) : fetched_bus;

	// Fetch data
	unsigned char temp = 0;
	unsigned short temp_rpc = 0;
	unsigned char op8 = 0;
	unsigned short op16 = 0;

	// Pure store instructions must not read from the destination address —
	// reading PPU registers ($2007 etc.) has side effects (e.g. increments v).
	bool store_only = (instruction == STA || instruction == STX || instruction == STY || instruction == SSAX ||
	                   instruction == SAHX || instruction == STAS || instruction == SSHY || instruction == SSHX);

	// Read-modify-write instructions: the operand is read, written back unchanged, then written again.
	const bool rmw = !store_only && (instruction == ASL || instruction == LSR || instruction == ROL || instruction == ROR ||
	                                 instruction == INC || instruction == DEC || instruction == SSLO || instruction == SRLA ||
	                                 instruction == SSRE || instruction == SRRA || instruction == SDCP || instruction == SISB ||
	                                 instruction == SLAS) && addressing_mode != ADDR_ACCUMULATOR;
	// Indexed accesses first read from the address before the page-crossing correction: always for
	// stores and read-modify-write, only when a page is crossed for loads (spec O-33).
	const bool write_type = store_only || rmw;
	unsigned char rmw_orig = 0;

	// All bus accesses of the instruction happen at its end as far as the PPU is concerned: FCEUX
	// charges the whole base cost before executing (spec O-33).
	pending_instruction_cycles = (unsigned char)instruction_cycles;

	switch (addressing_mode)
	{
	case ADDR_NONE:
		temp = 0;
		break;
	case ADDR_ACCUMULATOR:
		temp = ra;
		break;
	case ADDR_IMMEDIATE:
		temp = OPERAND8;
		break;
	case ADDR_ZERO_PAGE:
		op8 = OPERAND8;
		temp = VAR_ZERO_PAGE(op8);
		break;
	case ADDR_ZERO_PAGE_X:
		op8 = OPERAND8;
		temp = VAR_ZERO_PAGE_X(op8);
		break;
	case ADDR_ZERO_PAGE_Y:
		op8 = OPERAND8;
		temp = VAR_ZERO_PAGE_Y(op8);
		break;
	case ADDR_RELATIVE:
		temp = OPERAND8;
		break;
	case ADDR_ABSOLUTE:
		op16 = OPERAND16;
		if (!store_only) temp = mem_read(op16);
		break;
		case ADDR_ABSOLUTE_X:
	case ADDR_ABSOLUTE_Y:
	{
		op16 = OPERAND16;
		const unsigned short eff = (addressing_mode == ADDR_ABSOLUTE_X) ? ADDRESS_ADD_X(op16) : ADDRESS_ADD_Y(op16);
		const bool crossed = PAGE_CROSSED(op16, eff);
		if (write_type)
			mem_read((unsigned short)((eff & 0x00FF) | (op16 & 0xFF00)));
		else if (crossed)
			mem_read((unsigned short)(eff ^ 0x0100));
		if (crossed)
		{
			instruction_cycles += page_crossed_extra_cycles;
			pending_instruction_cycles = (unsigned char)instruction_cycles;
		}
		if (!store_only) temp = mem_read(eff);
		break;
	}
	case ADDR_INDIRECT_X:
		op8 = OPERAND8;
		if (!store_only) temp = mem_read(ADDRESS_ZERO_PAGE_WRAP(OPERAND_ADD_X(op8)));
		break;
	case ADDR_INDIRECT_Y:
	{
		op8 = OPERAND8;
		const unsigned short ptr = ADDRESS_ZERO_PAGE_WRAP(op8);
		const unsigned short eff = ADDRESS_ADD_Y(ptr);
		const bool crossed = PAGE_CROSSED(ptr, eff);
		if (write_type)
			mem_read((unsigned short)((eff & 0x00FF) | (ptr & 0xFF00)));
		else if (crossed)
			mem_read((unsigned short)(eff ^ 0x0100));
		if (crossed)
		{
			instruction_cycles += page_crossed_extra_cycles;
			pending_instruction_cycles = (unsigned char)instruction_cycles;
		}
		if (!store_only) temp = mem_read(eff);
		break;
	}
	case ADDR_JMP_ABSOLUTE:
		temp_rpc = OPERAND16;
		break;
	case ADDR_JMP_INDIRECT:
		temp_rpc = ADDRESS_INDIRECT_PAGE_WRAP(OPERAND16);
		break;
	default:
		break;
	}

	rmw_orig = temp;
	rpc = (uint16_t)(rpc + instruction_size);
	pending_instruction_cycles = (unsigned char)instruction_cycles;
	if (instruction_size > 0 && !store_only)
	{
		switch (addressing_mode)
		{
		case ADDR_ZERO_PAGE:
		case ADDR_ZERO_PAGE_X:
		case ADDR_ZERO_PAGE_Y:
			data_bus = temp;   // direct zero-page reads do not go through mem_read()
			break;
		default:
			break;
		}
	}

	// Operation
	bool store = false;
	unsigned char temp_out = 0;
	switch (instruction)
	{
	case ADC:
	{
		unsigned short sum = ra + temp + GET_CARRY();
		SET_CARRY(sum > 0xff ? 1 : 0);
		/* The overflow flag is set when the sign of the addends is the same and differs from the sign of the sum */
		/* See: https://stackoverflow.com/questions/29193303/6502-emulation-proper-way-to-implement-adc-and-sbc */
		SET_OVERFLOW(~(ra ^ temp) & (ra ^ sum) & 0x80);
		ra = (unsigned char)sum;
		SET_ZERO(ra);
		SET_NEGATIVE_BY_BIT(ra);
	}
	break;
	case AND:
		ra = ra & temp;
		SET_ZERO(ra);
		SET_NEGATIVE_BY_BIT(ra);
		break;
	case ASL:
		store = true;
		temp_out = temp << 1;
		SET_ZERO(temp_out);
		SET_NEGATIVE_BY_BIT(temp_out);
		SET_CARRY(temp >> 7);
		break;
	case BCC:
		if (GET_CARRY() == 0)
		{
			temp_rpc = rpc;
			rpc = temp_rpc + (char)temp;
			instruction_cycles += BRANCH_EXTRA_CYCLE;
			if (BRANCH_NEW_PAGE())
				instruction_cycles += BRANCH_NEW_PAGE_EXTRA_CYCLE;
		}
		break;
	case BCS:
		if (GET_CARRY() == 1)
		{
			temp_rpc = rpc;
			rpc = temp_rpc + (char)temp;
			instruction_cycles += BRANCH_EXTRA_CYCLE;
			if (BRANCH_NEW_PAGE())
				instruction_cycles += BRANCH_NEW_PAGE_EXTRA_CYCLE;
		}
		break;
	case BEQ:
		if (GET_ZERO() == 1)
		{
			temp_rpc = rpc;
			rpc = temp_rpc + (char)temp;
			instruction_cycles += BRANCH_EXTRA_CYCLE;
			if (BRANCH_NEW_PAGE())
				instruction_cycles += BRANCH_NEW_PAGE_EXTRA_CYCLE;
		}
		break;
	case BIT:
		SET_NEGATIVE_BY_BIT(temp);
		SET_OVERFLOW_BY_BIT(temp);
		SET_ZERO(ra & temp);
		break;
	case BMI:
		if (GET_NEGATIVE() == 1)
		{
			temp_rpc = rpc;
			rpc = temp_rpc + (char)temp;
			instruction_cycles += BRANCH_EXTRA_CYCLE;
			if (BRANCH_NEW_PAGE())
				instruction_cycles += BRANCH_NEW_PAGE_EXTRA_CYCLE;
		}
		break;
	case BNE:
		if (GET_ZERO() == 0)
		{
			temp_rpc = rpc;
			rpc = temp_rpc + (char)temp;
			instruction_cycles += BRANCH_EXTRA_CYCLE;
			if (BRANCH_NEW_PAGE())
				instruction_cycles += BRANCH_NEW_PAGE_EXTRA_CYCLE;
		}
		break;
	case BPL:
		if (GET_NEGATIVE() == 0)
		{
			temp_rpc = rpc;
			rpc = temp_rpc + (char)temp;
			instruction_cycles += BRANCH_EXTRA_CYCLE;
			if (BRANCH_NEW_PAGE())
				instruction_cycles += BRANCH_NEW_PAGE_EXTRA_CYCLE;
		}
		break;
	case BRK:
		rpc++;  // the byte after BRK is skipped: the pushed return address is BRK + 2
		STACK_PUSH_PC();
		STACK_PUSH(rp | 0x10);  // B and bit 5 are set in the pushed copy (spec O-6)
		SET_INTERRUPT(1);
		rpc = rd16(0xFFFE);
		rpi |= 0x04;   // no IRQ right after BRK (spec O-30)
		break;
	case BVC:
		if (GET_OVERFLOW() == 0)
		{
			temp_rpc = rpc;
			rpc = temp_rpc + (char)temp;
			instruction_cycles += BRANCH_EXTRA_CYCLE;
			if (BRANCH_NEW_PAGE())
				instruction_cycles += BRANCH_NEW_PAGE_EXTRA_CYCLE;
		}
		break;
	case BVS:
		if (GET_OVERFLOW() == 1)
		{
			temp_rpc = rpc;
			rpc = temp_rpc + (char)temp;
			instruction_cycles += BRANCH_EXTRA_CYCLE;
			if (BRANCH_NEW_PAGE())
				instruction_cycles += BRANCH_NEW_PAGE_EXTRA_CYCLE;
		}
		break;
	case CLC:
		SET_CARRY(0);
		break;
	case CLD:
		SET_DECIMAL(0);
		break;
	case CLI:
		SET_INTERRUPT(0);
		break;
	case CLV:
		SET_OVERFLOW(0);
		break;
	case CMP:
	{
		SET_CARRY(ra >= temp ? 1 : 0);
		unsigned char diff = ra - temp;
		SET_ZERO(diff);
		SET_NEGATIVE_BY_BIT(diff);
	}
	break;
	case CPX:
	{
		SET_CARRY(rx >= temp ? 1 : 0);
		unsigned char diff = rx - temp;
		SET_ZERO(diff);
		SET_NEGATIVE_BY_BIT(diff);
	}
	break;
	case CPY:
	{
		SET_CARRY(ry >= temp ? 1 : 0);
		unsigned char diff = ry - temp;
		SET_ZERO(diff);
		SET_NEGATIVE_BY_BIT(diff);
	}
	break;
	case DEC:
		store = true;
		temp_out = temp - 1;
		SET_ZERO(temp_out);
		SET_NEGATIVE_BY_BIT(temp_out);
		break;
	case DEX:
		rx = rx - 1;
		SET_ZERO(rx);
		SET_NEGATIVE_BY_BIT(rx);
		break;
	case DEY:
		ry = ry - 1;
		SET_ZERO(ry);
		SET_NEGATIVE_BY_BIT(ry);
		break;
	case EOR:
		ra = ra ^ temp;
		SET_ZERO(ra);
		SET_NEGATIVE_BY_BIT(ra);
		break;
	case INC:
		store = true;
		temp_out = temp + 1;
		SET_ZERO(temp_out);
		SET_NEGATIVE_BY_BIT(temp_out);
		break;
	case INX:
		rx = rx + 1;
		SET_ZERO(rx);
		SET_NEGATIVE_BY_BIT(rx);
		break;
	case INY:
		ry = ry + 1;
		SET_ZERO(ry);
		SET_NEGATIVE_BY_BIT(ry);
		break;
	case JMP:
		rpc = temp_rpc;
		break;
	case JSR:
		rpc--;
		STACK_PUSH_PC();
		rpc = temp_rpc;
		break;
	case LDA:
		ra = temp;
		SET_ZERO(ra);
		SET_NEGATIVE_BY_BIT(ra);
		break;
	case LDX:
		rx = temp;
		SET_ZERO(rx);
		SET_NEGATIVE_BY_BIT(rx);
		break;
	case LDY:
		ry = temp;
		SET_ZERO(ry);
		SET_NEGATIVE_BY_BIT(ry);
		break;
	case LSR:
		store = true;
		temp_out = temp >> 1;
		SET_ZERO(temp_out);
		SET_NEGATIVE_BY_BIT(temp_out);
		SET_CARRY(temp & 0x01);
		break;
	case NOP:
		break;
	case ORA:
		ra = ra | temp;
		SET_ZERO(ra);
		SET_NEGATIVE_BY_BIT(ra);
		break;
	case PHA:
		STACK_PUSH(ra);
		break;
	case PHP:
		STACK_PUSH(rp | 0x10);  // | 0x10 is temprary for test, in future the B flag should be refined.
		break;
	case PLA:
		STACK_POP(ra);
		SET_ZERO(ra);
		SET_NEGATIVE_BY_BIT(ra);
		break;
	case PLP:
		STACK_POP(rp);
		// bit 4 and bit 5 have not hardware implementation.
		rp = rp | 0x20; // bit 5 is always 1
		rp = rp & 0xef; // bit 4 (B flag) is normally 0
		break;
	case ROL:
		store = true;
		temp_out = (temp << 1) | GET_CARRY();
		SET_ZERO(temp_out);
		SET_NEGATIVE_BY_BIT(temp_out);
		SET_CARRY(temp >> 7);
		break;
	case ROR:
		store = true;
		temp_out = (temp >> 1) | (GET_CARRY() ? 0x80 : 0);
		SET_ZERO(temp_out);
		SET_NEGATIVE_BY_BIT(temp_out);
		SET_CARRY(temp & 0x01);
		break;
	case RTI:
		STACK_POP(rp);
		// bit 4 and bit 5 have not hardware implementation.
		rp = rp | 0x20; // bit 5 is always 1
		rp = rp & 0xef; // bit 4 (B flag) is normally 0
		STACK_POP_PC();
		rpi = rp;   // RTI's new I flag is seen by the very next IRQ check (spec O-30)
		break;
	case RTS:
		STACK_POP_PC();
		rpc++;
		break;
	case SBC:
	case SSBC:
		{
			// SBC is equivalent to ADC(~temp), with reversed carry flag.
			unsigned short sum = ra + (~temp) + GET_CARRY();
			SET_CARRY(sum > 0xff ? 0 : 1);
			SET_OVERFLOW(~(ra ^ (~temp)) & (ra ^ sum) & 0x80);
			ra = (unsigned char)sum;
			SET_ZERO(ra);
			SET_NEGATIVE_BY_BIT(ra);
		}
		break;
	case SEC:
		SET_CARRY(1);
		break;
	case SED:
		SET_DECIMAL(1);
		break;
	case SEI:
		SET_INTERRUPT(1);
		break;
	case STA:
		store = true;
		temp_out = ra;
		break;
	case STX:
		store = true;
		temp_out = rx;
		break;
	case STY:
		store = true;
		temp_out = ry;
		break;
	case TAX:
		rx = ra;
		SET_ZERO(rx);
		SET_NEGATIVE_BY_BIT(rx);
		break;
	case TAY:
		ry = ra;
		SET_ZERO(ry);
		SET_NEGATIVE_BY_BIT(ry);
		break;
	case TSX:
		rx = rsp;
		SET_ZERO(rx);
		SET_NEGATIVE_BY_BIT(rx);
		break;
	case TXA:
		ra = rx;
		SET_ZERO(ra);
		SET_NEGATIVE_BY_BIT(ra);
		break;
	case TXS:
		rsp = rx;
		break;
	case TYA:
		ra = ry;
		SET_ZERO(ra);
		SET_NEGATIVE_BY_BIT(ra);
		break;
	case SNOP:
		break;
	case SKIL:
		jammed = true;
		break;  // the CPU stays on the opcode; the 257-cycle cost per repeat is in the table (spec O-7)
	case SANC:
		ra = ra & temp;
		SET_ZERO(ra);
		SET_NEGATIVE_BY_BIT(ra);
		SET_CARRY(ra >> 7);
		break;
	case SALR:
		ra = ra & temp;
		SET_CARRY(ra & 0x01);
		ra = ra >> 1;
		SET_ZERO(ra);
		SET_NEGATIVE_BY_BIT(ra);
		break;
	case SARR:
		ra = ra & temp;
		ra = (unsigned char)((ra >> 1) | (GET_CARRY() ? 0x80 : 0));
		SET_ZERO(ra);
		SET_NEGATIVE_BY_BIT(ra);
		SET_CARRY((ra >> 6) & 0x01);
		SET_OVERFLOW(((ra >> 6) ^ (ra >> 5)) & 0x01);
		break;
	case SXAA:   // unstable on hardware; FCEUX behaves as A = (A | $EE) & X & imm (spec O-31)
		ra = (unsigned char)((ra | 0xEE) & rx & temp);
		SET_ZERO(ra);
		SET_NEGATIVE_BY_BIT(ra);
		break;
	case SATX:   // unstable on hardware; unverified against FCEUX
		ra = temp;
		rx = temp;
		SET_ZERO(ra);
		SET_NEGATIVE_BY_BIT(ra);
		break;
	case SAXS:
		{
			unsigned char ax = (unsigned char)(ra & rx);
			SET_CARRY(ax >= temp);
			rx = (unsigned char)(ax - temp);
			SET_ZERO(rx);
			SET_NEGATIVE_BY_BIT(rx);
		}
		break;
	case SLAS:
		rsp = (unsigned char)(temp & rsp);
		ra = rsp;
		rx = rsp;
		SET_ZERO(ra);
		SET_NEGATIVE_BY_BIT(ra);
		store = true;       // FCEUX treats LAS as read-modify-write: the operand is written back
		temp_out = temp;
		break;
	case SAHX:
	case STAS:
	case SSHY:
	case SSHX:
		{
			// Unstable stores (unverified against FCEUX): the stored value is ANDed with the high byte
			// of the base address plus 1, and a page-crossing index replaces the target's high byte.
			unsigned short base = (addressing_mode == ADDR_INDIRECT_Y) ? (unsigned short)ADDRESS_ZERO_PAGE_WRAP(op8) : op16;
			unsigned char idx = (instruction == SSHY) ? rx : ry;
			unsigned char v;
			if (instruction == SAHX)      v = (unsigned char)(ra & rx);
			else if (instruction == STAS) { rsp = (unsigned char)(ra & rx); v = rsp; }
			else if (instruction == SSHY) v = ry;
			else                          v = rx;
			unsigned short addr = (unsigned short)(base + idx);
			// AHX/TAS use the high byte of the base address, SHY/SHX that of the indexed address.
			const unsigned short hi_src = (instruction == SSHY || instruction == SSHX) ? addr : base;
			v = (unsigned char)(v & ((hi_src >> 8) + 1));
			// SHY/SHX always replace the target's high byte by the stored value (spec O-31); the
			// AHX/TAS stores write to the plain effective address.
			if (instruction == SSHY || instruction == SSHX)
				addr = (unsigned short)((addr & 0x00FF) | (v << 8));
			mem_write(addr, v);
		}
		break;
	case SLAX:
		ra = temp;
		rx = temp;
		SET_ZERO(ra);
		SET_NEGATIVE_BY_BIT(ra);
		break;
	case SSAX:
		store = true;
		temp_out = ra & rx;
		break;
	case SDCP:
		// DEC
		store = true;
		temp_out = temp - 1;
		SET_ZERO(temp_out);
		SET_NEGATIVE_BY_BIT(temp_out);
		// CMP
		{
			SET_CARRY(ra >= temp_out ? 1 : 0);
			unsigned char diff = ra - temp_out;
			SET_ZERO(diff);
			SET_NEGATIVE_BY_BIT(diff);
		}
		break;
	case SISB:
		// INC
		store = true;
		temp_out = temp + 1;
		//SET_ZERO(temp_out);
		//SET_NEGATIVE_BY_BIT(temp_out);
		// SBC
		{
			// SBC is equivalent to ADC(~temp), with reversed carry flag.
			unsigned short sum = ra + (~temp_out) + GET_CARRY();
			SET_CARRY(sum > 0xff ? 0 : 1);
			SET_OVERFLOW(~(ra ^ (~temp_out)) & (ra ^ sum) & 0x80);
			ra = (unsigned char)sum;
			SET_ZERO(ra);
			SET_NEGATIVE_BY_BIT(ra);
		}
		break;
	case SSLO:
		// ASL
		store = true;
		temp_out = temp << 1;
		//SET_ZERO(temp_out);
		//SET_NEGATIVE_BY_BIT(temp_out);
		SET_CARRY(temp >> 7);
		// ORA
		ra = ra | temp_out;
		SET_ZERO(ra);
		SET_NEGATIVE_BY_BIT(ra);
		break;
	case SRLA:
		// ROL
		store = true;
		temp_out = (temp << 1) | GET_CARRY();
		//SET_ZERO(temp_out);
		//SET_NEGATIVE_BY_BIT(temp_out);
		SET_CARRY(temp >> 7);
		// AND
		ra = ra & temp_out;
		SET_ZERO(ra);
		SET_NEGATIVE_BY_BIT(ra);
		break;
	case SSRE:
		// LSR
		store = true;
		temp_out = temp >> 1;
		//SET_ZERO(temp_out);
		//SET_NEGATIVE_BY_BIT(temp_out);
		SET_CARRY(temp & 0x01);
		// EOR
		ra = ra ^ temp_out;
		SET_ZERO(ra);
		SET_NEGATIVE_BY_BIT(ra);
		break;
	case SRRA:
		// ROR
		store = true;
		temp_out = (temp >> 1) | (GET_CARRY() ? 0x80 : 0);
		//SET_ZERO(temp_out);
		//SET_NEGATIVE_BY_BIT(temp_out);
		SET_CARRY(temp & 0x01);
		// ADC
		{
			unsigned short sum = ra + temp_out + GET_CARRY();
			SET_CARRY(sum > 0xff ? 1 : 0);
			/* The overflow flag is set when the sign of the addends is the same and differs from the sign of the sum */
			/* See: https://stackoverflow.com/questions/29193303/6502-emulation-proper-way-to-implement-adc-and-sbc */
			SET_OVERFLOW(~(ra ^ temp_out) & (ra ^ sum) & 0x80);
			ra = (unsigned char)sum;
			SET_ZERO(ra);
			SET_NEGATIVE_BY_BIT(ra);
		}
		break;

	default:
		
		break;
	}

	// Store data
	if (store)
	{
		switch (addressing_mode)
		{
		case ADDR_ACCUMULATOR:
			ra = temp_out;
			break;
		case ADDR_ZERO_PAGE:
			VAR_ZERO_PAGE(op8) = temp_out;
			break;
		case ADDR_ZERO_PAGE_X:
			VAR_ZERO_PAGE_X(op8) = temp_out;
			break;
		case ADDR_ZERO_PAGE_Y:
			VAR_ZERO_PAGE_Y(op8) = temp_out;
			break;
		case ADDR_ABSOLUTE:
			if (rmw) mem_write(op16, rmw_orig);   // dummy write of the unmodified value
			mem_write(op16, temp_out);
			break;
		case ADDR_ABSOLUTE_X:
			if (rmw) mem_write(ADDRESS_ADD_X(op16), rmw_orig);   // dummy write of the unmodified value
			mem_write(ADDRESS_ADD_X(op16), temp_out);
			break;
		case ADDR_ABSOLUTE_Y:
			if (rmw) mem_write(ADDRESS_ADD_Y(op16), rmw_orig);   // dummy write of the unmodified value
			mem_write(ADDRESS_ADD_Y(op16), temp_out);
			break;
		case ADDR_INDIRECT_X:
			if (rmw) mem_write(ADDRESS_ZERO_PAGE_WRAP(OPERAND_ADD_X(op8)), rmw_orig);   // dummy write of the unmodified value
			mem_write(ADDRESS_ZERO_PAGE_WRAP(OPERAND_ADD_X(op8)), temp_out);
			break;
		case ADDR_INDIRECT_Y:
			if (rmw) mem_write(ADDRESS_ADD_Y(ADDRESS_ZERO_PAGE_WRAP(op8)), rmw_orig);   // dummy write of the unmodified value
			mem_write(ADDRESS_ADD_Y(ADDRESS_ZERO_PAGE_WRAP(op8)), temp_out);
			break;
		default:
			break;
		}
	}

	// What is left on the data bus after the instruction (spec O-14): the stored value, or the
	// high byte of the new PC for JSR / indirect JMP / BRK (the last byte read).
	if (store && addressing_mode != ADDR_ACCUMULATOR)   // accumulator shifts write A, not the bus
		data_bus = temp_out;
	if (instruction == JSR || instruction == BRK || (instruction == JMP && addressing_mode == ADDR_JMP_INDIRECT))
		data_bus = (unsigned char)(rpc >> 8);
	// A branch that is not taken never reads its offset byte: the opcode stays on the bus (a taken
	// branch costs extra cycles and leaves the offset).
	if (addressing_mode == ADDR_RELATIVE && instruction_cycles == opcode_items[opcode].cycles)
		data_bus = fetched_bus;
	// Zero-page and immediate NOP variants do not read their operand: the opcode stays on the bus
	// (absolute variants do read; spec O-14).
	if (instruction == SNOP && (addressing_mode == ADDR_ZERO_PAGE || addressing_mode == ADDR_ZERO_PAGE_X ||
	                            addressing_mode == ADDR_IMMEDIATE))
		data_bus = fetched_bus;

	cycle += instruction_cycles;
	pending_instruction_cycles = 0;

	return true;
}

bool Cpu::nmi()
{
	if (jammed) return false;
	STACK_PUSH_PC();
	STACK_PUSH(rp & 0xEF);  // push P with B flag clear
	SET_INTERRUPT(1);
	rpc = rd16(0xFFFA);     // NMI vector at $FFFA/$FFFB
	data_bus = (uint8_t)(rpc >> 8);
	cycle += 7;
	rpi = rp;
	return true;
}

bool Cpu::take_pending_nmi()
{
	if (jammed) return false;
	nmi_state = 0;
	return nmi();
}

void Cpu::irq()
{
	STACK_PUSH_PC();
	STACK_PUSH(rp & 0xEF);
	SET_INTERRUPT(1);
	rpc = rd16(0xFFFE);
	data_bus = (uint8_t)(rpc >> 8);
	cycle += 7;
	rpi = rp;
}

}  // namespace nrt

// ---- services for generated code (nrt_exec.h) ----

extern "C" {

// The start of every instruction: remember P for the IRQ check, then run the APU up to the end of the
// instruction's base cost (spec O-25). Same steps as the top of Cpu::step().
uint8_t nrt_x_begin(nrt_cpu_regs *r, uint8_t opcode, uint8_t apu_cycles)
{
	nrt::Cpu *c = static_cast<nrt::Cpu *>(r);
	c->rpi = c->rp;
	uint8_t fetched_bus = opcode;
	const uint64_t target = c->cycle + apu_cycles;
	const uint32_t stall = c->apu->advance((uint32_t)(target - c->apu_cycle), c->data_bus);
	if (stall) fetched_bus = c->data_bus;   // a DMC fetch leaves its byte on the bus (spec O-25)
	c->cycle += stall;
	c->apu_cycle = target;
	return fetched_bus;
}

uint8_t nrt_x_read(nrt_cpu_regs *r, uint16_t addr) { return static_cast<nrt::Cpu *>(r)->mem_read(addr); }
void nrt_x_write(nrt_cpu_regs *r, uint16_t addr, uint8_t value) { static_cast<nrt::Cpu *>(r)->mem_write(addr, value); }
uint8_t nrt_x_peek(nrt_cpu_regs *r, uint16_t addr) { return static_cast<nrt::Cpu *>(r)->rd8(addr); }

}  // extern "C"

