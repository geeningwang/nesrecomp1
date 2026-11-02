import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import build_rom
import cdl
import dbg
from opcodes import OPCODES


class ToolTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def write(self, name, content):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        return path

    def test_prefix_strip_preserves_instruction_and_unprefixed_line(self):
        source = "C - - - - - 0x000041 00:C031: 78        SEI\nlabel:\n    LDA #$00\n"
        stripped = build_rom.BZK_REGEX.sub("   ", source)
        self.assertNotIn("0x000041", stripped)
        self.assertIn("SEI\nlabel:\n    LDA #$00\n", stripped)

    def test_build_metadata_and_include_only_files(self):
        path = self.write("assemble.sh", (
            'NES_OUTPUT_SIMPLE_NAME=_test\n'
            'NES_OUTPUT_FILE_SIZE=16400\n'
            'NES_OUTPUT_FILE_SHA1_ORIGINAL="' + "A" * 40 + '"\n'
            'NES_IGNORE_COMPILE_ASM_ARRAY=(bank_s1.asm copy_bank_s1.asm)\n'
        ))
        metadata = build_rom.parse_assemble_sh(path)
        self.assertEqual(metadata["NES_OUTPUT_FILE_SIZE"], "16400")
        self.assertEqual(metadata["ignore"], {"bank_s1.asm", "copy_bank_s1.asm"})

    def test_missing_and_malformed_build_metadata_fail(self):
        for text in ("", "NES_OUTPUT_SIMPLE_NAME=test\n"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                build_rom.parse_assemble_sh(self.write("bad.sh", text))
        with self.assertRaisesRegex(ValueError, "FILE_SIZE"):
            build_rom.parse_assemble_sh(self.write("bad.sh", (
                "NES_OUTPUT_SIMPLE_NAME=test\nNES_OUTPUT_FILE_SIZE=garbage\n"
                "NES_OUTPUT_FILE_SHA1_ORIGINAL=" + "A" * 40 + "\n"
            )))

    def test_latin_replacement_metadata(self):
        symbol = chr(0x0430)
        path = self.write("prepare.lua", (
            'local files_list = {"bank_FF.asm", "bank_ram.inc"}\n'
            'local replace_with_latin = true\n'
            'local latin_symbols = {\n    ["' + symbol + '"] = "a",\n}\n'
        ))
        self.assertEqual(build_rom.parse_lua_files(path), ["bank_FF.asm", "bank_ram.inc"])
        enabled, table = build_rom.parse_lua_latin(path)
        self.assertTrue(enabled)
        self.assertEqual(table[symbol], "a")

    def test_empty_preparation_or_latin_table_fails(self):
        with self.assertRaisesRegex(ValueError, "files_list"):
            build_rom.parse_lua_files(self.write("empty.lua", "local files_list = {}\n"))
        with self.assertRaisesRegex(ValueError, "symbol table"):
            build_rom.parse_lua_latin(self.write("empty.lua", "local replace_with_latin = true\n"))

    def test_concat_requires_a_real_definition(self):
        path = self.write("assemble.bat", (
            "copy /B header.bin + PRG_ROM.bin + CHR_screens.chr %file_name%.nes > nul\n"
        ))
        self.assertEqual(build_rom.parse_bat_concat(path), ["header.bin", "PRG_ROM.bin", "CHR_screens.chr"])
        with self.assertRaises(ValueError):
            build_rom.parse_bat_concat(self.write("empty.bat", "echo nothing\n"))

    def test_header_validation_and_mapper(self):
        header = bytes([0x4E, 0x45, 0x53, 0x1A, 1, 1, 0xB1, 0x50]) + bytes(8)
        info = build_rom.header_info(header)
        self.assertEqual(info["mapper"], 91)
        self.assertEqual(info["prg_size"], 16384)
        self.assertEqual(info["mirroring"], "vertical")
        for invalid in (b"", header[:15], b"BAD!" + header[4:], header[:7] + b"\x08" + header[8:]):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                build_rom.header_info(invalid)

    def test_readme_crc_is_game_specific(self):
        self.write("README.md", (
            "### Disassembled\n"
            "* Pac-Man (J) (V1.1) [!] `D747B639`\n"
            "* Legend of Zelda, The (U) (PRG0) [!] `D7AE93DF`\n"
            "* Castlevania III - Dracula's Curse (U) [!] `7CC9C669`\n"
            "### WIP\n"
        ))
        self.assertEqual(build_rom.expected_crc(self.root, "Pac-Man"), "D747B639")
        self.assertEqual(build_rom.expected_crc(self.root, "The Legend of Zelda"), "D7AE93DF")
        self.assertEqual(build_rom.expected_crc(self.root, "Castlevania 3"), "7CC9C669")
        with self.assertRaises(ValueError):
            build_rom.expected_crc(self.root, "Unlisted game")

    def test_upstream_exception_is_bound_to_exact_hash_pair(self):
        pair = build_rom.UPSTREAM_MISMATCH["Nuts & Milk"]
        self.assertEqual(pair[1], "8E10AA08B65F3A5FACD2368C40DCCF3AAB61341B")
        self.assertNotEqual(pair, (pair[0], "0" * 40))
        self.assertNotIn("Double Dragon II", build_rom.UPSTREAM_MISMATCH)
        self.assertNotIn("Street Fighter 3", build_rom.UPSTREAM_MISMATCH)

    def test_cdl_marks_operand_bytes_and_rejects_truncation(self):
        path = self.root / "test.cdl"
        path.write_bytes(bytes([1, 1, 0, 3, 0x10, 0x40]))
        log = cdl.Cdl(path, 6)
        self.assertEqual(log.runs(), [(0, 2), (3, 4)])
        self.assertEqual(log.counts()["code"], 3)
        self.assertTrue(log.is_code(1))
        with self.assertRaisesRegex(ValueError, "shorter"):
            cdl.Cdl(path, 7)

    def test_debug_bank_mapping_and_quoted_commas(self):
        self.assertEqual(dbg._fields('name="file, with comma",id=4')["name"], "file, with comma")
        path = self.write("test.dbg", (
            'version\tmajor=2,minor=0\n'
            'seg\tid=0,name="BANK_FF",start=0xC000,size=0x100,addrsize=absolute,ooffs=0\n'
            'mod\tid=0,name="copy_bank_FF.o",file=0\n'
            'scope\tid=0,name="",mod=0\n'
            'sym\tid=0,name="vec_RESET",scope=0,val=0xC000,type=lab\n'
            'sym\tid=1,name="constant",scope=0,val=0x20,type=equ\n'
        ))
        data = dbg.Dbg(path)
        self.assertEqual(data.prg_segments(), [("BANK_FF", 0xC000, 0x100, 0)])
        self.assertEqual(data.labels(), [("vec_RESET", 0xC000, "BANK_FF")])

    def test_opcode_table_includes_verified_edge_cases(self):
        self.assertEqual(len(OPCODES), 256)
        self.assertEqual(OPCODES[0xE2][3], 3)
        self.assertEqual(OPCODES[0xBB][3:], (4, 0))
        self.assertEqual(sum(op[0] == "*KIL" for op in OPCODES), 12)

    def fixture(self, missing_chr=False, split=False, embedded_chr=False):
        game = self.root / "Test Game"
        game.mkdir()
        header = b"NES\x1a" + bytes([1, 1 if missing_chr or split or embedded_chr else 0]) + bytes(10)
        prg = bytes(16384)
        chr_data = bytes(8192) if missing_chr or split or embedded_chr else b""
        rom = header + prg + chr_data
        sha = hashlib.sha1(rom).hexdigest().upper()
        crc = "%08X" % (zlib.crc32(rom) & 0xFFFFFFFF)
        self.write("README.md", "### Disassembled\n* Test Game (U) [!] `" + crc + "`\n### WIP\n")
        self.write("Test Game/assemble.sh", (
            "NES_OUTPUT_SIMPLE_NAME=test\nNES_OUTPUT_FILE_SIZE=%d\n" % len(rom)
            + "NES_OUTPUT_FILE_SHA1_ORIGINAL=" + sha + "\n"
            + "NES_IGNORE_COMPILE_ASM_ARRAY=(copy_bank_s1.asm)\n"
        ))
        self.write("Test Game/preparations.lua", (
            'local files_list = {"bank_FF.asm", "bank_s1.asm"}\n'
            "local replace_with_latin = false\n"
        ))
        self.write("Test Game/bank_FF.asm", "    SEI\n")
        self.write("Test Game/bank_s1.asm", "; include only\n")
        parts = "header.bin + PRG_ROM.bin + CHR_screens.chr" if split else "header.bin + PRG_ROM.bin + CHR_ROM.chr"
        self.write("Test Game/assemble.bat", "copy /B " + parts + " %file_name%.nes\n")
        (game / "header.bin").write_bytes(header)
        if split:
            self.write("Test Game/split.lua", (
                'local size_prg = 0x4000\nlocal size_chr = 0x2000\nlocal name_chr = "CHR_screens.chr"\n'
            ))

        def tool_run(cmd, cwd):
            if cmd[0] == "ld65":
                work = Path(cwd)
                (work / "PRG_ROM.bin").write_bytes(prg + (chr_data if split or embedded_chr else b""))
                (work / "game.dbg").write_text("version\tmajor=2,minor=0\n")
                (work / "game.map").write_text("test\n")
            return 0, ""

        return tool_run

    def test_missing_required_chr_fails_instead_of_partial_rom(self):
        run = self.fixture(missing_chr=True)
        with mock.patch.object(build_rom, "run", side_effect=run), self.assertRaises(FileNotFoundError):
            build_rom.build_game(str(self.root), "Test Game", str(self.root / "out"), "ca65", "ld65", True)
        self.assertFalse((self.root / "out/test_game/game.nes").exists())

    def test_chr_ram_build_and_include_exclusion(self):
        run = self.fixture()
        with mock.patch.object(build_rom, "run", side_effect=run) as runner:
            info = build_rom.build_game(str(self.root), "Test Game", str(self.root / "out"), "ca65", "ld65", True)
        self.assertTrue(info["identity_ok"])
        self.assertEqual(info["parts"], ["header.bin", "PRG_ROM.bin"])
        self.assertEqual(info["bank_files"], ["copy_bank_FF.o"])
        self.assertEqual(runner.call_count, 2)
        with (self.root / "out/test_game/rom_info.json").open() as stream:
            self.assertTrue(json.load(stream)["crc32_ok"])

    def test_split_output_reassembles_expected_rom(self):
        run = self.fixture(split=True)
        with mock.patch.object(build_rom, "run", side_effect=run):
            info = build_rom.build_game(str(self.root), "Test Game", str(self.root / "out"), "ca65", "ld65", False)
        self.assertTrue(info["identity_ok"])
        self.assertEqual(info["parts"], ["header.bin", "PRG_ROM.bin", "CHR_screens.chr"])
        self.assertFalse((self.root / "out/test_game/work").exists())

    def test_linked_chr_data_is_not_required_a_second_time(self):
        run = self.fixture(embedded_chr=True)
        with mock.patch.object(build_rom, "run", side_effect=run):
            info = build_rom.build_game(str(self.root), "Test Game", str(self.root / "out"), "ca65", "ld65", False)
        self.assertTrue(info["identity_ok"])
        self.assertEqual(info["size"], 16 + 16384 + 8192)


if __name__ == "__main__":
    unittest.main()
