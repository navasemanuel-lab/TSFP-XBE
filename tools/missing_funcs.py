"""Code addresses held in the image's data that are not recompiled functions but look like function starts: candidates
for config [functions], found before an indirect call reaches them.

python -I tools/missing_funcs.py assets/default.xbe generated/game/x86_func_mapping.cpp
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xbe_re import Xbe  # noqa: E402


def main():
    x = Xbe(sys.argv[1])
    funcs = set(int(m, 16) for m in re.findall(r"\{0x([0-9A-Fa-f]+)u,", open(sys.argv[2]).read()))
    text = next(s for s in x.sections if s[0] == ".text")
    lo, hi = text[1], text[1] + text[4]
    code = x.raw[text[3]:text[3] + text[4]]

    def byte(va):
        return code[va - lo]

    found = {}
    for s in x.sections:
        name, va, _, raw, rsize = s[0], s[1], s[2], s[3], s[4]
        if name == ".text" or name.startswith("$$") or rsize < 4:
            continue
        data = x.raw[raw:raw + rsize]
        for off in range(0, rsize - 3, 4):
            t = struct.unpack_from("<I", data, off)[0]
            if not (lo <= t < hi) or t in funcs or t & 0xF:
                continue
            prev = byte(t - 1)
            first = byte(t)
            # after padding or a return, and a usual first instruction
            if prev not in (0xCC, 0xC3, 0x90, 0xC2) and byte(t - 3) != 0xC2:
                continue
            if first not in (0x55, 0x53, 0x56, 0x57, 0x51, 0x83, 0x81, 0x8B, 0x6A, 0x68, 0xA1, 0x33, 0xB8, 0xD9, 0xF3, 0x0F,
                             0x8D, 0xE8, 0xE9, 0xC3, 0xC2, 0x80, 0xF6, 0x85, 0x64, 0x52, 0x50, 0xDD, 0x66):
                continue
            found.setdefault(t, f"{name}+{off:X}")
    for t in sorted(found):
        print(f"{t:08X}  (held at {found[t]})")
    print(f"{len(found)} candidates", file=sys.stderr)


if __name__ == "__main__":
    main()
