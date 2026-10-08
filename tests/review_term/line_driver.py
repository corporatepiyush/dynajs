#!/usr/bin/env python3
"""line_driver.py -- prompt()/confirm() 1 MiB cap + stream desync probes."""
import subprocess, sys, os

BIN = os.environ.get("DYNAJS", "./dynajs")
MIB = 1 << 20

def run(mode, payload, timeout=30):
    p = subprocess.Popen([BIN, "--std", "tests/review_term/probe_line.js", mode],
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
    return [l for l in out.decode("utf-8", "replace").splitlines()
            if ("A1=" in l or "A2=" in l)]

def grab(lines, tag):
    """prompt() writes its message without a newline, so the A1 line may be
    glued to message bytes. Extract from the tag onward (through JSON string)."""
    for l in lines:
        i = l.find(tag)
        if i >= 0:
            return l[i:]
    return "<missing>"

fails = 0
def check(pid, desc, ok, detail=""):
    global fails
    print("%s %s -- %s%s" % ("PASS" if ok else "FAIL", pid, desc,
                            "" if ok else " :: " + detail))
    if not ok: fails += 1

got = run("prompt", b"x" * (2 * MIB) + b"\nsecond\n")
a1, a2 = grab(got, "A1="), grab(got, "A2=")
check("P21", "2 MiB line: RangeError, next prompt still reads 'second' (no desync)",
      a1 == 'A1="E:RangeError"' and a2 == 'A2="R:second"', "A1=%r A2=%r raw=%r" % (a1, a2, got))

got = run("confirm", b"y" * (2 * MIB) + b"\nyes\n")
a1, a2 = grab(got, "A1="), grab(got, "A2=")
check("P22", "confirm: 2 MiB line RangeError then aligned 'yes' -> true",
      a1 == 'A1="E:RangeError/R:true"' and a2 == 'A2="R:false"', "A1=%r A2=%r raw=%r" % (a1, a2, got))

got = run("cap-exact", b"a" * MIB + b"\nsecond\n")
a1, a2 = grab(got, "A1="), grab(got, "A2=")
check("P23", "exactly 1 MiB + LF accepted at the boundary, stream aligned",
      a1 == 'A1="R-len:%d"' % MIB and a2 == 'A2="R:second"', "A1=%r A2=%r raw=%r" % (a1, a2, got))

got = run("cap-crlf", b"a" * MIB + b"\r\n")
a1 = grab(got, "A1=")
ok_len = a1 == 'A1="R-len:%d"' % MIB
check("P24", "1 MiB + CRLF accepted as 1 MiB of content (CR dropped)",
      ok_len, "A1=%r (if E:RangeError the CR counts against the cap -- boundary inconsistency with P23)" % (a1,))

print("line_driver: done")
sys.exit(1 if fails else 0)
