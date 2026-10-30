"""TimeSplitters: Future Perfect (Xbox) models (.xbr) to glTF 2.0 (.glb) and back, for editing them in Blender.

python -I tools/tsfp_model.py list     <game>  [text]
python -I tools/tsfp_model.py gltf     <game>  <model> <out.glb> [--bindpose <name>]
python -I tools/tsfp_model.py fromgltf <game>  <model> <edited.glb> <mod folder> [--bindpose <name>]
python -I tools/tsfp_model.py extract  <game>  <model> <folder>

<game> is the folder of TSFP.exe (with assets\\ holding default.xbe and pak\\) or the assets folder itself. <model> is
a model's name as the game has it (chrs/cortezarmouredingame, guns/pistol9) or any unique part of it. The mod folder
is mods\\<mod> beside the game: the edited model goes to its files\\ at the model's path in the paks, new textures to
files\\textures\\<id>.xbt, and mod.toml is written when there is none.

A model (as 0x8DF60 fixes it up): {texture table offset, header offset, material table offset} at 0; the texture
table 16 bytes an entry {id, id, 0, flags} to one of 0xFFFFFFFF (flags 0x10000000: the texture is in the file at the
first word); the header {nodes, matrices, 1, flags, ...} right after the nodes, 0xC8 bytes each; the material table
0x30 bytes an entry, the first word the texture table index. A node: three passes of {render state, positions,
UVs, normals, colours, -} at +0x18 each, its kind and links at +0x48 {kind, its matrix, previous node, next node,
-, other node}, the passes' chunk tables at +0x60 and strip tables at +0xBC, statistics at +0x70 {indices,
triangles}, hit volumes at +0x90 and +0xAC. Kind 0 is a bone's rigid piece (its own matrix), 1 a piece between two
bones (theirs), 2 a joint (the two bones' and its own, made between them as they move), 3 a joint with a third.
A pass's vertices: positions {x, y, z, 0xFFFFFF00 | 0xE0 + 4 * the palette entry}, UVs {u, v, 1}, normals {11:11:10,
the matrix}, colours BGRA (0x7F full). Its chunk table {material, vertices, kind, index, first vertex, 0xFFFF} 10
bytes a chunk to {0, 0, 0xFF}; runs of chunks share one index list, on the run's last chunk in the strip table
{list, count}: a triangle strip over the pass's vertices, strips joined by repeated indices.
Positions are in the space of their matrix. A skeleton's rest pose is in the animation pak skelts3.pak
(anim/data/ts3/*.raw, named in default.xbe): {-1, -1, flags, 1, 1, bones, ...}, 0x20 bytes a bone from 0x24, then a bone
each: translation (3 floats) and rotation (a quaternion, 4 u16 (v / 32767.5 - 1), or 3 with flags 0x10 and w made
positive), relative to the parent bone; the bones are the kind 0 nodes in order, each node's parent the first kind 0
node back along its previous-node links. A joint's matrix sits at the next bone, its rotation between the two bones'
(node +0xB8).
Textures (textures/<id>.xbt): {w, h, w, h, levels - 1, format, -1} then the levels from 0x80; formats 0 DXT1,
1 DXT3, 2 A8R8G8B8, 3 R5G6B5, 4 DXT5, 5 A8, 6 L8, 7 A8L8 (0x183D4), all but DXT swizzled.

Export: one glTF joint per matrix (bones b00_..., joints j05_...), one mesh per node and pass ("n04_p0_chest"), each
vertex on its one matrix, a material per material table entry ("m01_t0f4205") with its texture as PNG.
Import: the base model stays as it is but for the geometry of the passes the glTF has, which is written again
after the file's end (the old arrays left unused). A mesh named after a node and pass goes there; any other mesh is
cut by triangle to the node of the matrix most of its vertices follow (joint matrices to their joint node). Each
vertex follows the matrix of its node's palette it is weighted most to. A material named after one of the model's
("m01_t0f4205", "m00_plain" untextured) uses it (its picture changed: a new texture); any other gets a new texture from its picture and a new material
like the node's own. When any mesh is named after a piece, a piece the glTF lacks is removed; with none (a model
made anew) the pieces no mesh went to stay as they are.

Python 3, standard library only.
"""

import argparse
import base64
import json
import math
import os
import re
import struct
import sys
import zlib

MODEL_PATH = "assets/ts/models/%s/xbox/output/%s.xbr"
TEXTURE_PATH = "textures/%06x.xbt"
NODE = 0xC8
CHUNK_VERTS = 40
# guns whose rest pose's name is not theirs
GUN_POSES = {"brick_anim": "brick_boned_b", "dartgun_anim": "dartgun_offset_b",
             "doublebarrelshotgun_anim": "doubleshotgun_b", "electrogun_anim": "electrotool_b",
             "fsniper_anim": "futuresniper_bindpose_b", "futureshotgun_anim": "future_shotgun_b",
             "harpoon_anim": "harpoongun_b",
             "homingrocketlauncher_anim": "homing_launcher_b", "magnum_anim": "gun_magnum_b",
             "plasmamachinegun_anim": "plasmaauto_b", "rocketlauncher_anim": "heatseeker_b",
             "scifihandgun_anim": "plasma_pistol_b", "sniperbolt_anim": "rifle_b", "tnt_anim": "bomb_b",
             "timedmine_anim": "remotemine_b", "proxmine_anim": "remotemine_b", "mapboy_anim": "uplinkhighpol_b",
             "assaultshotgun_anim": "pumpaction_b"}
HUMAN19 = ["pelvis", "spine", "chest", "neck", "head", "r_clavicle", "r_upperarm", "r_forearm", "r_hand",
           "l_clavicle", "l_upperarm", "l_forearm", "l_hand", "r_thigh", "r_shin", "r_foot", "l_thigh", "l_shin",
           "l_foot"]


class ModelError(Exception):
    pass


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def f32(b, o):
    return struct.unpack_from("<f", b, o)[0]


# ----------------------------------------------------------------------------------------------------------------
# The game's files


class Game:
    """The paks of a game folder by path hash, and the model names default.xbe and the paks give."""

    def __init__(self, folder):
        folder = os.path.abspath(folder)
        if os.path.isdir(os.path.join(folder, "assets", "pak")):
            folder = os.path.join(folder, "assets")
        if not os.path.isdir(os.path.join(folder, "pak")):
            raise ModelError("no pak folder in %s: give the game's folder (TSFP.exe) or its assets folder" % folder)
        self.root = folder
        self.files = {}
        self._data = {}
        for dp, _, fn in os.walk(os.path.join(folder, "pak")):
            for f in sorted(fn):
                p = os.path.join(dp, f)
                with open(p, "rb") as fh:
                    h = fh.read(12)
                    if len(h) < 12 or h[:4] != b"P5CK":
                        continue
                    off, size = struct.unpack_from("<II", h, 4)
                    fh.seek(off)
                    table = fh.read(size)
                for i in range(0, len(table) - 15, 16):
                    hv, o, s = struct.unpack_from("<III", table, i)
                    self.files.setdefault(hv, (p, o, s))
        self._names = None

    def has(self, path):
        return zlib.crc32(path.encode()) in self.files

    def read(self, path):
        e = self.files.get(zlib.crc32(path.encode()))
        if not e:
            return None
        p, o, s = e
        with open(p, "rb") as fh:
            fh.seek(o)
            return fh.read(s)

    def _strings(self, xbe_only=False):
        xbe = os.path.join(self.root, "default.xbe")
        sources = [xbe] if os.path.isfile(xbe) else []
        if not xbe_only:
            for dp, _, fn in os.walk(os.path.join(self.root, "pak")):
                sources += [os.path.join(dp, f) for f in fn if not f.startswith(("music", "sounds"))]
        found = set()
        for p in sources:
            with open(p, "rb") as fh:
                d = fh.read()
            for m in re.finditer(rb"[A-Za-z0-9_]+(?:/[A-Za-z0-9_]+)+(?:\.raw)?", d):
                found.add(m.group().decode())
        return found

    def _cache(self):
        """The model list's file, kept between runs (finding the names reads every pak): its key the paks' sizes."""
        key = zlib.crc32(repr(sorted((os.path.relpath(p, self.root), os.path.getsize(p))
                                     for p, _, _ in set(self.files.values()))).encode())
        folder = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~")
        return os.path.join(folder, "tsfp_model", "models_%08x.json" % key)

    def models(self):
        """Every model whose name is written somewhere: {name: path}."""
        if self._names is None:
            cache = self._cache()
            try:
                with open(cache, encoding="utf-8") as f:
                    self._names = json.load(f)
                return self._names
            except (OSError, ValueError):
                pass
            out = {}
            for s in self._strings():
                parts = s.lower().split("/")
                for k in range(len(parts)):
                    name = "/".join(parts[k:])
                    path = MODEL_PATH % (name, parts[-1])
                    if zlib.crc32(path.encode()) in self.files:
                        out[name] = path
            self._names = dict(sorted(out.items()))
            try:
                os.makedirs(os.path.dirname(cache), exist_ok=True)
                with open(cache, "w", encoding="utf-8") as f:
                    json.dump(self._names, f, indent=0)
            except OSError:
                pass
        return self._names

    def bindposes(self):
        """The skeletons' rest poses: {name: path} (anim/data/ts3/human_19_bindpose.raw is human_19_bindpose)."""
        if getattr(self, "_poses", None) is not None:
            return self._poses
        out = {}
        for s in self._strings(xbe_only=True):
            if s.startswith("anim/data/") and s.endswith(".raw") and self.has(s):
                out[os.path.splitext(os.path.basename(s))[0]] = s
        self._poses = dict(sorted(out.items()))
        return self._poses

    def find(self, model):
        """A model's name and path from its name or a unique part of it."""
        model = model.replace("\\", "/").lower()
        m = re.match(r"assets/ts/models/(.+)/xbox/output/[^/]+\.xbr$", model)
        if m:
            model = m.group(1)
        path = MODEL_PATH % (model, model.split("/")[-1])
        if "/" in model and self.has(path):
            return model, path
        names = self.models()
        if model in names:
            return model, names[model]
        hits = [n for n in names if n.endswith("/" + model) or n.split("/")[-1] == model]
        if len(hits) != 1:
            hits = [n for n in names if model in n]
        if len(hits) == 1:
            return hits[0], names[hits[0]]
        if not hits:
            raise ModelError("no model named %s (list shows them)" % model)
        raise ModelError("%s names %d models: %s" % (model, len(hits), ", ".join(hits[:12])))


# ----------------------------------------------------------------------------------------------------------------
# Textures

XBT_FORMATS = {0: "DXT1", 1: "DXT3", 2: "A8R8G8B8", 3: "R5G6B5", 4: "DXT5", 5: "A8", 6: "L8", 7: "A8L8"}
_BPP = {2: 4, 3: 2, 5: 1, 6: 1, 7: 2}


def _swizzle_index(x, y, w, h):
    s = bit = k = 0
    while (1 << k) < w or (1 << k) < h:
        if (1 << k) < w:
            s |= ((x >> k) & 1) << bit
            bit += 1
        if (1 << k) < h:
            s |= ((y >> k) & 1) << bit
            bit += 1
        k += 1
    return s


def _swizzle_table(w, h):
    xs = [0] * w
    ys = [0] * h
    bit = k = 0
    while (1 << k) < w or (1 << k) < h:
        if (1 << k) < w:
            for x in range(w):
                xs[x] |= ((x >> k) & 1) << bit
            bit += 1
        if (1 << k) < h:
            for y in range(h):
                ys[y] |= ((y >> k) & 1) << bit
            bit += 1
        k += 1
    return xs, ys


def _rgb565(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def _dxt_colours(d, o, four):
    c0, c1 = struct.unpack_from("<HH", d, o)
    a, b = _rgb565(c0), _rgb565(c1)
    if four or c0 > c1:
        return [a + (255,), b + (255,), tuple((2 * a[i] + b[i]) // 3 for i in range(3)) + (255,),
                tuple((a[i] + 2 * b[i]) // 3 for i in range(3)) + (255,)]
    return [a + (255,), b + (255,), tuple((a[i] + b[i]) // 2 for i in range(3)) + (255,), (0, 0, 0, 0)]


def xbt_size(w, h, levels, fmt):
    size = 0
    for _ in range(levels):
        if fmt in (0, 1, 4):
            size += max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * (8 if fmt == 0 else 16)
        else:
            size += w * h * _BPP.get(fmt, 4)
        w, h = max(1, w // 2), max(1, h // 2)
    return size


def decode_xbt(d, at=0):
    """An .xbt's first level as (w, h, RGBA bytes)."""
    w, h, _, _, mips, fmt = struct.unpack_from("<6I", d, at)
    if not (0 < w <= 4096 and 0 < h <= 4096) or fmt > 7:
        raise ModelError("not a texture (%dx%d format %d)" % (w, h, fmt))
    o = at + 0x80
    out = bytearray(w * h * 4)
    if fmt in (0, 1, 4):
        bw, bh = (w + 3) // 4, (h + 3) // 4
        step = 8 if fmt == 0 else 16
        for by in range(bh):
            for bx in range(bw):
                p = o + (by * bw + bx) * step
                alpha = None
                if fmt == 1:
                    bits = int.from_bytes(d[p:p + 8], "little")
                    alpha = [((bits >> (4 * i)) & 15) * 17 for i in range(16)]
                    p += 8
                elif fmt == 4:
                    a0, a1 = d[p], d[p + 1]
                    bits = int.from_bytes(d[p + 2:p + 8], "little")
                    if a0 > a1:
                        pal = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
                    else:
                        pal = [a0, a1] + [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]
                    alpha = [pal[(bits >> (3 * i)) & 7] for i in range(16)]
                    p += 8
                cols = _dxt_colours(d, p, fmt != 0)
                idx = u32(d, p + 4)
                for i in range(16):
                    x, y = bx * 4 + (i & 3), by * 4 + (i >> 2)
                    if x >= w or y >= h:
                        continue
                    c = cols[(idx >> (2 * i)) & 3]
                    q = (y * w + x) * 4
                    out[q:q + 4] = bytes((c[0], c[1], c[2], alpha[i] if alpha else c[3]))
        return w, h, bytes(out)
    xs, ys = _swizzle_table(w, h)
    bpp = _BPP[fmt]
    for y in range(h):
        for x in range(w):
            p = o + (xs[x] | ys[y]) * bpp
            q = (y * w + x) * 4
            if fmt == 2:
                out[q:q + 4] = bytes((d[p + 2], d[p + 1], d[p], d[p + 3]))
            elif fmt == 3:
                out[q:q + 4] = bytes(_rgb565(struct.unpack_from("<H", d, p)[0]) + (255,))
            elif fmt == 5:
                out[q:q + 4] = bytes((255, 255, 255, d[p]))
            elif fmt == 6:
                out[q:q + 4] = bytes((d[p], d[p], d[p], 255))
            else:
                out[q:q + 4] = bytes((d[p], d[p], d[p], d[p + 1]))
    return w, h, bytes(out)


def encode_xbt(w, h, rgba):
    """RGBA as an A8R8G8B8 .xbt (format 2), every level to 4x4."""
    if w & (w - 1) or h & (h - 1) or not 4 <= w <= 2048 or not 4 <= h <= 2048:
        raise ModelError("a texture's sides must be powers of two from 4 to 2048 (this one is %dx%d)" % (w, h))
    levels = []
    cw, ch, cur = w, h, rgba
    while True:
        levels.append((cw, ch, cur))
        if cw <= 4 or ch <= 4:
            break
        nw, nh = cw // 2, ch // 2
        nxt = bytearray(nw * nh * 4)
        for y in range(nh):
            r0, r1 = (2 * y) * cw * 4, (2 * y + 1) * cw * 4
            for x in range(nw):
                a, b = r0 + 8 * x, r1 + 8 * x
                q = (y * nw + x) * 4
                for c in range(4):
                    nxt[q + c] = (cur[a + c] + cur[a + 4 + c] + cur[b + c] + cur[b + 4 + c] + 2) // 4
        cw, ch, cur = nw, nh, bytes(nxt)
    out = bytearray(struct.pack("<7I", w, h, w, h, len(levels) - 1, 2, 0xFFFFFFFF))
    out += bytes(0x80 - len(out))
    for lw, lh, px in levels:
        xs, ys = _swizzle_table(lw, lh)
        lv = bytearray(lw * lh * 4)
        for y in range(lh):
            for x in range(lw):
                q = (y * lw + x) * 4
                p = (xs[x] | ys[y]) * 4
                lv[p:p + 4] = bytes((px[q + 2], px[q + 1], px[q], px[q + 3]))
        out += lv
    return bytes(out)


def png_encode(w, h, rgba):
    raw = b"".join(b"\0" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def png_decode(d):
    """A PNG as (w, h, RGBA): 8 or 16 bits, grey, RGB, palette, grey + alpha or RGBA, not interlaced."""
    if d[:8] != b"\x89PNG\r\n\x1a\n":
        raise ModelError("a texture picture is not a PNG (save the image as PNG in Blender)")
    o, idat, pal, trns = 8, b"", None, None
    while o < len(d):
        n, t = struct.unpack_from(">I4s", d, o)
        c = d[o + 8:o + 8 + n]
        if t == b"IHDR":
            w, h, depth, ctype, _, _, inter = struct.unpack(">IIBBBBB", c)
        elif t == b"PLTE":
            pal = c
        elif t == b"tRNS":
            trns = c
        elif t == b"IDAT":
            idat += c
        o += 12 + n
    if inter:
        raise ModelError("interlaced PNGs are not read: save it again without interlacing")
    chans = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    if depth not in (8, 16) and not (ctype == 3 and depth in (1, 2, 4)):
        raise ModelError("a PNG of %d bits a channel is not read" % depth)
    bits = chans * depth
    stride = (w * bits + 7) // 8
    bpp = max(1, bits // 8)
    raw = zlib.decompress(idat)
    rows, prev = [], bytearray(stride)
    p = 0
    for _ in range(h):
        f = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    out = bytearray(w * h * 4)
    for y, line in enumerate(rows):
        for x in range(w):
            if ctype == 3:
                per = 8 // depth if depth < 8 else 1
                if depth < 8:
                    v = (line[x // per] >> (8 - depth * (x % per + 1))) & ((1 << depth) - 1)
                else:
                    v = line[x]
                r, g, b = pal[3 * v:3 * v + 3]
                a = trns[v] if trns and v < len(trns) else 255
                px = (r, g, b, a)
            else:
                s = depth // 8
                vals = [line[(x * chans + k) * s] for k in range(chans)]
                if ctype == 0:
                    px = (vals[0],) * 3 + (255,)
                elif ctype == 2:
                    px = tuple(vals) + (255,)
                elif ctype == 4:
                    px = (vals[0],) * 3 + (vals[1],)
                else:
                    px = tuple(vals)
            out[(y * w + x) * 4:(y * w + x) * 4 + 4] = bytes(px)
    return w, h, bytes(out)


# ----------------------------------------------------------------------------------------------------------------
# Small maths: 4x4 row-major nested lists, quaternions (x, y, z, w)


def ident():
    return [[float(i == j) for j in range(4)] for i in range(4)]


def mmul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def qmat(q, t=(0, 0, 0)):
    x, y, z, w = q
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), t[0]],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), t[1]],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), t[2]],
            [0.0, 0.0, 0.0, 1.0]]


def mat_quat(m):
    t = m[0][0] + m[1][1] + m[2][2]
    if t > 0:
        s = math.sqrt(t + 1) * 2
        q = [(m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s, s / 4]
    else:
        i = max(range(3), key=lambda k: m[k][k])
        j, k = (i + 1) % 3, (i + 2) % 3
        s = math.sqrt(max(1e-12, 1 + m[i][i] - m[j][j] - m[k][k])) * 2
        q = [0.0] * 4
        q[i] = s / 4
        q[j] = (m[j][i] + m[i][j]) / s
        q[k] = (m[k][i] + m[i][k]) / s
        q[3] = (m[k][j] - m[j][k]) / s
    n = math.sqrt(sum(c * c for c in q)) or 1
    return [c / n for c in q]


def slerp(a, b, t):
    d = sum(x * y for x, y in zip(a, b))
    if d < 0:
        b, d = [-c for c in b], -d
    if d > 0.9995:
        r = [x + (y - x) * t for x, y in zip(a, b)]
    else:
        th = math.acos(min(1, d))
        s1, s2 = math.sin((1 - t) * th), math.sin(t * th)
        r = [(s1 * x + s2 * y) / math.sin(th) for x, y in zip(a, b)]
    n = math.sqrt(sum(c * c for c in r)) or 1
    return [c / n for c in r]


def rigid_inverse(m):
    r = [[m[j][i] for j in range(3)] for i in range(3)]
    t = [-sum(r[i][k] * m[k][3] for k in range(3)) for i in range(3)]
    return [r[0] + [t[0]], r[1] + [t[1]], r[2] + [t[2]], [0.0, 0.0, 0.0, 1.0]]


def xform(m, p, w=1.0):
    return tuple(m[i][0] * p[0] + m[i][1] * p[1] + m[i][2] * p[2] + m[i][3] * w for i in range(3))


def normalize(v):
    n = math.sqrt(sum(c * c for c in v))
    return tuple(c / n for c in v) if n > 1e-12 else (0.0, 1.0, 0.0)


def colmajor(m):
    return [m[r][c] for c in range(4) for r in range(4)]


def from_colmajor(v):
    return [[v[c * 4 + r] for c in range(4)] for r in range(4)]


# ----------------------------------------------------------------------------------------------------------------
# The model


def unpack_normal(n):
    def s(v, bits):
        v &= (1 << bits) - 1
        return v - (1 << bits) if v >> (bits - 1) else v
    return (s(n, 11) / 1023.0, s(n >> 11, 11) / 1023.0, s(n >> 22, 10) / 511.0)


def pack_normal(n):
    def q(v, bits):
        m = (1 << (bits - 1)) - 1
        return int(round(max(-1.0, min(1.0, v)) * m)) & ((1 << bits) - 1)
    return q(n[0], 11) | q(n[1], 11) << 11 | q(n[2], 10) << 22


def strip_triangles(idx):
    out = []
    for k in range(len(idx) - 2):
        a, b, c = idx[k], idx[k + 1], idx[k + 2]
        if a == b or b == c or a == c:
            continue
        out.append((a, b, c) if k % 2 == 0 else (b, a, c))
    return out


class Model:
    def __init__(self, data):
        b = self.data = bytes(data)
        if len(b) < 16:
            raise ModelError("not a model")
        self.tex_at, self.hdr, self.mat_at = u32(b, 0), u32(b, 4), u32(b, 8)
        if self.hdr + 8 > len(b):
            raise ModelError("not a model")
        self.count = u32(b, self.hdr)
        self.matrices = u32(b, self.hdr + 4)
        self.base = self.hdr - self.count * NODE
        if not self.count or self.count > 1000 or self.base < 0:
            raise ModelError("this kind of model (a MapMaker tile or a level) is not handled")
        self.textures = []  # [id, id, word, flags]
        o = self.tex_at
        while o + 16 <= len(b) and u32(b, o) != 0xFFFFFFFF:
            self.textures.append(list(struct.unpack_from("<4I", b, o)))
            o += 16
        self.materials = []  # raw 0x30 bytes
        o = self.mat_at
        # an untextured material starts with -1 too: the end's has -1 at +0x10 as well
        while o + 0x30 <= len(b) and not (u32(b, o) == 0xFFFFFFFF and u32(b, o + 0x10) == 0xFFFFFFFF):
            self.materials.append(b[o:o + 0x30])
            o += 0x30
        self.mat_end = b[o:o + 0x30] if o + 0x30 <= len(b) else bytes([0xFF] * 0x14) + bytes(0x1C)
        self.nodes = [self._node(i) for i in range(self.count)]
        for nd in self.nodes:
            nd["palette"] = self._palette(nd)

    def _node(self, i):
        b = self.data
        a = self.base + i * NODE
        nd = {"i": i, "at": a, "kind": b[a + 0x48], "own": b[a + 0x49], "prev": b[a + 0x4A], "next": b[a + 0x4B],
              "other": b[a + 0x4D], "blend": f32(b, a + 0xB8), "passes": []}
        for p in range(3):
            w = [u32(b, a + 0x18 * p + 4 * k) for k in range(6)]
            chunks_at, strips_at = u32(b, a + 0x60 + 4 * p), u32(b, a + 0xBC + 4 * p)
            if not chunks_at or not w[1]:
                nd["passes"].append(None)
                continue
            chunks = []
            o = chunks_at
            while o + 10 <= len(b):
                grp, n, kind, idx, start = struct.unpack_from("<HBBHH", b, o)
                if n == 0 and kind == 0xFF:
                    break
                chunks.append((grp, n, kind, idx, start))
                o += 10
            nv = sum(c[1] for c in chunks)
            verts = []
            # most models keep each vertex in its matrix's space, tagged 0xFFFFFFE0 + 4 * palette slot; a few
            # (Subway's soldiers) keep the model's space, with a small number there instead
            in_model = nv > 0 and u32(b, w[1] + 12) >> 8 != 0xFFFFFF
            for k in range(nv):
                x, y, z, tag = struct.unpack_from("<3fI", b, w[1] + 16 * k)
                uv = struct.unpack_from("<2f", b, w[2] + 12 * k) if w[2] else (0.0, 0.0)
                n_, m = struct.unpack_from("<II", b, w[3] + 8 * k) if w[3] else (0, 0)
                col = tuple(b[w[4] + 4 * k:w[4] + 4 * k + 4]) if w[4] else None
                verts.append({"p": (x, y, z), "slot": 0 if in_model else ((tag & 0xFF) - 0xE0) // 4, "uv": uv,
                              "n": unpack_normal(n_), "m": m, "col": col})
            tris = []  # (a, b, c, material)
            if strips_at:
                for s in range(len(chunks)):
                    lp, cnt = struct.unpack_from("<II", b, strips_at + 8 * s)
                    if not cnt:
                        continue
                    if lp + 2 * cnt > len(b):
                        raise ModelError("this kind of model is not handled (node %d's strips are outside it)" % i)
                    idx = struct.unpack_from("<%dH" % cnt, b, lp)
                    for t in strip_triangles(idx):
                        if max(t) < nv:
                            tris.append(t + (chunks[s][0],))
            nd["passes"].append({"words": w, "chunks": chunks, "verts": verts, "tris": tris, "in_model": in_model,
                                 "kind": chunks[0][2] if chunks else (3 if nd["kind"] == 0 else 4)})
        return nd

    def _palette(self, nd):
        def own(j):
            return self.nodes_raw_own(j)
        if nd["kind"] == 0:
            return [nd["own"]]
        pal = [own(nd["prev"]), own(nd["next"])]
        if nd["kind"] >= 2:
            pal.append(nd["own"])
        if nd["kind"] >= 3 and nd["other"] < self.count:
            pal.append(own(nd["other"]))
        return pal

    def nodes_raw_own(self, j):
        return self.data[self.base + j * NODE + 0x49] if j < self.count else 0

    def texture_id(self, material):
        if material >= len(self.materials):
            return None
        t = u32(self.materials[material], 0)
        return self.textures[t][0] if t < len(self.textures) else None

    def texture_rgba(self, game, t):
        """A texture table entry's picture, (w, h, RGBA) or None."""
        tid, _, _, flags = self.textures[t]
        try:
            if flags & 0x10000000 and tid < len(self.data):
                return decode_xbt(self.data, tid)
            d = game.read(TEXTURE_PATH % tid)
            return decode_xbt(d) if d else None
        except (ModelError, struct.error, IndexError):
            return None


# ----------------------------------------------------------------------------------------------------------------
# The skeleton


def read_bindpose(d):
    flags, bones = u32(d, 8), u32(d, 0x14)
    at = 0x44 + bones * 0x20
    comps = 3 if flags & 0x10 else 4  # flags 0x10: x, y, z (w positive); 1: u16 each, else floats
    out = []
    for _ in range(bones):
        t = struct.unpack_from("<3f", d, at)
        at += 12
        if flags & 1:
            q = [v / 32767.5 - 1 for v in struct.unpack_from("<%dH" % comps, d, at)]
            at += 2 * comps
        else:
            q = list(struct.unpack_from("<%df" % comps, d, at))
            at += 4 * comps
        if comps == 3:
            q.append(math.sqrt(max(0.0, 1 - sum(c * c for c in q))))
        n = math.sqrt(sum(c * c for c in q)) or 1
        out.append((t, [c / n for c in q]))
    return out


def choose_bindpose(game, model_name, model, wanted=None):
    """(name, rest pose) for the model, or (None, None) when it has no skeleton the game names."""
    poses = game.bindposes()
    bones = sum(1 for nd in model.nodes if nd["kind"] == 0)
    if wanted:
        if wanted not in poses:
            raise ModelError("no rest pose %s (known: %s)" % (wanted, ", ".join(poses)))
        pose = read_bindpose(game.read(poses[wanted]))
        if len(pose) != bones:
            raise ModelError("%s has %d bones, the model %d" % (wanted, len(pose), bones))
        return wanted, pose
    if bones <= 1:
        return None, None
    base = model_name.split("/")[-1]
    stem = re.sub(r"(lod\d*|_cl|_ph)$", "", base)
    cands = []
    if base in GUN_POSES:
        cands.append((4, GUN_POSES[base], poses.get(GUN_POSES[base])))
    for name, path in poses.items():
        key = re.sub(r"(_b|_bindpose|bindpose|_bind)$", "", name.lower())
        score = 3 if key == base else 2 if key == stem else 1 if key and (key in base or base in key) else 0
        if score:
            cands.append((score, name, path))
    # the usual skeletons by their bones: people, first-person arms, people talking in cutscenes
    if bones == 19:
        cands.append((0, "human_19_bindpose", poses.get("human_19_bindpose")))
    elif bones == 20:
        first = "cortfirspershandnouplink_texboned_b" if base.endswith("arms") else "human_talking_bindpose"
        cands += [(0, first, poses.get(first))]
    elif bones == 25:
        cands.append((0, "cortezwithmouth_texboned_b", poses.get("cortezwithmouth_texboned_b")))
    for _, name, path in sorted(cands, key=lambda c: -c[0]):
        if not path:
            continue
        pose = read_bindpose(game.read(path))
        if len(pose) == bones:
            return name, pose
    return None, None


def world_matrices(model, pose):
    """Each matrix's rest transform in the model's space: {matrix id: 4x4}."""
    nodes = model.nodes
    worlds = {}
    bones = [nd for nd in nodes if nd["kind"] == 0]
    for k, nd in enumerate(bones):
        local = qmat(pose[k][1], pose[k][0]) if pose else ident()
        j = nd["prev"]
        while j < len(nodes) and nodes[j]["kind"] != 0:
            j = nodes[j]["prev"]
        parent = worlds.get(nodes[j]["own"]) if j < len(nodes) else None
        worlds[nd["own"]] = mmul(parent, local) if parent else local
    # a joint (kind 2) makes its matrix; a kind 3 node's "own" names one made elsewhere
    for nd in nodes:
        if nd["kind"] == 2 and nd["own"] not in worlds:
            a = worlds.get(model.nodes_raw_own(nd["prev"]), ident())
            b = worlds.get(model.nodes_raw_own(nd["next"]), ident())
            q = slerp(mat_quat(a), mat_quat(b), nd["blend"] if 0 <= nd["blend"] <= 1 else 0.5)
            worlds[nd["own"]] = qmat(q, (b[0][3], b[1][3], b[2][3]))
    for m in range(model.matrices):
        worlds.setdefault(m, ident())
    return worlds


def joint_names(model, pose_name):
    names = {}
    bones = [nd for nd in model.nodes if nd["kind"] == 0]
    human = pose_name and len(bones) == 19 and "human" in pose_name
    for k, nd in enumerate(bones):
        names[nd["own"]] = "b%02d_%s" % (k, HUMAN19[k]) if human else "b%02d" % k
    for nd in model.nodes:
        if nd["kind"] == 2 and nd["own"] not in names:
            child = names.get(model.nodes_raw_own(nd["next"]), "")
            names[nd["own"]] = "j%02d_%s" % (nd["own"], child.split("_", 1)[-1] if child else "joint")
    for m in range(model.matrices):
        names.setdefault(m, "m%02d" % m)
    return names


def node_name(model, nd, p, names):
    return "n%02d_p%d_%s" % (nd["i"], p, names.get(nd["own"], "") if nd["kind"] != 1 else "link")


# ----------------------------------------------------------------------------------------------------------------
# glTF


class Glb:
    def __init__(self):
        self.j = {"asset": {"version": "2.0", "generator": "tsfp_model.py"}, "scene": 0, "scenes": [{"nodes": []}],
                  "nodes": [], "meshes": [], "accessors": [], "bufferViews": [], "buffers": [], "materials": [],
                  "textures": [], "images": [], "samplers": [{"magFilter": 9729, "minFilter": 9987,
                                                              "wrapS": 10497, "wrapT": 10497}], "skins": []}
        self.bin = bytearray()

    def view(self, data, target=None):
        while len(self.bin) % 4:
            self.bin.append(0)
        v = {"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data)}
        if target:
            v["target"] = target
        self.bin += data
        self.j["bufferViews"].append(v)
        return len(self.j["bufferViews"]) - 1

    def accessor(self, values, ctype, typ, target=None, normalized=False, minmax=False):
        comps = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}[typ]
        fmt = {5126: "f", 5125: "I", 5123: "H", 5121: "B"}[ctype]
        flat = [c for v in values for c in (v if comps > 1 else (v,))]
        a = {"bufferView": self.view(struct.pack("<%d%s" % (len(flat), fmt), *flat), target),
             "componentType": ctype, "count": len(values), "type": typ}
        if normalized:
            a["normalized"] = True
        if minmax and values:
            a["min"] = [min(v[k] for v in values) for k in range(comps)]
            a["max"] = [max(v[k] for v in values) for k in range(comps)]
        self.j["accessors"].append(a)
        return len(self.j["accessors"]) - 1

    def write(self, path):
        while len(self.bin) % 4:
            self.bin.append(0)
        self.j["buffers"] = [{"byteLength": len(self.bin)}]
        for k in [k for k, v in self.j.items() if isinstance(v, list) and not v]:
            del self.j[k]
        js = json.dumps(self.j, separators=(",", ":")).encode()
        js += b" " * (-len(js) % 4)
        with open(path, "wb") as f:
            f.write(struct.pack("<4sII", b"glTF", 2, 12 + 8 + len(js) + 8 + len(self.bin)))
            f.write(struct.pack("<I4s", len(js), b"JSON") + js)
            f.write(struct.pack("<I4s", len(self.bin), b"BIN\0") + bytes(self.bin))


def to_gltf(game, model_name, model, out_path, pose_name=None, log=print):
    pose_name, pose = choose_bindpose(game, model_name, model, pose_name)
    if pose_name:
        log("rest pose: %s" % pose_name)
    elif sum(1 for nd in model.nodes if nd["kind"] == 0) > 1:
        log("no rest pose found for its %d bones: its pieces are shown at the origin" %
            sum(1 for nd in model.nodes if nd["kind"] == 0))
    worlds = world_matrices(model, pose)
    names = joint_names(model, pose_name)
    g = Glb()
    j = g.j
    # joints: bones under their parent bone, joints under the bone before them
    parent_of = {}
    for nd in model.nodes:
        if nd["kind"] == 0:
            k = nd["prev"]
            while k < model.count and model.nodes[k]["kind"] != 0:
                k = model.nodes[k]["prev"]
            parent_of[nd["own"]] = model.nodes[k]["own"] if k < model.count else None
        elif nd["kind"] == 2:
            parent_of.setdefault(nd["own"], model.nodes_raw_own(nd["prev"]))
    mids = sorted(worlds)
    gnode = {}
    for m in mids:
        gnode[m] = len(j["nodes"])
        j["nodes"].append({"name": names[m]})
    roots = []
    for m in mids:
        p = parent_of.get(m)
        local = mmul(rigid_inverse(worlds[p]), worlds[m]) if p is not None and p in worlds and p != m else worlds[m]
        n = j["nodes"][gnode[m]]
        n["translation"] = [local[0][3], local[1][3], local[2][3]]
        n["rotation"] = mat_quat(local)
        if p is not None and p in gnode and p != m:
            j["nodes"][gnode[p]].setdefault("children", []).append(gnode[m])
        else:
            roots.append(gnode[m])
    arm = len(j["nodes"])
    j["nodes"].append({"name": "skeleton", "children": roots,
                       "extras": {"tsfp_model": model_name, "tsfp_bindpose": pose_name or ""}})
    j["scenes"][0]["nodes"].append(arm)
    ibm = g.accessor([colmajor(rigid_inverse(worlds[m])) for m in mids], 5126, "MAT4")
    j["skins"].append({"joints": [gnode[m] for m in mids], "inverseBindMatrices": ibm, "skeleton": arm,
                       "name": "skeleton"})
    jindex = {m: k for k, m in enumerate(mids)}
    # materials and their pictures
    mat_index = {}
    for mi in range(len(model.materials)):
        t = u32(model.materials[mi], 0)
        mat = {"name": "m%02d_t%06x" % (mi, model.textures[t][0]) if t < len(model.textures) else "m%02d_plain" % mi,
               "pbrMetallicRoughness": {"metallicFactor": 0.0, "roughnessFactor": 1.0},
               "extras": {"tsfp_material": mi}}
        pic = model.texture_rgba(game, t) if t < len(model.textures) else None
        if pic:
            w, h, px = pic
            img = len(j["images"])
            j["images"].append({"name": "t%06x" % model.textures[t][0], "mimeType": "image/png",
                                "bufferView": g.view(png_encode(w, h, px))})
            j["textures"].append({"source": img, "sampler": 0})
            mat["pbrMetallicRoughness"]["baseColorTexture"] = {"index": len(j["textures"]) - 1}
            if any(px[k] < 250 for k in range(3, len(px), 4)):
                mat["alphaMode"] = "MASK"
                mat["alphaCutoff"] = 0.5
        mat_index[mi] = len(j["materials"])
        j["materials"].append(mat)
    # meshes
    for nd in model.nodes:
        for p, ps in enumerate(nd["passes"]):
            if not ps or not ps["tris"]:
                continue
            prims = []
            by_mat = {}
            for a, b, c, mat in ps["tris"]:
                by_mat.setdefault(mat, []).append((a, b, c))
            for mat, tris in sorted(by_mat.items()):
                used = sorted({v for t in tris for v in t})
                remap = {v: k for k, v in enumerate(used)}
                P, N, T, C, J, W = [], [], [], [], [], []
                for v in used:
                    vx = ps["verts"][v]
                    pal = nd["palette"]
                    m = pal[vx["slot"]] if vx["slot"] < len(pal) else pal[0]  # the matrix the game uses
                    if m not in worlds:
                        m = vx["m"] if vx["m"] in worlds else 0
                    if ps["in_model"]:
                        m = vx["m"] if vx["m"] in worlds else m
                        P.append(vx["p"])
                        N.append(normalize(vx["n"]))
                    else:
                        P.append(xform(worlds[m], vx["p"]))
                        N.append(normalize(xform(worlds[m], vx["n"], 0.0)))
                    T.append(vx["uv"])
                    if vx["col"]:
                        bgra = vx["col"]
                        C.append(tuple(min(255, c * 2) for c in (bgra[2], bgra[1], bgra[0], bgra[3])))
                    J.append((jindex[m], 0, 0, 0))
                    W.append((1.0, 0.0, 0.0, 0.0))
                attrs = {"POSITION": g.accessor(P, 5126, "VEC3", 34962, minmax=True),
                         "TEXCOORD_0": g.accessor(T, 5126, "VEC2", 34962),
                         "JOINTS_0": g.accessor(J, 5121, "VEC4", 34962),
                         "WEIGHTS_0": g.accessor(W, 5126, "VEC4", 34962)}
                if ps["words"][3]:  # without, the game lights it by its colours alone
                    attrs["NORMAL"] = g.accessor(N, 5126, "VEC3", 34962)
                if C:
                    attrs["COLOR_0"] = g.accessor(C, 5121, "VEC4", 34962, normalized=True)
                idx = [remap[v] for t in tris for v in t]
                prim = {"attributes": attrs, "indices": g.accessor(idx, 5125, "SCALAR", 34963), "mode": 4}
                if mat in mat_index:
                    prim["material"] = mat_index[mat]
                prims.append(prim)
            name = node_name(model, nd, p, names)
            j["meshes"].append({"name": name, "primitives": prims})
            j["nodes"].append({"name": name, "mesh": len(j["meshes"]) - 1, "skin": 0,
                               "extras": {"tsfp_node": nd["i"], "tsfp_pass": p}})
            j["scenes"][0]["nodes"].append(len(j["nodes"]) - 1)
    g.write(out_path)
    return pose_name


# ----------------------------------------------------------------------------------------------------------------
# glTF back to the model


class GltfIn:
    def __init__(self, path):
        with open(path, "rb") as f:
            d = f.read()
        if d[:4] == b"glTF":
            n = u32(d, 12)
            self.j = json.loads(d[20:20 + n])
            o = 20 + n
            self.bin = d[o + 8:o + 8 + u32(d, o)] if o + 8 <= len(d) else b""
            self.dir = os.path.dirname(path)
        else:
            self.j = json.loads(d)
            self.bin = b""
            self.dir = os.path.dirname(path)
        self._buffers = {}

    def buffer(self, i):
        if i not in self._buffers:
            b = self.j["buffers"][i]
            if "uri" not in b:
                self._buffers[i] = self.bin
            elif b["uri"].startswith("data:"):
                self._buffers[i] = base64.b64decode(b["uri"].split(",", 1)[1])
            else:
                with open(os.path.join(self.dir, b["uri"]), "rb") as f:
                    self._buffers[i] = f.read()
        return self._buffers[i]

    def view_bytes(self, vi):
        v = self.j["bufferViews"][vi]
        b = self.buffer(v["buffer"])
        o = v.get("byteOffset", 0)
        return b[o:o + v["byteLength"]], v.get("byteStride")

    def accessor(self, ai):
        a = self.j["accessors"][ai]
        comps = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}[a["type"]]
        fmt, size = {5126: ("f", 4), 5125: ("I", 4), 5123: ("H", 2), 5121: ("B", 1), 5122: ("h", 2),
                     5120: ("b", 1)}[a["componentType"]]
        n = a["count"]
        if "bufferView" not in a:
            out = [(0,) * comps] * n
        else:
            data, stride = self.view_bytes(a["bufferView"])
            o = a.get("byteOffset", 0)
            stride = stride or comps * size
            out = [struct.unpack_from("<%d%s" % (comps, fmt), data, o + k * stride) for k in range(n)]
        if a.get("normalized"):
            scale = {5121: 255.0, 5123: 65535.0, 5120: 127.0, 5122: 32767.0}.get(a["componentType"], 1.0)
            out = [tuple(c / scale for c in v) for v in out]
        return [v[0] for v in out] if comps == 1 else out

    def image(self, ii):
        im = self.j["images"][ii]
        if "bufferView" in im:
            data = self.view_bytes(im["bufferView"])[0]
        elif im.get("uri", "").startswith("data:"):
            data = base64.b64decode(im["uri"].split(",", 1)[1])
        else:
            with open(os.path.join(self.dir, im["uri"]), "rb") as f:
                data = f.read()
        return png_decode(data)

    def worlds(self):
        nodes = self.j.get("nodes", [])
        out = [None] * len(nodes)

        def local(n):
            if "matrix" in n:
                return from_colmajor(n["matrix"])
            t = n.get("translation", [0, 0, 0])
            q = n.get("rotation", [0, 0, 0, 1])
            s = n.get("scale", [1, 1, 1])
            m = qmat(q, t)
            for r in range(3):
                for c in range(3):
                    m[r][c] *= s[c]
            return m

        def walk(i, parent):
            out[i] = mmul(parent, local(nodes[i]))
            for c in nodes[i].get("children", []):
                walk(c, out[i])
        children = {c for n in nodes for c in n.get("children", [])}
        for i in range(len(nodes)):
            if i not in children:
                walk(i, ident())
        return out


def _strips(tris):
    """Triangles (a, b, c) as one index list of strips joined by repeated indices; each triangle keeps its winding."""
    by_edge = {}
    for k, (a, b, c) in enumerate(tris):
        for x, y, z in ((a, b, c), (b, c, a), (c, a, b)):
            by_edge.setdefault((x, y), []).append((k, z))
    used = [False] * len(tris)
    out = []
    for k0, t in enumerate(tris):
        if used[k0]:
            continue
        used[k0] = True
        s = list(t)
        while True:
            p, q = s[-2], s[-1]
            edge = (p, q) if (len(s) - 2) % 2 == 0 else (q, p)
            nxt = None
            for k, z in by_edge.get(edge, ()):
                if not used[k]:
                    nxt = (k, z)
                    break
            if not nxt:
                break
            used[nxt[0]] = True
            s.append(nxt[1])
        if out:
            out.append(out[-1])
            out.append(s[0])
            if len(out) % 2:
                out.append(s[0])
        out += s
    return out


class Builder:
    """The base model with passes' geometry, textures and materials added after its end."""

    def __init__(self, model):
        self.m = model
        self.out = bytearray(model.data)
        self.textures = [list(t) for t in model.textures]
        self.materials = [bytes(x) for x in model.materials]
        self.new_textures = {}  # id: .xbt bytes

    def put(self, data, align=16):
        while len(self.out) % align:
            self.out.append(0)
        at = len(self.out)
        self.out += data
        return at

    def set32(self, at, v):
        struct.pack_into("<I", self.out, at, v)

    def add_texture(self, game, w, h, rgba):
        xbt = encode_xbt(w, h, rgba)
        tid = 0x800000 | (zlib.crc32(xbt) & 0x7FFFFF)
        known = {t[0] for t in self.textures}
        while game.has(TEXTURE_PATH % tid) or (tid in known and tid not in self.new_textures) or \
                (tid in self.new_textures and self.new_textures[tid] != xbt):
            tid = 0x800000 | ((tid + 1) & 0x7FFFFF)
        self.new_textures[tid] = xbt
        for k, t in enumerate(self.textures):
            if t[0] == tid:
                return k
        self.textures.append([tid, tid, 0, 0])
        return len(self.textures) - 1

    def add_material(self, like, texture):
        base = bytearray(self.materials[like] if like < len(self.materials) else self.materials[0])
        struct.pack_into("<I", base, 0, texture)
        self.materials.append(bytes(base))
        return len(self.materials) - 1

    def write_pass(self, nd, p, verts, tris):
        """verts: [(pos, uv, normal, matrix, slot, colour BGRA or None)], tris: [(a, b, c, material)]."""
        a = nd["at"]
        if not tris:
            self.set32(a + 0x60 + 4 * p, 0)
            self.set32(a + 0xBC + 4 * p, 0)
            return
        base = nd["passes"][p]
        if base and base["in_model"]:
            raise ModelError("node %d keeps its vertices in the model's space (as Subway's soldiers do): changing "
                             "it is not supported yet" % nd["i"])
        kind = base["kind"] if base else (3 if nd["kind"] == 0 else 4)
        has_col = bool(base and base["words"][4])
        # the vertices by material run, in the order their triangles first use them
        runs = {}
        for t in tris:
            runs.setdefault(t[3], []).append(t[:3])
        order, index, run_lists = [], {}, []
        for mat, rt in sorted(runs.items()):
            first = len(order)
            for t in rt:
                for v in t:
                    if v not in index:
                        index[v] = len(order)
                        order.append(v)
            run_lists.append((mat, first, len(order), _strips([tuple(index[v] for v in t) for t in rt])))
        if len(order) > 65535:
            raise ModelError("node %d pass %d has %d vertices, at most 65535" % (nd["i"], p, len(order)))
        pos, uv, nrm, col = bytearray(), bytearray(), bytearray(), bytearray()
        for v in order:
            P, T, N, M, slot, C = verts[v]
            pos += struct.pack("<3fI", P[0], P[1], P[2], 0xFFFFFF00 | (0xE0 + 4 * slot))
            uv += struct.pack("<3f", T[0], T[1], 1.0)
            nrm += struct.pack("<II", pack_normal(N), M)
            col += bytes(C or (0x7F, 0x7F, 0x7F, 0x7F))
        chunks, lists = [], {}
        for mat, first, end, idx in run_lists:
            for s in range(first, end, CHUNK_VERTS):
                chunks.append([mat, min(CHUNK_VERTS, end - s), kind, len(chunks), s])
            if end == first:  # a run on earlier runs' vertices alone: a chunk of none, as the game's own have
                chunks.append([mat, 0, kind, len(chunks), first])
            lists[len(chunks) - 1] = idx
        w = list(base["words"]) if base else [0] * 6
        w[1] = self.put(pos)
        w[2] = self.put(uv) if not base or base["words"][2] else 0
        w[3] = self.put(nrm) if not base or base["words"][3] else 0
        w[4] = self.put(col) if has_col else 0
        if not w[0]:  # a pass the base lacked takes the first pass's render state
            w[0] = u32(self.out, a)
        strip_table = bytearray(8 * len(chunks))
        for k, idx in lists.items():
            at = self.put(struct.pack("<%dH" % len(idx), *idx))
            struct.pack_into("<II", strip_table, 8 * k, at, len(idx))
        st = self.put(strip_table, 4)
        ct = bytearray()
        for mat, n, kd, ix, start in chunks:
            ct += struct.pack("<HBBHHH", mat, n, kd, ix, start, 0xFFFF)
        ct += struct.pack("<HBBHHH", 0, 0, 0xFF, 0, 0, 0xFFFF)
        cta = self.put(ct, 4)
        for k in range(6):
            self.set32(a + 0x18 * p + 4 * k, w[k])
        self.set32(a + 0x60 + 4 * p, cta)
        self.set32(a + 0xBC + 4 * p, st)
        if p == 0:
            ntri = len(tris)
            nidx = sum(len(x[3]) for x in run_lists)
            struct.pack_into("<HH", self.out, a + 0x70, min(nidx, 0xFFFF), min(ntri, 0xFFFF))

    def finish(self):
        if self.textures != [list(t) for t in self.m.textures]:
            t = bytearray()
            for e in self.textures:
                t += struct.pack("<4I", *e)
            t += struct.pack("<4I", 0xFFFFFFFF, 0xFFFFFFFF, 0, 0)
            self.set32(0, self.put(t))
        if self.materials != list(self.m.materials):
            t = bytearray(b"".join(self.materials)) + self.m.mat_end
            self.set32(8, self.put(t))
        return bytes(self.out)


def from_gltf(game, model_name, model, glb_path, pose_name=None, log=print):
    """The model with the glTF's geometry, and its new textures: (bytes, {id: xbt})."""
    pose_name_found, pose = choose_bindpose(game, model_name, model, pose_name)
    gi = GltfIn(glb_path)
    j = gi.j
    gnodes = j.get("nodes", [])
    # the bind pose the glTF was made with, if its skeleton names one
    for n in gnodes:
        bp = n.get("extras", {}).get("tsfp_bindpose")
        if bp and not pose_name and bp != pose_name_found:
            pose_name_found, pose = choose_bindpose(game, model_name, model, bp)
    worlds = world_matrices(model, pose)
    inv = {m: rigid_inverse(w) for m, w in worlds.items()}
    names = joint_names(model, pose_name_found)
    by_name = {v: k for k, v in names.items()}
    gworld = gi.worlds()
    b = Builder(model)
    # materials: the model's by name, others new
    mat_of = {}
    pictures = {}

    def picture(mi_gltf):
        m = j["materials"][mi_gltf]
        tex = m.get("pbrMetallicRoughness", {}).get("baseColorTexture")
        if tex is None:
            return None
        src = j["textures"][tex["index"]].get("source")
        if src is None:
            return None
        if src not in pictures:
            pictures[src] = gi.image(src)
        return pictures[src]

    def material(mi_gltf, like):
        key = (mi_gltf, like)
        if key in mat_of:
            return mat_of[key]
        if mi_gltf is None:
            mat_of[key] = like
            return like
        m = j["materials"][mi_gltf]
        mm = re.match(r"m(\d+)_(t[0-9a-f]{6}|plain)", m.get("name", ""))
        mine = int(mm.group(1)) if mm and int(mm.group(1)) < len(model.materials) else None
        if mine is not None and mi_gltf in mat_of:
            mat_of[key] = mat_of[mi_gltf]
            return mat_of[key]
        pic = picture(mi_gltf)
        if mine is not None:
            t = u32(model.materials[mine], 0)
            old = model.texture_rgba(game, t) if t < len(model.textures) else None
            if pic is None or (old and old[0] == pic[0] and old[1] == pic[1] and
                               max(abs(x - y) for x, y in zip(old[2], pic[2])) <= 3):
                mat_of[key] = mat_of[mi_gltf] = mine
                return mine
            log("material %s: its picture changed, a new texture" % m.get("name"))
            r = mat_of[mi_gltf] = b.add_material(mine, b.add_texture(game, *pic))
        elif pic is None:
            log("material %s has no picture: the piece's own material is used" % m.get("name", mi_gltf))
            r = like
        else:
            log("material %s: a new texture" % m.get("name", mi_gltf))
            r = b.add_material(like, b.add_texture(game, *pic))
        mat_of[key] = r
        return r

    # the geometry, gathered by (node, pass)
    gathered = {}  # (node, pass): (verts, tris)
    owner = {}  # matrix: node that has it as its own
    for nd in model.nodes:
        if nd["kind"] in (0, 2):
            owner.setdefault(nd["own"], nd["i"])
    named = set()
    for gn_i, gn in enumerate(gnodes):
        if "mesh" not in gn:
            continue
        name = re.sub(r"\.\d+$", "", gn.get("name", ""))
        mt = re.match(r"n(\d+)_p(\d)", name)
        target = None
        if mt and int(mt.group(1)) < model.count and int(mt.group(2)) < 3:
            target = (int(mt.group(1)), int(mt.group(2)))
            named.add(target)
        skin = j["skins"][gn["skin"]] if "skin" in gn else None
        jm = []  # skin joint -> matrix id or None
        jworld = []
        if skin:
            ibm = gi.accessor(skin["inverseBindMatrices"]) if "inverseBindMatrices" in skin else None
            for k, ji in enumerate(skin["joints"]):
                jn = re.sub(r"\.\d+$", "", gnodes[ji].get("name", ""))
                jm.append(by_name.get(jn))
                jworld.append(mmul(gworld[ji], from_colmajor(ibm[k])) if ibm else gworld[ji])
        mesh = j["meshes"][gn["mesh"]]
        for prim in mesh["primitives"]:
            if prim.get("mode", 4) != 4:
                continue
            at = prim["attributes"]
            P = gi.accessor(at["POSITION"])
            N = gi.accessor(at["NORMAL"]) if "NORMAL" in at else None
            T = gi.accessor(at["TEXCOORD_0"]) if "TEXCOORD_0" in at else [(0.0, 0.0)] * len(P)
            C = gi.accessor(at["COLOR_0"]) if "COLOR_0" in at else None
            J = gi.accessor(at["JOINTS_0"]) if "JOINTS_0" in at and skin else None
            W = gi.accessor(at["WEIGHTS_0"]) if "WEIGHTS_0" in at and skin else None
            idx = gi.accessor(prim["indices"]) if "indices" in prim else list(range(len(P)))
            # each vertex: its place in the model's space and its weight a matrix
            wp, wn, wts = [], [], []
            for v in range(len(P)):
                weights = {}
                if J:
                    for jj, ww in zip(J[v], W[v]):
                        if ww > 0 and jj < len(jm):
                            weights[jm[jj]] = weights.get(jm[jj], 0) + ww
                if skin and J and sum(ww for jj, ww in zip(J[v], W[v]) if jj < len(jworld)) > 0:
                    tot = sum(ww for jj, ww in zip(J[v], W[v]) if jj < len(jworld))
                    p_ = [0.0, 0.0, 0.0]
                    n_ = [0.0, 0.0, 0.0]
                    for jj, ww in zip(J[v], W[v]):
                        if ww <= 0 or jj >= len(jworld):
                            continue
                        q = xform(jworld[jj], P[v])
                        p_ = [p_[c] + q[c] * ww / tot for c in range(3)]
                        if N:
                            q = xform(jworld[jj], N[v], 0.0)
                            n_ = [n_[c] + q[c] * ww / tot for c in range(3)]
                    wp.append(tuple(p_))
                    wn.append(normalize(n_) if N else (0.0, 1.0, 0.0))
                else:
                    wp.append(xform(gworld[gn_i], P[v]))
                    wn.append(normalize(xform(gworld[gn_i], N[v], 0.0)) if N else (0.0, 1.0, 0.0))
                weights.pop(None, None)
                wts.append(weights)
            for k in range(0, len(idx) - 2, 3):
                tri = idx[k:k + 3]
                tgt = target
                if tgt is None:
                    total = {}
                    for v in tri:
                        for m, ww in wts[v].items():
                            total[m] = total.get(m, 0) + ww
                    best = max(total, key=total.get) if total else None
                    if best is None or best not in owner:
                        raise ModelError("mesh %s: name it after a node (n04_p0...) or weight it to the skeleton"
                                         % gn.get("name"))
                    tgt = (owner[best], 0)
                nd = model.nodes[tgt[0]]
                pal = nd["palette"]
                like = None
                base = nd["passes"][tgt[1]]
                if base and base["tris"]:
                    like = base["tris"][0][3]
                elif nd["passes"][0] and nd["passes"][0]["tris"]:
                    like = nd["passes"][0]["tris"][0][3]
                else:
                    like = 0
                mat = material(prim.get("material"), like)
                verts, tris = gathered.setdefault(tgt, ([], []))
                key_of = gathered.setdefault(("keys",) + tgt, {})
                out_t = []
                for v in tri:
                    ws = wts[v]
                    slots = [s for s in range(len(pal)) if pal[s] in worlds] or [0]
                    slot = max(slots, key=lambda s: ws.get(pal[s], 0)) if ws else slots[0]
                    if not ws or ws.get(pal[slot], 0) <= 0:
                        # no weight on the node's matrices: the nearest of them
                        slot = min(slots, key=lambda s: sum(
                            (worlds.get(pal[s], worlds[0])[c][3] - wp[v][c]) ** 2 for c in range(3)))
                    m = pal[slot]
                    lp = xform(inv[m], wp[v])
                    ln = normalize(xform(inv[m], wn[v], 0.0))
                    col = None
                    if C:
                        c = C[v] + (1.0,) * (4 - len(C[v]))
                        col = tuple(min(0x7F, int(round(x * 0x7F))) for x in (c[2], c[1], c[0], c[3]))
                    rec = (tuple(round(x, 6) for x in lp), (round(T[v][0], 6), round(T[v][1], 6)),
                           tuple(round(x, 4) for x in ln), m, slot, col)
                    if rec not in key_of:
                        key_of[rec] = len(verts)
                        verts.append(rec)
                    out_t.append(key_of[rec])
                if len(set(out_t)) == 3:
                    tris.append(tuple(out_t) + (mat,))
    for key in list(gathered):
        if key[0] == "keys":
            continue
        verts, tris = gathered[key]
        nd = model.nodes[key[0]]
        b.write_pass(nd, key[1], verts, tris)
        log("%s: %d vertices, %d triangles" % (node_name(model, nd, key[1], names), len(verts), len(tris)))
    # a glTF of this model's pieces (any named after one): the pieces it lacks were deleted
    if named:
        for nd in model.nodes:
            for p, ps in enumerate(nd["passes"]):
                if ps and ps["tris"] and (nd["i"], p) not in gathered:
                    b.write_pass(nd, p, [], [])
                    log("%s: removed" % node_name(model, nd, p, names))
    return b.finish(), b.new_textures


# ----------------------------------------------------------------------------------------------------------------
# Commands


def write_mod(game, model_path, data, textures, mod_folder, log=print):
    files = os.path.join(mod_folder, "files")
    out = os.path.join(files, *model_path.split("/"))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "wb") as f:
        f.write(data)
    log("wrote %s" % out)
    for tid, xbt in textures.items():
        p = os.path.join(files, *(TEXTURE_PATH % tid).split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(xbt)
        log("wrote %s" % p)
    toml = os.path.join(mod_folder, "mod.toml")
    if not os.path.exists(toml):
        with open(toml, "w", encoding="utf-8") as f:
            f.write('name = "%s"\ndescription = "%s, edited in Blender"\n' %
                    (os.path.basename(os.path.normpath(mod_folder)), model_path.split("/")[-1]))


def load(game, model):
    name, path = game.find(model)
    return name, path, Model(game.read(path))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("list", help="the models the game names")
    s.add_argument("game")
    s.add_argument("text", nargs="?", default="")
    s = sub.add_parser("gltf", help="a model as .glb")
    s.add_argument("game")
    s.add_argument("model")
    s.add_argument("out")
    s.add_argument("--bindpose")
    s = sub.add_parser("fromgltf", help="an edited .glb into a mod")
    s.add_argument("game")
    s.add_argument("model")
    s.add_argument("glb")
    s.add_argument("mod")
    s.add_argument("--bindpose")
    s = sub.add_parser("extract", help="a model's .xbr and its textures' .xbt")
    s.add_argument("game")
    s.add_argument("model")
    s.add_argument("folder")
    a = ap.parse_args(argv)
    try:
        game = Game(a.game)
        if a.cmd == "list":
            for n in game.models():
                if a.text.lower() in n:
                    print(n)
        elif a.cmd == "gltf":
            name, path, m = load(game, a.model)
            to_gltf(game, name, m, a.out, a.bindpose)
            print("wrote %s" % a.out)
        elif a.cmd == "fromgltf":
            name, path, m = load(game, a.model)
            data, tex = from_gltf(game, name, m, a.glb, a.bindpose)
            write_mod(game, path, data, tex, a.mod)
        elif a.cmd == "extract":
            name, path, m = load(game, a.model)
            os.makedirs(a.folder, exist_ok=True)
            with open(os.path.join(a.folder, os.path.basename(path)), "wb") as f:
                f.write(m.data)
            for t in m.textures:
                d = game.read(TEXTURE_PATH % t[0])
                if d:
                    with open(os.path.join(a.folder, "%06x.xbt" % t[0]), "wb") as f:
                        f.write(d)
            print("wrote %s" % a.folder)
    except ModelError as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
