"""Functions ranked by SSE packed math, to find vector kernels such as skinning.

python -I tools/simd_funcs.py assets/default.xbe generated/game/x86_func_mapping.cpp
"""
import bisect
import os
import re
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xbe_re import Xbe  # noqa: E402

PACKED = {"mulps", "addps", "subps", "shufps", "movaps", "movups", "movhlps", "movlhps", "unpcklps", "unpckhps",
          "rsqrtps", "rcpps", "maxps", "minps", "cvtdq2ps", "cvtps2dq", "andps", "orps", "xorps"}


def main():
    x = Xbe(sys.argv[1])
    x.md.skipdata = True
    funcs = sorted(int(m, 16) for m in re.findall(r"\{0x([0-9A-Fa-f]+)u,", open(sys.argv[2]).read()))
    s = next(s for s in x.sections if s[0] == ".text")
    counts = defaultdict(Counter)
    for ins in x.md.disasm(x.raw[s[3]:s[3] + s[4]], s[1]):
        if ins.mnemonic in PACKED:
            f = funcs[bisect.bisect_right(funcs, ins.address) - 1]
            counts[f][ins.mnemonic] += 1
    ranked = sorted(counts.items(), key=lambda kv: -sum(kv[1].values()))
    for f, c in ranked[:25]:
        print(f"{f:08X} {sum(c.values()):4} " + " ".join(f"{k}:{v}" for k, v in c.most_common(8)))


if __name__ == "__main__":
    main()
