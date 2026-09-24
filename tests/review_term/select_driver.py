#!/usr/bin/env python3
"""select_driver.py -- select() navigation/cancel/refusal probes."""
import subprocess, sys, os, time

BIN = os.environ.get("DYNAJS", "./dynajs")

def run(mode, payload, timeout=30):
    p = subprocess.Popen([BIN, "--std", "tests/review_term/probe_select.js", mode],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT)
    try:
        p.stdin.write(payload)
        p.stdin.close()
    except BrokenPipeError:
        pass
    try:
        out = p.stdout.read()
        p.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        p.kill()
        return ["<TIMEOUT>"]
    return [l for l in out.decode("utf-8", "replace").splitlines() if l.startswith(("A=", "<TIMEOUT>"))]

fails = 0
def check(pid, desc, ok, detail=""):
    global fails
    print("%s %s -- %s%s" % ("PASS" if ok else "FAIL", pid, desc,
                            "" if ok else " :: " + detail))
    if not ok: fails += 1

# S1: down down enter -> "three"
got = run("three", b"\x1b[B\x1b[B\r")
check("S1", "down x2 + enter picks 'three'", got == ['A="three"'], repr(got))

# S2: up from 0 wraps to the last
got = run("three", b"\x1b[A\r")
check("S2", "up from the top wraps to the last option", got == ['A="three"'], repr(got))

# S3: bare ESC cancels to null
got = run("three", b"\x1b")
check("S3", "bare Escape cancels -> null", got == ["A=null"], repr(got))

# S4: Ctrl-C cancels to null
got = run("three", b"\x03")
check("S4", "Ctrl-C cancels -> null", got == ["A=null"], repr(got))

# S5: EOF cancels to null
got = run("three", b"")
check("S5", "EOF cancels -> null", got == ["A=null"], repr(got))

# S6: junk keys ignored, enter still picks the first
got = run("three", b"xy\x1b[Zq\r")
check("S6", "unknown keys are ignored (incl shift+tab), enter picks top",
      got == ['A="one"'], repr(got))

# S7: ESC + enter within the window = meta+enter: must be IGNORED (enter with
# meta is not plain enter) and EOF then cancels.
got = run("three", b"\x1b\r")
check("S7", "ESC-enter (meta+enter) is ignored, not a selection", got == ["A=null"], repr(got))

# S8-S10: non-string options must REFUSE (not stringify)
got = run("nonstring-num", b"\r")
check("S8", "option 42 refused (TypeError naming the index), not stringified",
      len(got) == 1 and got[0].startswith("A=TypeError") and "option 1" in got[0], repr(got))
got = run("nonstring-null", b"\r")
check("S9", "option null refused (TypeError)", len(got) == 1 and got[0].startswith("A=TypeError"), repr(got))
got = run("nonstring-obj", b"\r")
check("S10", "option {} refused (TypeError)", len(got) == 1 and got[0].startswith("A=TypeError"), repr(got))

# S11: empty array refused
got = run("empty", b"\r")
check("S11", "empty options refused", len(got) == 1 and got[0].startswith("A=TypeError"), repr(got))

# S12: 65537 refused, 65536 allowed
got = run("toobig", b"\r")
check("S12", "65537 options -> RangeError", len(got) == 1 and got[0].startswith("A=RangeError"), repr(got))
got = run("max", b"\r", timeout=60)
check("S13", "65536 options at the documented max works", got == ['A="q"'], repr(got))

# S14: getter-mutated array: answer must be the snapshot string
got = run("getter-snapshot", b"\x1b[B\r")
check("S14", "menu answers from the snapshot, not a live getter",
      len(got) == 1 and got[0] == 'A="second" reads=1', repr(got))

print("select_driver: done")
sys.exit(1 if fails else 0)
