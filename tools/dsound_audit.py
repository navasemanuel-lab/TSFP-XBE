"""Lists the DSOUND library's API entry points (they all take the library lock first) next to the config's names.

python -I tools/dsound_audit.py assets/default.xbe config/game.toml
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xbe_re import Xbe  # noqa: E402

LOCK = 0x4069FE  # the library's lock helper in this image


def main():
    x = Xbe(sys.argv[1])
    text = open(sys.argv[2]).read()
    syms = {int(v, 16): k for k, v in re.findall(r"^(\w+)\s*=\s*(0x[0-9A-Fa-f]+)", text, re.M)}
    s = next(s for s in x.sections if s[0] == "DSOUND")
    data = x.raw[s[3]:s[3] + s[4]]
    for i in range(4, len(data) - 5):
        if data[i] != 0xE8 or s[1] + i + 5 + struct.unpack_from("<i", data, i + 1)[0] != LOCK:
            continue
        va = s[1] + i
        start = va - 1 if data[i - 1] == 0x56 else va
        if data[i - 4:i - 1] == bytes([0x55, 0x8B, 0xEC]):
            start = va - 4
        ret = ""
        for ins in x.md.disasm(x.read(va, 600), va):
            if ins.mnemonic == "ret":
                ret = ins.op_str
                break
        print(f"{start:08X} ret {ret:<6} {syms.get(start, '??')}")


if __name__ == "__main__":
    main()
