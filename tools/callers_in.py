"""Direct calls from an address range into functions of another range.

python -I tools/callers_in.py assets/default.xbe generated/game/x86_func_mapping.cpp <callee lo> <callee hi> <caller lo> <caller hi>
"""
import os
import re
import struct
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xbe_re import Xbe  # noqa: E402


def main():
    x = Xbe(sys.argv[1])
    funcs = set(int(m, 16) for m in re.findall(r"\{0x([0-9A-Fa-f]+)u,", open(sys.argv[2]).read()))
    tlo, thi, clo, chi = (int(v, 16) for v in sys.argv[3:7])
    data = x.read(clo, chi - clo)
    hits = defaultdict(list)
    for i in range(len(data) - 5):
        if data[i] != 0xE8:
            continue
        t = clo + i + 5 + struct.unpack_from("<i", data, i + 1)[0]
        if tlo <= t < thi and t in funcs:
            hits[t].append(clo + i)
    for t in sorted(hits):
        print(f"{t:08X} x{len(hits[t]):<3} " + " ".join(f"{a:08X}" for a in hits[t][:10]))


if __name__ == "__main__":
    main()
