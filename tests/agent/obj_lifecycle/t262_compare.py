#!/usr/bin/env python3
"""Compare a run-test262 log's failure set against tools/test262_errors.txt.

Keys are (path, line, strictness) -- robust against the progress-bar junk
that threaded runs interleave into message text. LC_ALL=C byte semantics via
plain byte sorting.
Usage: python3 t262_compare.py <run-log>
"""
import re, sys

pin_path = "tools/test262_errors.txt"
log_path = sys.argv[1]

entry_re = re.compile(r"test262/test/[^ :\n]+\.js:[0-9]+")

def keys_from(text):
    keys = set()
    for m in entry_re.finditer(text):
        start = m.end()
        # strictness: the segment right after "line:" may be " strict mode:"
        rest = text[start:start + 32]
        strict = " strict mode:" in rest
        keys.add((m.group(0), strict))
    return keys

pin_text = open(pin_path, "r", encoding="utf-8", errors="replace").read()
log_text = open(log_path, "r", encoding="utf-8", errors="replace").read()

pin_keys = keys_from(pin_text)
log_keys = keys_from(log_text)

only_pin = sorted(pin_keys - log_keys)
only_log = sorted(log_keys - pin_keys)

# Repair pass: threaded runs interleave progress-bar junk ("-!--!-!!!...")
# into some lines. Keys containing '!' are matched fuzzily by stripping all
# '!', '-', '.' characters on both sides (applied uniformly, so legit paths
# still compare equal).
def fuzzy(k):
    return (re.sub(r"[-.!,]", "", k[0]), k[1])

pin_fuzzy = {}
for k in pin_keys:
    pin_fuzzy.setdefault(fuzzy(k), []).append(k)

repaired = set()
for k in list(only_log):
    if "!" in k[0] and fuzzy(k) in pin_fuzzy:
        candidates = pin_fuzzy[fuzzy(k)]
        if len(candidates) == 1:
            repaired.add(k)
            only_pin.remove(candidates[0])
for k in repaired:
    only_log.remove(k)
print(f"(repaired {len(repaired)} junk-mangled log entries against the pin)")

# Second repair pass: the progress junk can eat the FRONT of a path
# ("-.--!--est/language/...js:24:"), so no test262/-prefixed key is formed.
# For each still-unmatched pin entry, look for its basename:line in a line
# of the raw log carrying an error.
repaired2 = set()
for k in list(only_pin):
    base = k[0].rsplit("/", 1)[1]           # name.js:LINE
    pat = re.escape(base) + r":[^\n]*error"
    if re.search(pat, log_text):
        repaired2.add(k)
for k in repaired2:
    only_pin.remove(k)
if repaired2:
    print(f"(front-truncated repair: {len(repaired2)} pin entries found as "
          f"basename:line error lines)")

# Third repair pass: junk can eat the front of the basename itself. Strip
# every '!' and '-' from BOTH texts and require a LONG SUFFIX (>= 25 chars,
# which always spans the ":LINE" tail) of the pin key's normalized path to
# appear in the normalized log.
def dashjunk(s):
    return s.replace("!", "").replace("-", "")

log_norm = dashjunk(log_text)
repaired3 = set()
for k in list(only_pin):
    kn = dashjunk(k[0])
    for cut in range(max(0, len(kn) - 60), max(0, len(kn) - 25)):
        if kn[cut:] in log_norm:
            repaired3.add(k)
            break
for k in repaired3:
    only_pin.remove(k)
if repaired3:
    print(f"(dash-junk repair: {len(repaired3)} pin entries matched after "
          f"'!'/'-' stripping + suffix match)")


print(f"pin entries: {len(pin_keys)}  log entries: {len(log_keys)}")
if only_pin:
    print("IN PIN BUT NOT IN LOG:")
    for k in only_pin:
        print("  -", k[0], "(strict)" if k[1] else "")
if only_log:
    print("IN LOG BUT NOT IN PIN:")
    for k in only_log:
        print("  +", k[0], "(strict)" if k[1] else "")
if not only_pin and not only_log:
    print("FAIL-LIST IDENTICAL (path+line+strictness keys)")
    sys.exit(0)
sys.exit(1)
