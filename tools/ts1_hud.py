"""TimeSplitters 1's HUD textures as a mod for the HUD style TimeSplitters 1.

Reads the ammunition icons of the player's own TimeSplitters (PS2, extracted: PAK/MISC/textures/misc/ammo_*.qpm)
and writes them as .xbt into a mod folder, at ts1/<sprite>.xbt by the names of this game's sprites (the game asks
for none of these paths: the HUD serves them in that style only, hud_ammo.cpp).

    python tools/ts1_hud.py "<TimeSplitters folder>" <mods folder>

.qpm: a text header "Q8\\n<w> <h>\\n255 v 256 \\n", 256 colours RGBA (alpha 0x80 opaque, in the PS2's CLUT order),
then w*h indices, line by line.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tsfp_model import encode_xbt  # noqa: E402

# this game's sprite names, by TimeSplitters 1's icon each takes
ICONS = {
    "ammo_small": ["ammo_small"],
    "ammo_big": ["ammo_big", "ammo_sniper"],
    "ammo_shell": ["ammo_shell"],
    "ammo_plasma": ["ammo_plasma", "ammo_plasmamachine", "ammo_sticky"],
    "ammo_rocket": ["ammo_rocket", "ammo_homingrocket"],
    "ammo_granade": ["ammo_grenade"],
    "ammo_brick": ["ammo_brick"],
    "ammo_prox": ["ammo_prox"],
    "ammo_remote": ["ammo_remote"],
    "ammo_timed": ["ammo_timed"],
    "ammo_tnt": ["ammo_tnt"],
}


def decode_qpm(d):
    m = re.match(rb"Q8\n(\d+) (\d+)\n255 v 256 *\n", d)
    if not m:
        raise ValueError("not a .qpm")
    w, h = int(m.group(1)), int(m.group(2))
    o = m.end()
    pal, pix = d[o:o + 1024], d[o + 1024:o + 1024 + w * h]
    colours = []
    for i in range(256):
        j = (i & 0xE7) | ((i & 8) << 1) | ((i & 16) >> 1)
        r, g, b, a = pal[j * 4:j * 4 + 4]
        colours.append(bytes((r, g, b, min(255, a * 2))))
    return w, h, b"".join(colours[p] for p in pix)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src = os.path.join(sys.argv[1], "PAK", "MISC", "textures", "misc")
    mod = os.path.join(sys.argv[2], "ts1_hud")
    out = os.path.join(mod, "files", "ts1")
    os.makedirs(out, exist_ok=True)
    n = 0
    for icon, names in ICONS.items():
        w, h, rgba = decode_qpm(open(os.path.join(src, icon + ".qpm"), "rb").read())
        xbt = encode_xbt(w, h, rgba)
        for name in names:
            open(os.path.join(out, name + ".xbt"), "wb").write(xbt)
            n += 1
    with open(os.path.join(mod, "mod.toml"), "w", encoding="utf-8") as f:
        f.write('name = "TS1 HUD"\nauthor = "TSFP recomp"\n'
                'description = "TimeSplitters 1\'s ammunition icons for the HUD style TimeSplitters 1"\n')
    print("%d textures written to %s" % (n, out))


if __name__ == "__main__":
    main()
