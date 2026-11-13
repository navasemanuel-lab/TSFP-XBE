"""Writes the [[hook]] block of the native frame rate (src/game/native_clock.cpp) into config/game.toml.

The game counts time two ways: whole 60 Hz ticks (0x4E7940 raw, 0x4E7944 game) and the same span as a float
(0x4E7948, 0x4E794C). Native mode gives every frame its real, fractional span, so:
  * every int -> float conversion of a tick word (cvtsi2ss / fild) takes the float next to it (+8)
  * the few "did time pass" tests of a tick word, which skip a whole update on a frame with no whole tick, test the float

Run after the recompiler has written generated/game (any build):  python -I tools/native_hooks.py
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, "generated", "game")
TOML = os.path.join(ROOT, "config", "game.toml")
BEGIN = "# >>> native frame rate (tools/native_hooks.py)"
END = "# <<< native frame rate"

# loads of 0x4E7944 that only decide whether the update runs at all (Doffman's pause gates, US 45410066): the register
# is overwritten right after, or only tested. Native: 1 when game time advanced this frame, else 0
GATES = [
    0x108793, 0x164E00, 0x224170, 0x222B60, 0x21AF90, 0x1D88B0, 0x1260C0, 0x228880, 0x15EB0C, 0xD5EBF,
    0xC73C0, 0xC7610, 0xCEA8D, 0xCA040, 0xC7D73, 0xC7E0C, 0xCD70F, 0xA75E7, 0x1B6FF0, 0x217059, 0x216958,
    0xF4A66, 0xBB6C0, 0x2B93DA, 0x14A5CD, 0x17E6C3, 0x17DA03, 0x166899, 0x172AB5,
]

LINE = re.compile(r"^\s*// ([0-9A-F]{8}) (\S+) (.*)$")


def main():
    sites = {}
    for path in glob.glob(os.path.join(GEN, "x86_recomp.*.cpp")):
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                if "0x004E794" not in line:
                    continue
                m = LINE.match(line)
                if not m:
                    continue
                addr, mnem, ops = int(m.group(1), 16), m.group(2), m.group(3)
                sites[addr] = (mnem, ops)
    hooks = {}
    for addr, (mnem, ops) in sorted(sites.items()):
        word = "Game" if "0x004E7944]" in ops else "Raw" if "0x004E7940]" in ops else None
        if not word:
            continue
        if mnem == "cvtsi2ss":
            m = re.match(r"xmm(\d), \[", ops)
            if m:
                hooks[addr] = f"Native{word}Xmm{m.group(1)}"
        elif mnem == "fild":
            hooks[addr] = f"Native{word}Fild"
    for addr in GATES:
        mnem, ops = sites.get(addr, (None, None))
        m = re.match(r"(e[a-d]x|e[sd]i), \[0x004E7944\]", ops or "")
        if mnem != "mov" or not m:
            sys.exit(f"gate {addr:X} is not a mov of 0x4E7944: {mnem} {ops}")
        hooks[addr] = "NativeGate" + m.group(1)[1:].capitalize()
    out = [BEGIN]
    for addr, call in sorted(hooks.items()):
        out += ["[[hook]]", f"at = 0x{addr:X}", f'call = "{call}"', "after = true"]
    out.append(END)
    block = "\n".join(out) + "\n"
    with open(TOML, encoding="utf-8", newline="") as f:
        text = f.read()
    crlf = "\r\n" in text
    text = text.replace("\r\n", "\n")
    if BEGIN in text:
        text = re.sub(re.escape(BEGIN) + r".*?" + re.escape(END) + r"\n", lambda _: block, text, flags=re.S)
    else:
        text = text.rstrip("\n") + "\n\n" + block
    if crlf:
        text = text.replace("\n", "\r\n")
    with open(TOML, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    kinds = {}
    for call in hooks.values():
        kinds[call] = kinds.get(call, 0) + 1
    print(f"{len(hooks)} hooks:", ", ".join(f"{k} x{v}" for k, v in sorted(kinds.items())))


main()
