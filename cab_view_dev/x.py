#!/usr/bin/env python3
"""Tiny X11 driver for the test display: click, drag, key, move."""
import sys, time
sys.path.insert(0, '/home/claude/deps/py/python-xlib')
from Xlib import X, XK, display
from Xlib.ext import xtest

d = display.Display(':99')

def move(x, y):
    xtest.fake_input(d, X.MotionNotify, x=int(x), y=int(y)); d.sync(); time.sleep(0.05)

def click(x, y, button=1, double=False):
    move(x, y)
    for _ in range(2 if double else 1):
        xtest.fake_input(d, X.ButtonPress, button); d.sync(); time.sleep(0.05)
        xtest.fake_input(d, X.ButtonRelease, button); d.sync(); time.sleep(0.08)

def drag(x0, y0, x1, y1, steps=10):
    move(x0, y0)
    xtest.fake_input(d, X.ButtonPress, 1); d.sync(); time.sleep(0.1)
    for i in range(1, steps + 1):
        move(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps)
    xtest.fake_input(d, X.ButtonRelease, 1); d.sync(); time.sleep(0.1)

def key(name, mods=()):
    codes = [d.keysym_to_keycode(XK.string_to_keysym(m)) for m in mods]
    for c in codes: xtest.fake_input(d, X.KeyPress, c)
    kc = d.keysym_to_keycode(XK.string_to_keysym(name))
    xtest.fake_input(d, X.KeyPress, kc); d.sync(); time.sleep(0.03)
    xtest.fake_input(d, X.KeyRelease, kc)
    for c in reversed(codes): xtest.fake_input(d, X.KeyRelease, c)
    d.sync(); time.sleep(0.05)

def type_text(s):
    special = {' ': 'space', '_': 'underscore', '-': 'minus', '.': 'period', '/': 'slash', ',': 'comma', '0':'0'}
    for ch in s:
        if ch.isupper():
            key(ch, mods=('Shift_L',))
        elif ch == '_':
            key('minus', mods=('Shift_L',))
        else:
            key(special.get(ch, ch))

if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'click': click(int(sys.argv[2]), int(sys.argv[3]))
    elif cmd == 'dclick': click(int(sys.argv[2]), int(sys.argv[3]), double=True)
    elif cmd == 'rclick': click(int(sys.argv[2]), int(sys.argv[3]), button=3)
    elif cmd == 'wheel': 
        n = int(sys.argv[4]); b = 4 if n > 0 else 5
        for _ in range(abs(n)): click(int(sys.argv[2]), int(sys.argv[3]), button=b)
    elif cmd == 'move': move(int(sys.argv[2]), int(sys.argv[3]))
    elif cmd == 'drag': drag(*(int(a) for a in sys.argv[2:6]))
    elif cmd == 'key': key(sys.argv[2], tuple(sys.argv[3:]))
    elif cmd == 'type': type_text(sys.argv[2])
