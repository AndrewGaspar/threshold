#!/usr/bin/env python3
"""Constant-rate horizontal mouse-look (camera yaw) through XTEST, for frame-pacing tests (from Pharos).
Runs inside the instance container. Sends `dx` pixels every `period_ms` for `seconds`, on an
absolute timeline (no drift), and logs nothing.

usage: spin.py <seconds> <dx> [period_ms]
"""
import ctypes
import sys
import time

secs, dx = float(sys.argv[1]), int(sys.argv[2])
period = float(sys.argv[3]) / 1000.0 if len(sys.argv) > 3 else 0.002

x11 = ctypes.CDLL("libX11.so.6")
xtst = ctypes.CDLL("libXtst.so.6")
x11.XOpenDisplay.restype = ctypes.c_void_p
x11.XFlush.argtypes = [ctypes.c_void_p]
xtst.XTestFakeRelativeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_ulong]
dpy = x11.XOpenDisplay(None)
if not dpy:
    sys.exit("cannot open display")

t0 = time.monotonic()
n = 0
while True:
    t = t0 + n * period
    if t - t0 > secs:
        break
    left = t - time.monotonic()
    if left > 0:
        time.sleep(left)
    xtst.XTestFakeRelativeMotionEvent(dpy, dx, 0, 0)
    x11.XFlush(dpy)
    n += 1
