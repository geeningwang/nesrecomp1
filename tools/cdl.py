"""FCEUX Code/Data Logger (.cdl) reader.

One byte per PRG ROM byte (CHR bytes follow); flags, as written by FCEUX's debugger (debug.cpp):
    0x01 code (executed; every byte of the instruction, opcode and operands)
    0x02 data (read as data by an instruction)
    0x0C two address bits of the 4 KB slot the byte was seen in (CPU address bits 11-12)
    0x10 code reached indirectly (JMP (ind) / RTS-style computed jump target)
    0x20 data accessed indirectly (through a pointer)
    0x40 PCM sample data (DMC fetch)
    0x80 set = byte was accessed in the lower ($6000-$7FFF) CPU area
"""
import os

CODE, DATA, IND_CODE, IND_DATA, PCM = 0x01, 0x02, 0x10, 0x20, 0x40


class Cdl:
    def __init__(self, path, prg_size):
        with open(path, "rb") as fh:
            raw = fh.read()
        self.path = path
        self.prg = raw[:prg_size]
        self.chr = raw[prg_size:]
        if len(raw) < prg_size:
            raise ValueError("%s: %d bytes is shorter than the PRG size %d" % (path, len(raw), prg_size))

    def is_code(self, off):
        return bool(self.prg[off] & CODE)

    def runs(self):
        """[(start, end_exclusive)] of contiguous code bytes."""
        out, i, n = [], 0, len(self.prg)
        while i < n:
            if self.prg[i] & CODE:
                j = i
                while j < n and self.prg[j] & CODE:
                    j += 1
                out.append((i, j))
                i = j
            else:
                i += 1
        return out

    def counts(self):
        c = {"code": 0, "data": 0, "indirect_code": 0, "indirect_data": 0, "pcm": 0, "unused": 0}
        for b in self.prg:
            if b & CODE: c["code"] += 1
            if b & DATA: c["data"] += 1
            if b & IND_CODE: c["indirect_code"] += 1
            if b & IND_DATA: c["indirect_data"] += 1
            if b & PCM: c["pcm"] += 1
            if not (b & (CODE | DATA | PCM)): c["unused"] += 1
        return c


def find_cdl(game_dir):
    misc = os.path.join(game_dir, "_misc")
    if not os.path.isdir(misc):
        return None
    for f in sorted(os.listdir(misc)):
        if f.lower().endswith(".cdl"):
            return os.path.join(misc, f)
    return None
