"""Code addresses the code itself names (push imm32, mov ..., imm32) that are not recompiled functions: callbacks the
function finder missed, for config [functions] before an indirect call reaches them.

python -I tools/missing_callbacks.py assets/default.xbe generated/game/x86_func_mapping.cpp
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xbe_re import Xbe  # noqa: E402


def main():
    x = Xbe(sys.argv[1])
    x.md.skipdata = True
    funcs = set(int(m, 16) for m in re.findall(r"\{0x([0-9A-Fa-f]+)u,", open(sys.argv[2]).read()))
    text = next(s for s in x.sections if s[0] == ".text")
    lo, hi = text[1], text[1] + text[4]
    data = x.raw[text[3]:text[3] + text[4]]
    def at(va, n=1):
        return data[va - lo:va - lo + n]

    def start_like(t):
        # a usual first instruction, after padding, a return or a jump table (code addresses before it)
        b = at(t, 3)
        first = b[0] in (0x55, 0x56, 0x53, 0x57, 0x51, 0x6A, 0xA1, 0x68) or b[:2] in (b"\x83\xec", b"\x81\xec", b"\x8b\x44",
                                                                                     b"\x8b\x4c", b"\x8b\x54", b"\x8b\x0d",
                                                                                     b"\x8b\x15", b"\xd9\x44", b"\xf3\x0f")
        prev = at(t - 1)[0]
        table = lo <= int.from_bytes(at(t - 4, 4), "little") < hi
        return first and (prev in (0xCC, 0xC3, 0x90) or at(t - 3)[0] == 0xC2 or table)

    found = {}
    for ins in x.md.disasm(data, lo):
        if ins.mnemonic not in ("push", "mov"):
            continue
        m = re.search(r"(?:^|, )(0x[0-9a-f]+)$", ins.op_str)
        if not m:
            continue
        t = int(m.group(1), 16)
        if lo <= t < hi and t not in funcs and t % 16 == 0 and start_like(t):
            found.setdefault(t, ins.address)
    for t in sorted(found):
        print(f"{t:08X}  (named at {found[t]:08X})")
    print(f"{len(found)} candidates", file=sys.stderr)


if __name__ == "__main__":
    main()
