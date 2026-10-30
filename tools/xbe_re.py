"""Small reverse engineering helper for the TSFP XBE: sections, strings, references, disassembly.

python -I tools/xbe_re.py assets/default.xbe sections
python -I tools/xbe_re.py assets/default.xbe strings <regex>
python -I tools/xbe_re.py assets/default.xbe xrefs <hex address>
python -I tools/xbe_re.py assets/default.xbe dis <hex address> [count]
python -I tools/xbe_re.py assets/default.xbe floats <value> [tolerance]
"""
import re
import struct
import sys

import capstone


class Xbe:
    def __init__(self, path):
        self.raw = open(path, "rb").read()
        r = self.raw
        self.base = struct.unpack_from("<I", r, 0x104)[0]
        n, hdr = struct.unpack_from("<II", r, 0x11C)
        self.sections = []
        for i in range(n):
            o = hdr - self.base + i * 56
            flags, va, vsize, raw_addr, raw_size, name_addr = struct.unpack_from("<IIIIII", r, o)
            no = name_addr - self.base
            name = r[no:r.index(b"\0", no)].decode("ascii", "replace")
            self.sections.append((name, va, vsize, raw_addr, raw_size, flags))
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    def section_of(self, va):
        for s in self.sections:
            if s[1] <= va < s[1] + s[2]:
                return s
        return None

    def read(self, va, size):
        s = self.section_of(va)
        if not s:
            return None
        off = va - s[1]
        if off >= s[4]:
            return b"\0" * size
        return self.raw[s[3] + off:s[3] + min(off + size, s[4])]

    def code_sections(self):
        return [s for s in self.sections if s[5] & 4]  # executable flag

    def iter_all(self):
        for s in self.sections:
            yield s, self.raw[s[3]:s[3] + s[4]]

    def strings(self, pattern):
        rx = re.compile(pattern.encode(), re.I)
        for s, data in self.iter_all():
            for m in re.finditer(rb"[\x20-\x7e]{4,}", data):
                if rx.search(m.group()):
                    yield s[1] + m.start(), m.group().decode()
            # UTF-16LE
            for m in re.finditer(rb"(?:[\x20-\x7e]\x00){4,}", data):
                t = m.group().decode("utf-16le")
                if rx.search(t.encode()):
                    yield s[1] + m.start(), "L\"" + t + "\""

    def xrefs(self, target):
        needle = struct.pack("<I", target)
        for s, data in self.iter_all():
            start = 0
            while True:
                i = data.find(needle, start)
                if i < 0:
                    break
                yield s[0], s[1] + i
                start = i + 1
        # relative calls / jumps
        for s in self.code_sections():
            data = self.raw[s[3]:s[3] + s[4]]
            for i in range(len(data) - 5):
                if data[i] in (0xE8, 0xE9):
                    rel = struct.unpack_from("<i", data, i + 1)[0]
                    if s[1] + i + 5 + rel == target:
                        yield s[0] + (" call" if data[i] == 0xE8 else " jmp"), s[1] + i

    def dis(self, va, count=40):
        data = self.read(va, count * 15)
        out = []
        for ins in self.md.disasm(data, va):
            out.append(f"{ins.address:08X}  {ins.bytes.hex():<20} {ins.mnemonic} {ins.op_str}")
            count -= 1
            if count <= 0:
                break
        return out

    def func_start(self, va):
        """Walks back to the nearest likely prologue (after int3/nop/ret padding)."""
        data_s = self.section_of(va)
        lo = data_s[1]
        p = va
        while p > lo:
            b = self.read(p - 1, 1)[0]
            if b in (0xCC, 0x90) and self.read(p, 1)[0] not in (0xCC, 0x90):
                return p
            p -= 1
        return lo

    def floats(self, value, tol=1e-4):
        for s, data in self.iter_all():
            for i in range(0, len(data) - 3):
                f = struct.unpack_from("<f", data, i)[0]
                if abs(f - value) <= tol:
                    yield s[0], s[1] + i


def main():
    x = Xbe(sys.argv[1])
    cmd = sys.argv[2]
    if cmd == "sections":
        print(f"base {x.base:08X}")
        for name, va, vsize, ra, rs, flags in x.sections:
            print(f"{name:<12} va {va:08X} size {vsize:08X} raw {ra:08X} rsize {rs:08X} flags {flags:X}")
    elif cmd == "strings":
        for va, t in x.strings(sys.argv[3]):
            print(f"{va:08X}  {t}")
    elif cmd == "xrefs":
        for sec, va in x.xrefs(int(sys.argv[3], 16)):
            print(f"{va:08X}  {sec}")
    elif cmd == "dis":
        n = int(sys.argv[4]) if len(sys.argv) > 4 else 40
        print("\n".join(x.dis(int(sys.argv[3], 16), n)))
    elif cmd == "start":
        print(f"{x.func_start(int(sys.argv[3], 16)):08X}")
    elif cmd == "floats":
        tol = float(sys.argv[4]) if len(sys.argv) > 4 else 1e-4
        for sec, va in x.floats(float(sys.argv[3]), tol):
            print(f"{va:08X}  {sec}")


if __name__ == "__main__":
    main()
