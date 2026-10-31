"""ld65 --dbgfile reader: segments, labels, line/span tables.

Only what the analysis needs: the segment layout (CPU address <-> PRG file offset) and the labels
with the segment (bank) they are defined in.
"""
import re


def _fields(rest):
    d = {}
    for m in re.finditer(r'(\w+)=("[^"]*"|[^,]*)', rest):
        v = m.group(2)
        d[m.group(1)] = v[1:-1] if v.startswith('"') else v
    return d


def _num(v):
    return int(v, 0)


class Dbg:
    def __init__(self, path):
        self.segs, self.syms, self.lines, self.spans = {}, [], {}, {}
        self.mods, self.scopes = {}, {}
        with open(path, encoding="utf-8", errors="replace") as fh:
            for raw in fh:
                kind, _, rest = raw.rstrip("\n").partition("\t")
                f = _fields(rest)
                if kind == "seg":
                    self.segs[int(f["id"])] = {
                        "name": f["name"], "start": _num(f["start"]), "size": _num(f["size"]),
                        "ooffs": int(f["ooffs"]) if "ooffs" in f else None, "oname": f.get("oname")}
                elif kind == "span":
                    self.spans[int(f["id"])] = (int(f["seg"]), int(f["start"]), int(f["size"]))
                elif kind == "line":
                    self.lines[int(f["id"])] = [int(x) for x in f["span"].split("+")] if "span" in f else []
                elif kind == "mod":
                    self.mods[int(f["id"])] = f["name"]
                elif kind == "scope":
                    self.scopes[int(f["id"])] = int(f["mod"])
                elif kind == "sym":
                    self.syms.append(f)

    def prg_segments(self):
        """Segments that end up in the PRG image: (name, cpu_start, size, prg_offset)."""
        return [(s["name"], s["start"], s["size"], s["ooffs"]) for s in self.segs.values()
                if s["ooffs"] is not None and s["size"] > 0]

    def labels(self):
        """[(name, cpu_addr, seg_name_or_None)] for address labels (type=lab) with a defining segment."""
        out = []
        for s in self.syms:
            if s.get("type") != "lab" or "val" not in s:
                continue
            # the object module (copy_bank_XX.o) names the bank: BANK_XX (+ BANK_XXi / VECTORS_XX variants)
            mod = self.mods.get(self.scopes.get(int(s.get("scope", -1)), -1), "")
            seg = "BANK_" + mod[len("copy_bank_"):-2] if mod.startswith("copy_bank_") and mod.endswith(".o") else None
            out.append((s["name"], _num(s["val"]), seg))
        return out
