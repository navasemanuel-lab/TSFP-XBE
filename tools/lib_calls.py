"""Direct calls from outside a library section into it whose target the config does not name.
Those run the library's original code, which breaks when the host owns the library's objects.

python -I tools/lib_calls.py assets/default.xbe config/game.toml DSOUND
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
    text = open(sys.argv[2]).read()
    syms = {int(v, 16): k for k, v in re.findall(r"^(\w+)\s*=\s*(0x[0-9A-Fa-f]+)", text, re.M)}
    lib = next(s for s in x.sections if s[0] == sys.argv[3])
    lo, hi = lib[1], lib[1] + lib[2]
    callers = defaultdict(list)
    for s in x.code_sections():
        if s[0] == lib[0]:
            continue
        data = x.raw[s[3]:s[3] + s[4]]
        for i in range(len(data) - 5):
            if data[i] in (0xE8, 0xE9):
                t = s[1] + i + 5 + struct.unpack_from("<i", data, i + 1)[0]
                if lo <= t < hi:
                    callers[t].append(s[1] + i)
    for t in sorted(callers):
        # follow a thunk's jmp
        final = t
        b = x.read(t, 5)
        if b[0] == 0xE9:
            final = t + 5 + struct.unpack_from("<i", b, 1)[0]
        name = syms.get(t) or syms.get(final)
        mark = "  " if name else "!!"
        sites = " ".join(f"{c:08X}" for c in callers[t][:4])
        extra = f" -> {final:08X}" if final != t else ""
        print(f"{mark} {t:08X}{extra} {name or '??':<40} x{len(callers[t])} {sites}")


if __name__ == "__main__":
    main()
