"""Instructions in .text using an immediate, grouped by the recompiled function they are in.

python -I tools/find_imm.py assets/default.xbe generated/game/x86_func_mapping.cpp <hex imm> [mnemonic,...]
"""
import bisect
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xbe_re import Xbe  # noqa: E402


def main():
    x = Xbe(sys.argv[1])
    x.md.skipdata = True
    funcs = sorted(int(m, 16) for m in re.findall(r"\{0x([0-9A-Fa-f]+)u,", open(sys.argv[2]).read()))
    imm = int(sys.argv[3], 16)
    mnems = set(sys.argv[4].split(",")) if len(sys.argv) > 4 else {"test", "and", "or", "cmp", "mov"}
    s = next(s for s in x.sections if s[0] == ".text")
    data = x.raw[s[3]:s[3] + s[4]]
    want = f", {imm:#x}"
    for ins in x.md.disasm(data, s[1]):
        if ins.mnemonic in mnems and ins.op_str.endswith(want):
            f = funcs[bisect.bisect_right(funcs, ins.address) - 1]
            print(f"{ins.address:08X} {ins.mnemonic} {ins.op_str:<36} in {f:08X}")


if __name__ == "__main__":
    main()
