#!/usr/bin/env python3
"""probe_pty.py -- real-TTY probes: raw-mode restoration, typeahead survival
across keypress()->prompt(), terminal-not-left-raw. Uses a pty pair."""
import os, pty, subprocess, sys, time, select as osselect, termios, tty

BIN = os.environ.get("DYNAJS", "./dynajs")
fails = 0

def check(pid, desc, ok, detail=""):
    global fails
    print("%s %s -- %s%s" % ("PASS" if ok else "FAIL", pid, desc,
                            "" if ok else " :: " + detail))
    if not ok: fails += 1

def pty_run(script, feed, drain_sec=3.0, timeout=25, ready=None):
    """Run script with stdin/stdout on a pty; feed writes with interleaved
    draining (tcsetattr blocks until its prior output is consumed -- a harness
    that starves the master starves the child). With `ready`, the feed schedule
    starts only after the marker appears in the output: otherwise the child's
    startup time races the byte timing the scenarios are about."""
    mfd, sfd = pty.openpty()
    p = subprocess.Popen([BIN, "--std", script], stdin=sfd, stdout=sfd, stderr=sfd,
                         close_fds=True)
    os.close(sfd)
    out = b""
    def pump(sec):
        nonlocal out
        end = time.time() + sec
        while time.time() < end:
            r, _, _ = osselect.select([mfd], [], [], 0.05)
            if mfd in r:
                try:
                    d = os.read(mfd, 65536)
                except OSError:
                    return
                if not d:
                    return
                out += d
    if ready:
        deadline = time.time() + 15
        while time.time() < deadline and ready not in out:
            pump(0.1)
    for chunk, gap in feed:
        if chunk:
            try:
                os.write(mfd, chunk)
            except OSError:
                break
        pump(gap)
    pump(drain_sec)
    try:
        p.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        p.kill()
        out += b"<TIMEOUT>"
    pump(0.3)
    os.close(mfd)
    return out, p.returncode

# T1: keypress() must restore the terminal (echo+canonical back) -- the child
# checks the tty attrs AFTER keypress() itself.
open("tests/review_term/_pty_child1.js", "w").write("""
import { keypress, prompt } from "dyna:cli";
import * as std from "std";
std.out.puts("READY\\n");
std.out.flush();
const k = keypress();
print("K=" + JSON.stringify(k && k.name));
// after keypress the tty must be back to canonical+echo; the observable proof
// is the next LINE read: with echo+canonical on, the tty driver line-buffers
// and the whole line arrives at once.
const s = prompt("P:");
print("S=" + JSON.stringify(s));
""")
out, rc = pty_run("tests/review_term/_pty_child1.js",
                  [(b"", 0.3), (b"x", 0.3), (b"hello\n", 0.5)], drain_sec=3.0, timeout=10,
                  ready=b"READY")
text = out.decode("utf-8", "replace")
check("T1", "keypress() then prompt() on a pty: both work, terminal restored",
      ('K="x"' in text) and ('S="hello"' in text) and rc == 0, "out=%r rc=%s" % (out, rc))

# T2: input typed DURING the raw-mode window must survive the raw-mode
# teardown into the following prompt(). The READY marker guarantees the child
# is already blocked in keypress() (raw mode active) when the bytes arrive;
# then one write feeds "aburst\n": keypress takes 'a', prompt must get "burst".
# A flush on raw-mode EXIT discards "burst\n" and the prompt hangs.
out, rc = pty_run("tests/review_term/_pty_child1.js",
                  [(b"aburst\n", 0.0)], drain_sec=4.0, timeout=8, ready=b"READY")
text = out.decode("utf-8", "replace")
check("T2", "input queued in raw mode survives keypress()->prompt() (raw-off must not flush)",
      ('K="a"' in text) and ('S="burst"' in text), "out=%r rc=%s" % (out, rc))

# T2b: the same repro in its minimal shape -- the interleave must deliver the
# "X\\n" written while keypress() is pending to the prompt() that follows.
out, rc = pty_run("tests/review_term/_pty_child1.js",
                  [(b"aX\n", 0.0)], drain_sec=4.0, timeout=8, ready=b"READY")
text = out.decode("utf-8", "replace")
check("T2b", "the pty interleave delivers 'X\\n' to the prompt after the consumed key",
      ('K="a"' in text) and ('S="X"' in text), "out=%r rc=%s" % (out, rc))

# T3: terminal is NOT left in raw mode when select() cancels: after the child
# exits, the tty attrs must show ICANON+ECHO. Synchronize on the menu output
# so the cancel byte cannot race the raw-mode entry.
open("tests/review_term/_pty_child2.js", "w").write("""
import { select } from "dyna:cli";
const r = select("Pick:", ["a", "b"]);
print("R=" + JSON.stringify(r));
""")
mfd, sfd = pty.openpty()
before = termios.tcgetattr(mfd)
p = subprocess.Popen([BIN, "--std", "tests/review_term/_pty_child2.js"],
                     stdin=sfd, stdout=sfd, stderr=sfd, close_fds=True)
os.close(sfd)
# Keep DRAINING the master continuously: tcsetattr(TCSAFLUSH) inside the
# child's raw-mode entry BLOCKS until prior output is consumed, so a harness
# that stops reading starves the child (harness artifact, not the product).
buf = b""
end = time.time() + 5
while time.time() < end and b"Pick" not in buf:
    r, _, _ = osselect.select([mfd], [], [], 0.2)
    if mfd in r:
        try: buf += os.read(mfd, 4096)
        except OSError: break
menu_seen = b"Pick" in buf
time.sleep(0.4)
os.write(mfd, b"\x03")   # Ctrl-C cancels
end = time.time() + 6
while time.time() < end and p.poll() is None:
    r, _, _ = osselect.select([mfd], [], [], 0.1)
    if mfd in r:
        try: buf += os.read(mfd, 65536)
        except OSError: break
try:
    p.wait(timeout=5)
except subprocess.TimeoutExpired:
    p.kill()
    p.wait(timeout=5)
after = termios.tcgetattr(mfd)
os.close(mfd)
lf_before, lf_after = before[3], after[3]
check("T3", "select() Ctrl-C leaves ICANON+ECHO restored on the tty",
      (lf_after & termios.ICANON) and (lf_after & termios.ECHO) and p.returncode == 0,
      "menu_seen=%s lflag before=%x after=%x rc=%s out=%r" %
      (menu_seen, lf_before, lf_after, p.returncode, buf[:200]))

# T4: EOF (master closed) must not hang keypress: close master promptly.
mfd, sfd = pty.openpty()
p = subprocess.Popen([BIN, "--std", "tests/review_term/probe_keys.js"],
                     stdin=sfd, stdout=sfd, stderr=sfd, close_fds=True)
os.close(sfd)
os.close(mfd)   # EOF on the pty slave
try:
    p.wait(timeout=10)
    check("T4", "pty EOF: keypress() returns null and the process exits", p.returncode == 0,
          "rc=%s" % p.returncode)
except subprocess.TimeoutExpired:
    p.kill()
    check("T4", "pty EOF: keypress() returns null and the process exits", False, "HANG")

print("probe_pty: done")
sys.exit(1 if fails else 0)
