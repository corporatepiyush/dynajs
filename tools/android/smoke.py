#!/usr/bin/env python3
# Drives tools/android/demo.js on a running emulator or device through adb and checks what each
# step did, from the app's own log (build-apk.sh log) and screen captures. Run it through
# `tools/android/build-apk.sh smoke`, which builds and installs the demo first.
#
# Coordinates come from the app's "screen" line (logical size, scale, insets), never from a
# guessed device: the nav bar is the bottom dock, 44 pt tall in the touch density, above the
# bottom inset; its three items share the width. Every check names what it expected.
import json, re, subprocess, sys, time, zlib, struct

ADB, PKG, ACT = sys.argv[1], "org.dynajs.app", "org.dynajs.app/org.dynajs.app.DynaActivity"
fails = 0


def adb(*a, out=False):
    r = subprocess.run([ADB, *a], capture_output=True)
    return r.stdout if out else r.returncode


def log():
    return subprocess.run([ADB, "shell", "run-as", PKG, "cat", "files/dynajs.log"], capture_output=True, text=True).stdout


def last(kind):
    lines = [l for l in log().splitlines() if l.startswith(kind + " ")]
    return lines[-1] if lines else ""


def state():
    m = re.match(r"state (\{.*\}) frames", last("state"))
    return json.loads(m.group(1)) if m else {}


def check(ok, what):
    global fails
    print(("ok    " if ok else "FAIL  ") + what)
    fails += not ok


def png():
    d = adb("exec-out", "screencap", "-p", out=True)
    i, idat = 8, b""
    while i < len(d):
        n = struct.unpack(">I", d[i:i + 4])[0]
        t, c = d[i + 4:i + 8], d[i + 8:i + 8 + n]
        i += 12 + n
        if t == b"IHDR":
            w, h, _, ct = struct.unpack(">IIBB", c[:10])
        elif t == b"IDAT":
            idat += c
    raw, bpp = zlib.decompress(idat), 4 if ct == 6 else 3
    st, out, prev, p = w * bpp, bytearray(), bytearray(w * bpp), 0
    for _ in range(h):
        f, line = raw[p], bytearray(raw[p + 1:p + 1 + st])
        p += 1 + st
        for x in range(st):
            a, b, c = line[x - bpp] if x >= bpp else 0, prev[x], prev[x - bpp] if x >= bpp else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += line
        prev = line
    return w, h, bpp, out


def black_fraction(y0, y1):
    w, h, bpp, px = png()
    n = k = 0
    for y in range(int(y0), int(y1), 9):
        for x in range(0, w, 9):
            i = (y * w + x) * bpp
            n += 1
            k += px[i] == px[i + 1] == px[i + 2] == 0
    return k / max(n, 1)


def wait_for(fn, what, secs=6):
    end = time.time() + secs
    while time.time() < end:
        if fn():
            return True
        time.sleep(0.3)
    return False


adb("shell", "am", "force-stop", PKG)
adb("shell", "am", "start", "-W", "-n", ACT)
check(wait_for(lambda: last("screen") and last("state"), "start"), "the app starts and logs its screen and state")
m = re.match(r"screen ([\d.]+)x([\d.]+) pt (\d+)x(\d+) px scale ([\d.]+) (\w+) insets ([-\d.,]+)", last("screen"))
check(m is not None, f"the screen line parses ({last('screen')!r})")
if not m:
    sys.exit(1)
lw, lh, pw, ph, s = float(m[1]), float(m[2]), int(m[3]), int(m[4]), float(m[5])
ins = [float(v) for v in m[7].split(",")]
check(abs(lw * s - pw) < 1 and abs(lh * s - ph) < 1, f"logical size x scale is the pixel size ({lw} x {s} = {pw})")
check(all(v >= 0 for v in ins), f"insets are never negative ({ins})")
nav_y = (lh - ins[3] - 22) * s
tab = lambda k: str(int((ins[0] + (lw - ins[0] - ins[2]) * (k + 0.5) / 3) * s))
for k, want in ((1, 1), (2, 2), (0, 0)):
    adb("shell", "input", "tap", tab(k), str(int(nav_y)))
    check(wait_for(lambda: state().get("nav") == want, "nav"), f"a tap on nav item {k} selects it (nav {state().get('nav')})")
fab = (str(int((lw - ins[2] - 16 - 28) * s)), str(int((lh - ins[3] - 44 - 16 - 28) * s)))
before = state().get("toasts", 0)
adb("shell", "input", "tap", *fab)
check(wait_for(lambda: state().get("toasts", 0) == before + 1, "fab"), "a tap on the FAB runs its action once")
adb("shell", "input", "tap", tab(1), str(int(nav_y)))
wait_for(lambda: state().get("nav") == 1, "contacts")
x, y0, y1 = str(int(lw / 2 * s)), int((ins[1] + 420) * s), int((ins[1] + 120) * s)
adb("shell", "input", "swipe", x, str(y0), x, str(y1), "120")
time.sleep(2)
check(state().get("sel") == -1, f"a swipe over the list scrolls it and selects nothing (sel {state().get('sel')})")
adb("shell", "input", "tap", x, str(int((ins[1] + 200) * s)))
check(wait_for(lambda: state().get("sel", -1) > 8, "sel"), f"a tap after the fling selects a row far down the list (sel {state().get('sel')})")
held = state().get("sel")
adb("shell", "input", "swipe", x, str(int((ins[1] + 250) * s)), x, str(int((ins[1] + 250) * s)), "900")
time.sleep(1.5)
check(state().get("sel") != held, f"a long press selects the row it holds ({held} -> {state().get('sel')})")
adb("shell", "input", "keyevent", "KEYCODE_HOME")
time.sleep(2)
adb("shell", "am", "start", "-n", ACT)
time.sleep(2.5)
frac = black_fraction(ins[1] * s, (lh - ins[3]) * s)
check(frac < 0.05, f"after HOME and relaunch the app repaints without input ({frac:.0%} of the app area black)")
adb("shell", "input", "tap", tab(2), str(int(nav_y)))
wait_for(lambda: state().get("nav") == 2, "settings")
time.sleep(1)
field_y = None
for yy in range(int((ins[1] + 150) * s), int((lh - ins[3] - 60) * s), int(10 * s)):
    adb("shell", "input", "tap", str(int(lw * 0.3 * s)), str(yy))
    time.sleep(0.8)
    if "mInputShown=true" in subprocess.run([ADB, "shell", "dumpsys", "input_method"], capture_output=True, text=True).stdout:
        field_y = yy
        break
check(field_y is not None, "a tap on the text field raises the on-screen keyboard")
time.sleep(1)
kb = [float(v) for v in re.search(r"insets ([-\d.,]+)$", last("screen")).group(1).split(",")]
check(kb[3] > ins[3] + 100, f"the keyboard becomes the bottom inset ({ins[3]} -> {kb[3]} pt)")
adb("shell", "input", "text", "Zed")
check(wait_for(lambda: state().get("name", "").endswith("Zed"), "ime"), f"typing through the IME reaches the field ({state().get('name')!r})")
adb("shell", "input", "tap", str(int(lw / 2 * s)), str(int((ins[1] + 120) * s)))
time.sleep(2)
back = [float(v) for v in re.search(r"insets ([-\d.,]+)$", last("screen")).group(1).split(",")]
check(back[3] == ins[3], f"tapping elsewhere hides the keyboard and restores the inset ({back[3]})")
print(f"android smoke: {fails} failure(s)")
sys.exit(1 if fails else 0)
