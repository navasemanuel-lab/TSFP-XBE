"""The preview folder put beside TSFP.exe in a release: from the packer's own November 2004 preview of Future Perfect
for PS2 (its .iso), only what the early Breaking and Entering skyline reads (game/beta_skyline.cpp): the four
buildings' models and their textures, in a pak as the preview's (PAK/STORY/L_6_ST.PAK, "P5CK").
    py tools/preview_pack.py <preview .iso> <out folder>"""
import os
import struct
import sys
import zlib

PAKS = ["PAK/STORY/L_6_ST.PAK", "PAK/STORY/L_6_STM.PAK"]
BUILDINGS = ["buildingnorth", "buildingsouth", "buildingeast", "buildingwest"]


def iso_file(iso, name):
    def sectors(lba, n):
        iso.seek(lba * 2048)
        return iso.read(n)
    pvd = sectors(16, 2048)
    assert pvd[1:6] == b"CD001", "not an ISO 9660 image"
    lba, size = struct.unpack_from("<I", pvd, 156 + 2)[0], struct.unpack_from("<I", pvd, 156 + 10)[0]
    for part in name.split("/"):
        d = sectors(lba, size)
        p, found = 0, False
        while p < size:
            n = d[p]
            if not n:
                p = (p // 2048 + 1) * 2048
                continue
            entry = d[p + 33:p + 33 + d[p + 32]].decode("latin-1").split(";")[0]
            if entry.upper() == part.upper():
                lba, size = struct.unpack_from("<I", d, p + 2)[0], struct.unpack_from("<I", d, p + 10)[0]
                found = True
                break
            p += n
        if not found:
            sys.exit("no %s in the image" % name)
    return sectors(lba, size)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    files = {}
    with open(sys.argv[1], "rb") as iso:
        for pak in PAKS:
            d = iso_file(iso, pak)
            assert d[:4] == b"P5CK"
            at, n = struct.unpack_from("<2I", d, 4)
            for e in range(at, at + n, 16):
                crc, off, size = struct.unpack_from("<3I", d, e)
                files.setdefault(crc, d[off:off + size])
    out = {}
    for b in BUILDINGS:
        path = "assets/ts/models/nearfuture1/%s/ps2/output/%s.raw" % (b, b)
        raw = files.get(zlib.crc32(path.encode()))
        if raw is None:
            sys.exit("no %s" % path)
        out[path] = raw
        p = struct.unpack_from("<I", raw, 0)[0]
        while p + 16 <= len(raw) and struct.unpack_from("<I", raw, p)[0] != 0xFFFFFFFF:
            tex = "textures/%06x.raw" % struct.unpack_from("<I", raw, p)[0]
            if zlib.crc32(tex.encode()) not in files:
                sys.exit("no %s" % tex)
            out[tex] = files[zlib.crc32(tex.encode())]
            p += 16
    body, table = b"", b""
    base = 16
    for path, data in sorted(out.items()):
        table += struct.pack("<4I", zlib.crc32(path.encode()), base + len(body), len(data), 0)
        body += data + b"\0" * (-len(data) % 16)
    folder = os.path.join(sys.argv[2], "PAK", "STORY")
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, "L_6_ST.PAK"), "wb") as f:
        f.write(b"P5CK" + struct.pack("<3I", base + len(body), len(table), 0) + body + table)
    print("%d files, %d bytes" % (len(out), sum(map(len, out.values()))))


main()
