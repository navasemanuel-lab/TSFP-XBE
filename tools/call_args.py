"""The instructions before each call to a function, to read the arguments pushed at every call site.

python -I tools/call_args.py assets/default.xbe <callee hex> [instructions before, default 6]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xbe_re import Xbe  # noqa: E402


def main():
    x = Xbe(sys.argv[1])
    x.md.skipdata = True
    callee = int(sys.argv[2], 16)
    back = int(sys.argv[3]) if len(sys.argv) > 3 else 6
    s = next(s for s in x.sections if s[0] == ".text")
    data = x.raw[s[3]:s[3] + s[4]]
    window = []
    for ins in x.md.disasm(data, s[1]):
        window = (window + [ins])[-(back + 1):]
        if ins.mnemonic in ("call", "jmp") and ins.op_str == hex(callee):
            print(f"-- {ins.address:08X}")
            for w in window:
                print(f"   {w.address:08X} {w.mnemonic} {w.op_str}")


if __name__ == "__main__":
    main()
