"""Functions the recompiler found in an address range, with their first ret and the config's names.

python -I tools/funcs.py assets/default.xbe config/game.toml generated/game/x86_func_mapping.cpp <lo hex> <hi hex>
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xbe_re import Xbe  # noqa: E402


def main():
    x = Xbe(sys.argv[1])
    syms = {int(v, 16): k for k, v in re.findall(r"^(\w+)\s*=\s*(0x[0-9A-Fa-f]+)", open(sys.argv[2]).read(), re.M)}
    funcs = sorted(int(m, 16) for m in re.findall(r"\{0x([0-9A-Fa-f]+)u,", open(sys.argv[3]).read()))
    lo, hi = int(sys.argv[4], 16), int(sys.argv[5], 16)
    for i, f in enumerate(funcs):
        if not lo <= f < hi:
            continue
        end = funcs[i + 1] if i + 1 < len(funcs) else f + 0x400
        ret, calls = "", []
        for ins in x.md.disasm(x.read(f, end - f), f):
            if ins.mnemonic == "call" and ins.op_str.startswith("0x"):
                t = int(ins.op_str, 16)
                calls.append(syms.get(t, f"{t:X}"))
            if ins.mnemonic == "ret":
                ret = ins.op_str or "0"
        print(f"{f:08X} size {end - f:5X} ret {ret:<5} {syms.get(f, ''):<36} calls {' '.join(calls[:6])}")


if __name__ == "__main__":
    main()
