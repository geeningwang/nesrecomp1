import base64
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import make_input


class MovieTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.rom = self.root / "own_fixture.nes"
        data = b"NES\x1a" + bytes([1, 0]) + bytes(10) + bytes(16384)
        self.rom.write_bytes(data)
        self.md5 = hashlib.md5(data[16:]).digest()
        self.checksum = base64.b64encode(self.md5).decode("ascii")
        self.movie = self.root / "own_fixture.fm2"
        self.output = self.root / "replay.in"

    def write_movie(self, frames="|0|........|........||\n", extra="", header=""):
        text = "version 3\nromChecksum base64:%s\n%s%s" % (self.checksum, header, extra)
        self.movie.write_text(text + frames, encoding="utf-8")

    def test_all_button_positions(self):
        self.assertEqual(make_input.pad_byte("RLDUTSBA"), 255)
        for i, char in enumerate("RLDUTSBA"):
            self.assertEqual(make_input.pad_byte("." * i + char + "." * (7 - i)), 1 << (7 - i))

    def test_wrong_button_position_and_length_are_rejected(self):
        for text in (".......R", "........A", "????...."):
            with self.subTest(text=text), self.assertRaises(make_input.ReplayError):
                make_input.pad_byte(text)

    def test_two_pads_and_commands_preserved(self):
        self.write_movie("|0|.......A|......B.||\n|3|........|........||\n")
        self.assertEqual(make_input.convert(self.movie, self.rom, self.output), 2)
        text = self.output.read_text()
        self.assertIn("00 01 02 00 00\n03 00 00 00 00\n", text)
        self.assertIn("rom_hash=%016X" % make_input.fnv1a64(self.rom.read_bytes()), text)

    def test_four_score_and_famicom_attachment_preserved(self):
        self.write_movie("|0|.......A|......B.|.....S..|....T...||\n",
                         header="fourscore 1\nport0 1\nport1 1\nport2 3\n")
        make_input.convert(self.movie, self.rom, self.output)
        self.assertIn("fourscore=1\n", self.output.read_text())
        self.assertIn("port2=3\n", self.output.read_text())
        self.assertTrue(self.output.read_text().endswith("00 01 02 04 08\n"))

    def test_disabled_second_port(self):
        self.write_movie("|0|.......A|||\n", header="port0 1\nport1 0\n")
        make_input.convert(self.movie, self.rom, self.output)
        self.assertTrue(self.output.read_text().endswith("00 01 00 00 00\n"))

    def test_unsupported_features_fail_explicitly(self):
        for feature in ("palFlag 1\n", "binary 1\n", "NewPPU 1\n", "RAMInitOption 2\n",
                        "microphone 1\n", "FDS 1\n", "savestate base64:AA==\n", "port2 4\n"):
            with self.subTest(feature=feature):
                self.write_movie(extra=feature)
                with self.assertRaises(make_input.ReplayError):
                    make_input.convert(self.movie, self.rom, self.output)
                self.assertFalse(self.output.exists())

    def test_checksum_is_required_and_game_specific(self):
        self.write_movie()
        self.movie.write_text(self.movie.read_text().replace(self.checksum, "AAAAAAAAAAAAAAAAAAAAAA=="))
        with self.assertRaisesRegex(make_input.ReplayError, "checksum"):
            make_input.convert(self.movie, self.rom, self.output)

    def test_unknown_command_and_malformed_records(self):
        for frame in ("|8|........|........||\n", "|0|........||\n", "|x|........|........||\n",
                      "|0|........|........|payload|\n"):
            with self.subTest(frame=frame):
                self.write_movie(frame)
                with self.assertRaises(make_input.ReplayError):
                    make_input.convert(self.movie, self.rom, self.output)

    def test_empty_movie_invalid_range_and_duplicate_header(self):
        self.write_movie("")
        with self.assertRaisesRegex(make_input.ReplayError, "no frames"):
            make_input.convert(self.movie, self.rom, self.output)
        self.write_movie()
        with self.assertRaises(make_input.ReplayError):
            make_input.convert(self.movie, self.rom, self.output, 2)
        self.write_movie(extra="version 3\n")
        with self.assertRaisesRegex(make_input.ReplayError, "duplicate"):
            make_input.convert(self.movie, self.rom, self.output)

    def test_known_fnv_hash_and_truncated_rom(self):
        self.assertEqual(make_input.fnv1a64(b"hello"), 0xA430D84680AABD0B)
        self.rom.write_bytes(b"NES\x1a")
        with self.assertRaises(make_input.ReplayError):
            make_input.rom_identity(self.rom)


if __name__ == "__main__":
    unittest.main()
