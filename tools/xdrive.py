#!/usr/bin/env python3
"""Drives an X display through XTest, for screenshots and manual checks in a
private Xvfb (no xdotool needed; only libX11 and libXtst).

    DISPLAY=:37 tools/xdrive.py key ctrl+2 sleep 0.5 shot out.png \
        type hello click 400 300 move 10 10 button 3 400 300

Commands (any number, in order):
    key SPEC        e.g. ctrl+shift+n, alt+Left, space, question, F2
    type TEXT       types plain ASCII text
    click X Y       left click at window-system coordinates
    button N X Y    presses mouse button N (3: right, 8/9: thumb buttons)
    move X Y        moves the pointer
    drag X1 Y1 X2 Y2  presses the left button at X1 Y1, moves to X2 Y2, releases
    sleep SECONDS
    shot FILE       saves the whole screen (ImageMagick `import`)
"""
import ctypes
import ctypes.util
import os
import subprocess
import sys
import time

x11 = ctypes.CDLL(ctypes.util.find_library("X11") or "libX11.so.6")
xtst = ctypes.CDLL(ctypes.util.find_library("Xtst") or "libXtst.so.6")
x11.XOpenDisplay.restype = ctypes.c_void_p
x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
x11.XStringToKeysym.restype = ctypes.c_ulong
x11.XStringToKeysym.argtypes = [ctypes.c_char_p]
x11.XKeysymToKeycode.restype = ctypes.c_ubyte
x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
x11.XFlush.argtypes = [ctypes.c_void_p]
x11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
x11.XDefaultScreen.argtypes = [ctypes.c_void_p]
xtst.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
xtst.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
xtst.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_ulong]

dpy = x11.XOpenDisplay(None)
if not dpy:
    sys.exit("xdrive: can't open the display")

MODS = {"ctrl": "Control_L", "shift": "Shift_L", "alt": "Alt_L", "super": "Super_L"}
NAMES = {"enter": "Return", "esc": "Escape", "escape": "Escape", "space": "space", "tab": "Tab",
         "backspace": "BackSpace", "delete": "Delete", "/": "slash", "?": "question", "=": "equal",
         "-": "minus", "+": "plus", "menu": "Menu", "home": "Home", "end": "End", "up": "Up",
         "down": "Down", "left": "Left", "right": "Right", "pageup": "Prior", "pagedown": "Next"}


def code(name):
    ks = x11.XStringToKeysym(name.encode())
    if not ks:
        sys.exit("xdrive: unknown key " + name)
    return x11.XKeysymToKeycode(dpy, ks)


def press(spec):
    parts = spec.split("+") if spec != "+" else ["+"]
    mods = [MODS[p.lower()] for p in parts[:-1]]
    key = parts[-1]
    name = NAMES.get(key.lower(), key)
    shifted = {"question", "plus", "asciitilde", "underscore", "exclam", "at", "numbersign", "dollar",
               "percent", "asciicircum", "ampersand", "asterisk", "parenleft", "parenright", "colon",
               "quotedbl", "less", "greater", "braceleft", "braceright", "bar"}
    need_shift = name in shifted or (len(name) == 1 and name.isupper())
    if need_shift and "Shift_L" not in mods:
        mods.append("Shift_L")
    if len(name) == 1 and name.isupper():
        name = name.lower()
    for m in mods:
        xtst.XTestFakeKeyEvent(dpy, code(m), 1, 0)
    xtst.XTestFakeKeyEvent(dpy, code(name), 1, 0)
    xtst.XTestFakeKeyEvent(dpy, code(name), 0, 0)
    for m in reversed(mods):
        xtst.XTestFakeKeyEvent(dpy, code(m), 0, 0)
    x11.XSync(dpy, 0)
    time.sleep(0.05)


def main(args):
    i = 0
    while i < len(args):
        c = args[i]
        if c == "key":
            press(args[i + 1]); i += 2
        elif c == "type":
            for ch in args[i + 1]:
                press({" ": "space", "/": "slash", ".": "period", "-": "minus", "~": "asciitilde",
                       "_": "underscore"}.get(ch, ch))
            i += 2
        elif c in ("click", "button"):
            if c == "click":
                b, x, y = 1, int(args[i + 1]), int(args[i + 2]); i += 3
            else:
                b, x, y = int(args[i + 1]), int(args[i + 2]), int(args[i + 3]); i += 4
            xtst.XTestFakeMotionEvent(dpy, x11.XDefaultScreen(dpy), x, y, 0)
            xtst.XTestFakeButtonEvent(dpy, b, 1, 0)
            xtst.XTestFakeButtonEvent(dpy, b, 0, 0)
            x11.XSync(dpy, 0)
            time.sleep(0.05)
        elif c == "drag":
            x1, y1, x2, y2 = (int(v) for v in args[i + 1:i + 5]); i += 5
            scr = x11.XDefaultScreen(dpy)
            xtst.XTestFakeMotionEvent(dpy, scr, x1, y1, 0)
            xtst.XTestFakeButtonEvent(dpy, 1, 1, 0)
            x11.XSync(dpy, 0); time.sleep(0.1)
            for k in range(1, 21):
                xtst.XTestFakeMotionEvent(dpy, scr, x1 + (x2 - x1) * k // 20, y1 + (y2 - y1) * k // 20, 0)
                x11.XSync(dpy, 0); time.sleep(0.03)
            time.sleep(0.2)
            xtst.XTestFakeButtonEvent(dpy, 1, 0, 0)
            x11.XSync(dpy, 0); time.sleep(0.1)
        elif c == "move":
            xtst.XTestFakeMotionEvent(dpy, x11.XDefaultScreen(dpy), int(args[i + 1]), int(args[i + 2]), 0)
            x11.XSync(dpy, 0); i += 3
        elif c == "sleep":
            time.sleep(float(args[i + 1])); i += 2
        elif c == "shot":
            subprocess.run(["import", "-display", os.environ["DISPLAY"], "-window", "root", args[i + 1]], check=True)
            i += 2
        else:
            sys.exit("xdrive: unknown command " + c)


main(sys.argv[1:])
