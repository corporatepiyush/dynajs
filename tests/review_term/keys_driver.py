#!/usr/bin/env python3
"""keys_driver.py -- adversarial key-decoder scenarios with owned byte timing.
Each scenario: list of (bytes, sleep_after_sec) chunks written to the child's
stdin, expected decoded event list. Prints PASS/FAIL per scenario (each one is
a probe). The driver synchronizes on the child's flushed READY marker before
the first write, so a slow child start cannot coalesce scenario bytes that
must arrive split (the scenarios are about arrival gaps, not about startup)."""
import os, select, subprocess, sys, time

BIN = os.environ.get("DYNAJS", "./dynajs")

def run(chunks, timeout=20):
    p = subprocess.Popen([BIN, "--std", "tests/review_term/probe_keys.js"],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT)
    out = b""
    try:
        # sync: wait for READY (or child exit) before any byte is fed
        deadline = time.time() + 15
        while time.time() < deadline and b"READY\n" not in out:
            r, _, _ = select.select([p.stdout], [], [], 0.1)
            if p.stdout in r:
                d = os.read(p.stdout.fileno(), 65536)
                if not d:
                    break
                out += d
            elif p.poll() is not None:
                break
        for data, gap in chunks:
            if data:
                p.stdin.write(data)
                p.stdin.flush()
            if gap:
                time.sleep(gap)
        p.stdin.close()
    except BrokenPipeError:
        pass
    try:
        out += p.stdout.read()
        p.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        p.kill()
        return ["<TIMEOUT>"]
    return [l for l in out.decode("utf-8", "replace").splitlines() if l.startswith(("E|", "EOF", "<TIMEOUT>"))]

def ev(name, hexseq, cms):
    return "E|%s|%s|%s" % (name, hexseq, cms)

SCN = []  # (probe-id, description, chunks, expected)

# K1: CSI split byte-by-byte with >50ms gaps: each byte becomes its own event.
SCN.append(("K1", "ESC [ A split 60ms apart degrades to 3 events (timeout window)",
    [(b"\x1b", .2), (b"[", .2), (b"A", .1)],
    [ev("escape", "1b,", "000"), ev("[", "5b,", "000"), ev("A", "41,", "000"), "EOF"]))

# K2: same 3 bytes delivered fast must be ONE up-arrow event.
SCN.append(("K2", "ESC [ A fast decodes as one 'up'",
    [(b"\x1b[A", .05)],
    [ev("up", "1b,5b,41,", "000"), "EOF"]))

# K3: split mid-CSI params: truncated event (name null) + leftovers as chars.
SCN.append(("K3", "ESC [ 1 ; split from 5 ~ : truncated event, leftovers decode as chars",
    [(b"\x1b[1;", .2), (b"5~", .1)],
    [ev("<null>", "1b,5b,31,3b,", "000"), ev("5", "35,", "000"), ev("~", "7e,", "000"), "EOF"]))

# K4: ESC ESC alone: bare escape with meta.
SCN.append(("K4", "ESC ESC is escape with meta",
    [(b"\x1b\x1b", .06)],
    [ev("escape", "1b,1b,", "010"), "EOF"]))

# K5: ESC ESC A: meta chord on the char.
SCN.append(("K5", "ESC ESC A is meta+A",
    [(b"\x1b\x1bA", .05)],
    [ev("A", "1b,1b,41,", "010"), "EOF"]))

# K6: bare ESC then a late byte: escape first, then the char.
SCN.append(("K6", "bare ESC times out to 'escape', late q is its own key",
    [(b"\x1b", .2), (b"q", .1)],
    [ev("escape", "1b,", "000"), ev("q", "71,", "000"), "EOF"]))

# K7: ESC [ A delivered at 30ms gaps: still inside the 50ms window.
SCN.append(("K7", "ESC [ A split 30ms apart stays one 'up' event",
    [(b"\x1b", .02), (b"[", .02), (b"A", .1)],
    [ev("up", "1b,5b,41,", "000"), "EOF"]))

# K8: CSI with garbage parameter bytes (0x30-0x3f soup).
SCN.append(("K8", "CSI 9:;<=>?~ garbage params: one event, name null, then clean stream",
    [(b"\x1b[9:;<=>?~", .05), (b"z", .05)],
    [ev("<null>", "1b,5b,39,3a,3b,3c,3d,3e,3f,7e,", "000"), ev("z", "7a,", "000"), "EOF"]))

# K9: very long CSI string: param buffer caps at 11; watch consumed-vs-sequence.
SCN.append(("K9", "CSI with 30 param digits: params overflow spares no crash; stream spills",
    [(b"\x1b[" + b"1" * 30 + b"~", .05)],
    ["SPLILL"]))   # special expectation, checked below

# K10: P/Q/R/S (F1-F4) are unknown names but clean events.
SCN.append(("K10", "SS3 P/Q/R/S decode to name-null events with full sequence",
    [(b"\x1bOP\x1bOQ\x1bOR\x1bOS", .05)],
    [ev("<null>", "1b,4f,50,", "000"), ev("<null>", "1b,4f,51,", "000"),
     ev("<null>", "1b,4f,52,", "000"), ev("<null>", "1b,4f,53,", "000"), "EOF"]))

# K11: Ctrl-C / Ctrl-D / Ctrl-Z arrive as ctrl chords, no signals, no EOF.
SCN.append(("K11", "Ctrl-C/D/Z are ctrl chords c/d/z (ISIG off), never EOF",
    [(b"\x03\x04\x1a", .05)],
    [ev("c", "3,", "100"), ev("d", "4,", "100"), ev("z", "1a,", "100"), "EOF"]))

# K12: valid UTF-8 char in one read.
SCN.append(("K12", "e-acute (c3 a9) decodes to one key named U+00E9",
    [(b"\xc3\xa9", .05)],
    [ev("\u00e9", "e9,", "000"), "EOF"]))

# K13: stray continuation byte: name null, sequence must appear as U+FFFD.
SCN.append(("K13", "lone 0x80: name null, sequence is U+FFFD (d.ts claim)",
    [(b"\x80", .05)],
    [ev("<null>", "fffd,", "000"), "EOF"]))

# K14a: truncated UTF-8 lead at EOF.
SCN.append(("K14a", "truncated c3 at EOF: name null, sequence U+FFFD",
    [(b"\xc3", .05)],
    [ev("<null>", "fffd,", "000"), "EOF"]))

# K14b: c3 + 'A' fast: 'A' is NOT a continuation byte. Does it survive?
SCN.append(("K14b", "c3 then non-continuation A within the window: A must not vanish",
    [(b"\xc3A", .05)],
    ["SWALLOW"]))  # special expectation

# K14c: c3 + 'A' slow: A arrives after the window, must be its own key.
SCN.append(("K14c", "c3 then late A: two events",
    [(b"\xc3", .2), (b"A", .1)],
    [ev("<null>", "fffd,", "000"), ev("A", "41,", "000"), "EOF"]))

# K15: 4-byte emoji one event.
SCN.append(("K15", "4-byte emoji is one key, name is the character",
    [(b"\xf0\x9f\x98\x80", .05)],
    [ev("\U0001f600", "1f600,", "000"), "EOF"]))

# K16: modifier encodings: shift+tab, ctrl+right, page up, home/end, delete.
SCN.append(("K16", "CSI modifier encodings: Z=shift+tab, 1;5C=ctrl+right, 5~/6~=pages, 1~/4~/3~",
    [(b"\x1b[Z\x1b[1;5C\x1b[5~\x1b[6~\x1b[1~\x1b[4~\x1b[3~\x1b[H\x1bOF", .05)],
    [ev("tab", "1b,5b,5a,", "001"), ev("right", "1b,5b,31,3b,35,43,", "100"),
     ev("pageup", "1b,5b,35,7e,", "000"), ev("pagedown", "1b,5b,36,7e,", "000"),
     ev("home", "1b,5b,31,7e,", "000"), ev("end", "1b,5b,34,7e,", "000"),
     ev("delete", "1b,5b,33,7e,", "000"), ev("home", "1b,5b,48,", "000"),
     ev("end", "1b,4f,46,", "000"), "EOF"]))

# K17: meta+CSI: ESC ESC [ A must be meta+up in ONE event.
SCN.append(("K17", "ESC ESC [ A is meta+up, one event, 4-byte sequence",
    [(b"\x1b\x1b[A", .05)],
    [ev("up", "1b,1b,5b,41,", "010"), "EOF"]))

# K18: empty stdin: immediate null, no hang.
SCN.append(("K18", "empty stdin: keypress() is null at once",
    [(b"", .05)],
    ["EOF"]))

# K19: an ESC-run past the 15-byte sequence cap: the tail is DELIVERED as the
# next event(s) instead of being consumed past what sequence reports.
SCN.append(("K19", "20-byte ESC run: 15 + 5 delivered, 20 bytes consumed 20 reported",
    [(b"\x1b" * 20, .05)],
    [ev("escape", "1b," * 15, "010"), ev("escape", "1b," * 5, "010"), "EOF"]))

# K20: CSI param overflow split across reads (in-window gaps): decoding may
# depend on arrival timing only through the 50 ms window, so a split that
# stays inside it must spill EXACTLY like the single write.
SCN.append(("K20", "CSI param-overflow spill is identical whether one write or in-window split",
    [(b"\x1b[" + b"1" * 13, .02), (b"1" * 10, .02), (b"1" * 7 + b"~", .05)],
    ["SPLILL2"]))   # special expectation, checked below (compared to K9's shape)

# K21: a meta chord split in-window is still one event (the same bytes, the
# same decode -- read boundaries do not exist for the decoder).
SCN.append(("K21", "ESC ESC [ A split in-window is still one meta+up event",
    [(b"\x1b\x1b", .02), (b"[A", .05)],
    [ev("up", "1b,1b,5b,41,", "010"), "EOF"]))

fails = 0
for pid, desc, chunks, expect in SCN:
    got = run(chunks)
    if expect == ["SPLILL"] or expect == ["SPLILL2"]:
        # K9/K20: the exact decoder contract -- params cap 11, seq cap 15. One
        # name-null event (ESC [ + 12 param digits: 11 buffered, the 12th ends
        # the sequence), then the un-consumed tail spills as its own keys.
        ok = (len(got) == 21 and got[0].startswith("E|<null>|") and
              all(g == ev("1", "31,", "000") for g in got[1:19]) and
              got[19] == ev("~", "7e,", "000") and got[20] == "EOF")
        detail = "got %d events, first=%r" % (len(got), got[0] if got else None)
    elif expect == ["SWALLOW"]:
        # K14b: contract says sequence is "the input the event consumed".
        # 2 bytes went in. Correct: one U+FFFD event consuming c3, then 'A'.
        want = [ev("<null>", "fffd,", "000"), ev("A", "41,", "000"), "EOF"]
        ok = (got == want)
        if not ok:
            alt = [ev("<null>", "fffd,", "000"), "EOF"]
            if got == alt:
                detail = "BYTE-SWALLOWED: 'A' consumed but neither delivered nor reported (want %r got %r)" % (want, got)
            else:
                detail = "want %r got %r" % (want, got)
    else:
        ok = (got == expect)
        detail = "want %r got %r" % (expect, got)
    print("%s %s -- %s%s" % ("PASS" if ok else "FAIL", pid, desc,
                            "" if ok else " :: " + detail))
    if not ok:
        fails += 1
print("keys_driver: %d/%d scenarios ok" % (len(SCN) - fails, len(SCN)))
sys.exit(1 if fails else 0)
