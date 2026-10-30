"""Counts NV2A push buffer method headers written as immediates in code (mov [reg+disp], imm32 / mov reg, imm32),
by method, to see which hardware commands game code pushes itself.

python -I tools/nv2a_scan.py assets/default.xbe [section ...]
"""
import os
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xbe_re import Xbe  # noqa: E402

import capstone  # noqa: E402

NAMES = {
    0x17FC: "BEGIN_END", 0x1800: "ARRAY_ELEMENT16", 0x1808: "ARRAY_ELEMENT32", 0x1810: "DRAW_ARRAYS",
    0x1818: "INLINE_ARRAY", 0x1720: "VERTEX_DATA_ARRAY_OFFSET", 0x1760: "VERTEX_DATA_ARRAY_FORMAT",
    0x1B00: "TEXTURE_OFFSET", 0x1B04: "TEXTURE_FORMAT", 0x0B00: "TRANSFORM_PROGRAM", 0x0B80: "TRANSFORM_CONSTANT",
    0x1E9C: "PROGRAM_LOAD", 0x1EA0: "PROGRAM_START", 0x1EA4: "CONSTANT_LOAD", 0x0260: "COMBINER_ALPHA_ICW",
    0x0A60: "COMBINER_FACTOR0", 0x0288: "COMBINER_SPECULAR_FOG_CW0", 0x1E60: "COMBINER_CONTROL",
    0x0480: "MODELVIEW_MATRIX", 0x0680: "COMPOSITE_MATRIX", 0x0A00: "VIEWPORT_OFFSET", 0x0AF0: "VIEWPORT_SCALE",
    0x1D94: "CLEAR_SURFACE", 0x1E94: "SHADER_STAGE_PROGRAM", 0x1D6C: "SEMAPHORE_OFFSET", 0x0100: "NOP",
    0x1E40: "CONSTANT_LOAD?", 0x1E70: "SHADER_OTHER_STAGE_INPUT", 0x1E78: "SHADER_CLIP_PLANE_MODE",
}


def name(m):
    if m in NAMES:
        return NAMES[m]
    for base, n, size in ((0x1720, "VERTEX_DATA_ARRAY_OFFSET", 0x40), (0x1760, "VERTEX_DATA_ARRAY_FORMAT", 0x40),
                          (0x1B00, "TEXTURE_*", 0x100), (0x0B80, "TRANSFORM_CONSTANT", 0x80),
                          (0x0B00, "TRANSFORM_PROGRAM", 0x80), (0x0260, "COMBINER_*", 0x40),
                          (0x0A60, "COMBINER_FACTOR", 0x40), (0x0AC0, "COMBINER_*", 0x40)):
        if base <= m < base + size:
            return n
    return ""


def main():
    x = Xbe(sys.argv[1])
    want = set(sys.argv[2:]) or {".text"}
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    counts = Counter()
    where = defaultdict(list)
    for s in x.sections:
        if s[0] not in want:
            continue
        data = x.raw[s[3]:s[3] + s[4]]
        for ins in md.disasm(data, s[1]):
            if ins.mnemonic != "mov" or len(ins.operands) != 2:
                continue
            op = ins.operands[1]
            if op.type != capstone.x86.X86_OP_IMM:
                continue
            v = op.imm & 0xFFFFFFFF
            if v & 0xA0030003 or v < 0x40000:
                continue
            n = (v >> 18) & 0x7FF
            m = v & 0x1FFC
            if not 1 <= n <= 128 or m < 0x100:
                continue
            counts[m] += 1
            if len(where[m]) < 3:
                where[m].append(ins.address)
    for m, c in counts.most_common(60):
        print(f"{m:04X} {name(m):<26} x{c:<4} {' '.join(f'{a:08X}' for a in where[m])}")


if __name__ == "__main__":
    main()
