"""Copies the keyboard and mouse pictures (Xelu's Keyboard & Mouse prompts, Alt style) the launcher and game show
for keys into prompts/keyboard under the short names icons.cpp looks for:
python -I tools/make_key_prompts.py <pack folder with the Alt pictures>"""
import os, shutil, sys

src, dst = sys.argv[1], os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'prompts', 'keyboard')
os.makedirs(dst, exist_ok=True)
names = {}
for c in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ': names[c.lower()] = 'T_%s_Key_Alt' % c
for c in '012356789': names[c] = 'T_%s_Key_Alt' % c
names['4'] = 'T_3_Key_Alt-1'  # the pack's file for 4 is misnamed
for n in range(1, 13): names['f%d' % n] = 'T_F%d_Key_Alt' % n
names.update({'enter': 'T_Enter_Key_Alt', 'space': 'T_Space_Key_Alt', 'backspace': 'T_BackSpace_Key_Alt', 'esc': 'T_Esc_Key_Alt',
              'tab': 'T_Tab_Key_Alt', 'ctrl': 'T_Crtl_Key_Alt', 'shift': 'T_Shift_Key_Alt', 'alt': 'T_Alt_Key_Alt',
              'capslock': 'T_CapsLock_Key_Alt', 'up': 'T_Up_Key_Alt', 'down': 'T_Down_Key_Alt', 'left': 'T_Left_Key_Alt',
              'right': 'T_Right_Key_Alt', 'pageup': 'T_PageUp_Key_Alt', 'pagedown': 'T_PageDown_Key_Alt', 'ins': 'T_Ins_Key_Alt',
              'del': 'T_Del_Key_Alt', 'home': 'T_Home_Key_Alt', 'end': 'T_End_Key_Alt', 'mouse_left': 'T_Mouse_Left_Key_Alt',
              'mouse_right': 'T_Mouse_Right_Key_Alt', 'mouse_middle': 'T_Mouse_Middle_Key_Alt', 'mouse_xy': 'T_Mouse_XY_Key_Alt',
              'mouse_x': 'T_Mouse_X_Key_Alt', 'mouse_y': 'T_Mouse_Y_Key_Alt', 'mouse_scroll_up': 'T_Mouse_Scroll_Up_Key_Dark_Key_Alt',
              'mouse_scroll_down': 'T_Mouse_Scroll_Down_Key_Dark_Key_Alt', 'mouse': 'T_Mouse_Simple_Key_Alt'})
for short, full in names.items():
    shutil.copyfile(os.path.join(src, full + '.png'), os.path.join(dst, short + '.png'))
# the mouse's side buttons: the pack has none, so the plain mouse gets two bars on its left edge, the one pressed lit
from PIL import Image, ImageDraw
base = Image.open(os.path.join(dst, 'mouse.png')).convert('RGBA')
for name, lit in (('mouse_forward', 0), ('mouse_back', 1)):
    big = Image.new('RGBA', (base.width * 4, base.height * 4), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    for i, (y0, y1) in enumerate(((30, 52), (57, 79))):
        d.rounded_rectangle([14 * 4, y0 * 4, 34 * 4, y1 * 4], radius=7 * 4,
                            fill=(255, 255, 254, 255) if i == lit else (15, 14, 23, 255))
    out = base.copy()
    out.alpha_composite(big.resize(base.size, Image.LANCZOS))
    out.save(os.path.join(dst, name + '.png'))
print(len(names) + 2, 'pictures in', os.path.normpath(dst))
