"""The ts2 folder put beside TSFP.exe in a release: from the packer's own TimeSplitters 2 for Xbox, only what the early
HUD and font read (game/ts2_style.cpp, game/hud_ammo.cpp): the font's glyphs as glyphs.bin and its texture with the
HUD's sprites in data/hud.pak, a pak as TimeSplitters 2's ("P8CK").
    py tools/ts2_pack.py <TimeSplitters 2 folder> <Future Perfect default.xbe> <out folder>"""
import os
import struct
import sys

FONT_NAME = b"host0:textures/misc/impact.raw\0"
FONT_FILE = "textures/misc/impact.xbt"
GLYPHS = 101
SPRITES, SPRITE_SIZE = 0x4D2D58, 0x18          # Future Perfect's sprite table: {flags, name, 0, texture, ...}
WANTED = list(range(142, 179)) + [274]         # hud_ammo.cpp: kFirstSprite..kLastSprite and kCircle


class Xbe:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        d = self.d
        assert d[:4] == b"XBEH", path
        base, count, headers = struct.unpack_from("<I", d, 0x104)[0], *struct.unpack_from("<I", d, 0x11C), \
            struct.unpack_from("<I", d, 0x120)[0]
        headers -= base
        self.sections = []
        for i in range(min(count, 64)):
            va, _, raw, size = struct.unpack_from("<4I", d, headers + i * 56 + 4)
            self.sections.append((va, raw, size))

    def offset(self, va):
        for s_va, raw, size in self.sections:
            if s_va <= va < s_va + size and raw + size <= len(self.d):
                return raw + va - s_va
        return 0

    def address(self, off):
        for va, raw, size in self.sections:
            if raw <= off < raw + size:
                return va + off - raw
        return 0

    def u32(self, off):
        return struct.unpack_from("<I", self.d, off)[0]

    def cstring(self, va):
        o = self.offset(va)
        return self.d[o:self.d.index(b"\0", o)].decode() if o else ""


def ts2_glyphs(xbe):
    at = xbe.d.find(FONT_NAME)
    name = xbe.address(at) if at >= 0 else 0
    if not name:
        sys.exit("no TimeSplitters 2 font record in its default.xbe")
    for i in range(0, len(xbe.d) - 12, 4):
        if xbe.u32(i) != name:
            continue
        g = xbe.offset(xbe.u32(i + 8))
        if not g or g + GLYPHS * 10 > len(xbe.d):
            continue
        out = xbe.d[g:g + GLYPHS * 10]
        rects = [struct.unpack_from("<5H", out, k * 10) for k in range(GLYPHS)]
        if all(r[0] <= r[2] <= 256 and r[1] <= r[3] <= 256 for r in rects):
            return out
    sys.exit("TimeSplitters 2's glyphs not found")


def ts2_files(data, wanted):
    out = {}
    for root, _, files in os.walk(data):
        for f in sorted(files):
            if not f.lower().endswith(".pak"):
                continue
            d = open(os.path.join(root, f), "rb").read()
            if d[:4] != b"P8CK":
                continue
            directory, count, names = struct.unpack_from("<3I", d, 4)
            for i in range(count):
                n, size, off = struct.unpack_from("<3I", d, directory + i * 12)
                name = d[names + n:d.index(b"\0", names + n)].decode("latin-1")
                if name in wanted and name not in out:
                    out[name] = d[off:off + size]
    return out


def write_pak(path, files):
    names = b""
    entries = []
    for name, data in files.items():
        entries.append((len(names), data))
        names += name.encode() + b"\0"
    names += b"\0" * (-len(names) % 4)
    names_at = 16
    directory = names_at + len(names)
    body_at = directory + 12 * len(entries)
    table, body = b"", b""
    for n, data in entries:
        table += struct.pack("<3I", n, len(data), body_at + len(body))
        body += data + b"\0" * (-len(data) % 16)
    with open(path, "wb") as f:
        f.write(b"P8CK" + struct.pack("<3I", directory, len(entries), names_at) + names + table + body)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    ts2, fp, out = sys.argv[1:]
    fp = Xbe(fp)
    wanted = {FONT_FILE}
    for s in WANTED:
        name = fp.cstring(fp.u32(fp.offset(SPRITES + s * SPRITE_SIZE + 4)))
        if name:
            wanted.add("textures/misc/%s.xbt" % name)
    files = ts2_files(os.path.join(ts2, "data"), wanted)
    if FONT_FILE not in files:
        sys.exit("no %s in TimeSplitters 2's paks" % FONT_FILE)
    os.makedirs(os.path.join(out, "data"), exist_ok=True)
    open(os.path.join(out, "glyphs.bin"), "wb").write(ts2_glyphs(Xbe(os.path.join(ts2, "default.xbe"))))
    write_pak(os.path.join(out, "data", "hud.pak"), dict(sorted(files.items())))
    print("%d of %d files, %d bytes" % (len(files), len(wanted), sum(map(len, files.values()))))


main()
