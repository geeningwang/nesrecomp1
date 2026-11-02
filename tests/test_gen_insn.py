import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import gen_insn  # noqa: E402


def tiny_rom(prg_bytes, banks=1):
    prg = bytearray(16384 * banks)
    prg[:len(prg_bytes)] = prg_bytes
    # vectors: NMI, RESET, IRQ all at $8000 (or $C000 for a 16 KB image the CPU sees at both)
    base = 0x8000 if banks == 2 else 0xC000
    for k in range(3):
        prg[len(prg) - 6 + 2 * k] = base & 0xFF
        prg[len(prg) - 5 + 2 * k] = base >> 8
    return b"NES\x1a" + bytes([banks, 1, 0, 0]) + bytes(8) + bytes(prg) + bytes(8192)


class GeneratorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.it = gen_insn.Interpreter()

    def test_interpreter_table_is_complete(self):
        self.assertEqual(len(self.it.table), 256)
        names = {t[0] for t in self.it.table}
        modes = {t[2] for t in self.it.table}
        for name in names - {"UNK"}:
            self.assertIn(name, self.it.ops, name)
        for mode in modes:
            if mode not in self.it.fetch:                        # falls to the interpreter's `default: break;`
                self.assertEqual(self.it.fetch["default"].replace("break;", "").strip(), "", mode)
                self.assertEqual(mode, "ADDR_IMPLICIT")

    def test_every_opcode_generates_balanced_c(self):
        for op in range(256):
            size = self.it.table[op][1]
            prg = bytes([op, 0x12, 0x34]) + bytes(16384 - 3)
            text = gen_insn.gen_function(self.it, prg, 0, "nrt_t")
            self.assertIsNotNone(text, hex(op))
            self.assertEqual(text.count("{"), text.count("}"), hex(op))
            self.assertIn("static int nrt_t_i_0000(nrt_cpu_regs *c)", text)
            self.assertIn("nrt_x_begin(c, 0x%02X," % op, text)
            if size >= 2:
                self.assertIn("k_op8 = 0x12", text)
            if size == 3:
                self.assertIn("k_op16 = 0x3412", text)

    def test_constants_for_a_known_instruction(self):
        prg = bytes([0xBD, 0xFE, 0x10]) + bytes(16384 - 3)      # LDA $10FE,X
        text = gen_insn.gen_function(self.it, prg, 0, "nrt_t")
        self.assertIn("const int instruction = LDA;", text)
        self.assertIn("const int addressing_mode = ADDR_ABSOLUTE_X;", text)
        self.assertIn("int instruction_cycles = 4;", text)
        self.assertIn("page_crossed_extra_cycles = 1;", text)
        self.assertIn("const int store_only = 0;", text)
        self.assertIn("data_bus = instruction_size > 1 ? 0x10 : fetched_bus;", text)
        sta = gen_insn.gen_function(self.it, bytes([0x9D, 0, 2]) + bytes(16381), 0, "nrt_t")
        self.assertIn("const int store_only = 1;", sta)
        inc = gen_insn.gen_function(self.it, bytes([0xFE, 0, 2]) + bytes(16381), 0, "nrt_t")
        self.assertIn("const int rmw = 1;", inc)

    def test_instruction_running_off_the_image_is_skipped(self):
        prg = bytes(16383) + bytes([0xAD])                       # LDA abs with no operand bytes left
        self.assertIsNone(gen_insn.gen_function(self.it, prg, 16383, "nrt_t"))

    def test_kil_keeps_the_interpreter_cycle_model(self):
        text = gen_insn.gen_function(self.it, bytes([0x02]) + bytes(16383), 0, "nrt_t")
        self.assertIn("nrt_x_begin(c, 0x02, 2)", text)           # the APU runs only 2 cycles per KIL repeat
        self.assertIn("int instruction_cycles = 257;", text)

    def test_rom_hash_definition(self):
        # FNV-1a 64 over prg_size and chr_size (u32 LE), then PRG, then CHR - the definition of nrt_rom_hash()
        h = 14695981039346656037
        for byte in (1, 0, 0, 0, 2, 0, 0, 0, 7, 9, 8):
            h = ((h ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
        self.assertEqual(gen_insn.fnv_rom_hash(bytes([7]), bytes([9, 8])), h)

    def test_plain_rom_code_map_follows_control_flow(self):
        # LDX #0 / loop: INX / BNE loop / JMP done / done: JMP done
        code = bytes([0xA2, 0x00, 0xE8, 0xD0, 0xFD, 0x4C, 0x08, 0xC0, 0x4C, 0x08, 0xC0])
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "t.nes")
            open(path, "wb").write(tiny_rom(code))
            game = gen_insn.PlainRom(path)
            starts, unresolved, computed, sources = gen_insn.code_map(game, game.vector_seeds(), [])
            self.assertEqual(sorted(starts), [0, 2, 3, 5, 8])
            self.assertEqual(len(unresolved), 0)

    def test_brk_falls_through_to_its_return_address(self):
        code = bytes([0x00, 0xFF, 0xA9, 0x01, 0x4C, 0x04, 0xC0])      # BRK, pad, LDA #1, JMP self
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "t.nes")
            open(path, "wb").write(tiny_rom(code))
            game = gen_insn.PlainRom(path)
            starts, _, _, _ = gen_insn.code_map(game, game.vector_seeds(), [])
            self.assertEqual(sorted(starts), [0, 2, 4])

    def test_executed_pc_feedback_adds_coverage(self):
        code = bytes([0x4C, 0x10, 0xC0]) + bytes(13) + bytes([0xEA, 0x4C, 0x10, 0xC0])
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "t.nes")
            open(path, "wb").write(tiny_rom(code))
            game = gen_insn.PlainRom(path)
            base, _, _, _ = gen_insn.code_map(game, game.vector_seeds(), [])
            self.assertNotIn(0x20, base)
            more, _, _, sources = gen_insn.code_map(game, game.vector_seeds(), [0xC020])
            self.assertIn(0x20, more)
            self.assertEqual(sources["executed_pcs_only"], len(more - base))


if __name__ == "__main__":
    unittest.main()
