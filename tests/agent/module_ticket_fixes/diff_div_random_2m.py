#!/usr/bin/env python3
# diff_div_random_2m.py -- 2,000,000-case random differential for dyna:decimal
# div (the FIX 1 guard-digit bug: sticky round-up landed on the LSD after
# dec_trim folded the quotient's low zeros into the exponent).
#
# Seeded operand generator (repeated-digit-biased, the bug's favourite shape),
# python decimal oracle under the identical context, dynajs runs baked batches
# and prints one canonical result per line; any mismatch is reported verbatim.
#
# Usage: python3 diff_div_random_2m.py [path-to-dynajs] [N]
import os, sys, json, random, subprocess, tempfile
from decimal import Decimal, getcontext, localcontext
import decimal as _d

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_BIN = os.path.join(HERE, "..", "..", "..", "dynajs")

getcontext().prec = 200000

MODES = ["up", "down", "ceil", "floor", "halfUp", "halfDown", "halfEven"]
PYMODE = {
    "up": _d.ROUND_UP, "down": _d.ROUND_DOWN, "ceil": _d.ROUND_CEILING,
    "floor": _d.ROUND_FLOOR, "halfUp": _d.ROUND_HALF_UP,
    "halfDown": _d.ROUND_HALF_DOWN, "halfEven": _d.ROUND_HALF_EVEN,
}
PRECS = [2, 3, 9, 17, 34, 50, 100]
BATCH = 100000

def rep_digits(rng):
    """A repeated-digit number: digit runs, optional fraction, optional
    exponent, optional sign -- the expansion class behind the bug."""
    d1 = str(rng.randrange(10)) if rng.random() < 0.9 else str(rng.randrange(1, 10))
    run1 = rng.randrange(1, 21)
    ip = d1 * run1
    s = ip
    if rng.random() < 0.6:
        d2 = str(rng.randrange(10))
        run2 = rng.randrange(1, 13)
        s += "." + d2 * run2
    if rng.random() < 0.3:
        s += "e" + str(rng.randrange(-12, 13))
    if rng.random() < 0.4:
        s = "-" + s
    # keep it canonical enough for Decimal()
    d = Decimal(s)
    return str(d) if rng.random() < 0.5 else s

def canond(d):
    if not d.is_finite():
        raise ValueError("non-finite")
    if d == 0:
        return Decimal(0)
    with localcontext() as c:
        c.prec = 200000
        return d.normalize()

def dyna_str(d):
    d = canond(d)
    if d == 0:
        return "0"
    sign, digits, exp = d.as_tuple()
    s = "".join(map(str, digits))
    ndig = len(s)
    point = ndig + exp
    if point <= 0:
        out = "0." + "0" * (-point) + s
    elif point < ndig:
        out = s[:point] + "." + s[point:]
    else:
        out = s + "0" * (point - ndig)
    return ("-" if sign else "") + out

def make_case(rng):
    a = rep_digits(rng)
    b = rep_digits(rng)
    if Decimal(b) == 0:
        b = "3"
    prec = rng.choice(PRECS)
    mode = rng.choice(MODES)
    return a, b, prec, mode

PROBE_TMPL = """import { Decimal } from "dyna:decimal";
var M = %s;
var out = [];
for (var i = 0; i < M.length; i++) {
  var c = M[i];
  try { out.push(new Decimal(c[0]).div(c[1], { precision: c[2], rounding: c[3] }).toString()); }
  catch (e) { out.push("THROW:" + ((e && e.constructor && e.constructor.name) || e)); }
}
print(out.join("\\n"));
"""

def main():
    bin_ = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BIN
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 2000000
    seed = 0xD1A   # deterministic: every re-run replays the same 2M cases
    rng = random.Random(seed)
    tmp = tempfile.mkdtemp(prefix="d2m_", dir=os.path.join(HERE, "scratch"))
    os.makedirs(tmp, exist_ok=True)
    bad = 0
    done = 0
    batch_no = 0
    while done < n:
        cnt = min(BATCH, n - done)
        cases = [make_case(rng) for _ in range(cnt)]
        # oracle
        want = []
        for a, b, prec, mode in cases:
            with localcontext() as c:
                c.prec = prec
                c.rounding = PYMODE[mode]
                q = Decimal(a) / Decimal(b)
            want.append(dyna_str(q))
        # engine
        pj = os.path.join(tmp, "b%04d.js" % batch_no)
        with open(pj, "w") as f:
            f.write(PROBE_TMPL % json.dumps([list(c) for c in cases]))
        r = subprocess.run([bin_, pj], capture_output=True, text=True, timeout=600)
        if r.returncode != 0:
            print("ENGINE FAIL batch %d rc=%d: %s" % (batch_no, r.returncode, r.stderr[:400]))
            bad += cnt
        else:
            got = r.stdout.splitlines()
            if len(got) != cnt:
                print("LINE COUNT MISMATCH batch %d: %d vs %d" % (batch_no, len(got), cnt))
                bad += cnt
            for i in range(cnt):
                if got[i] != want[i]:
                    if bad < 20:
                        print("MISMATCH div(%s/%s, p%d, %s): dyna=%s oracle=%s"
                              % (cases[i][0], cases[i][1], cases[i][2], cases[i][3], got[i], want[i]))
                    bad += 1
        os.unlink(pj)
        done += cnt
        batch_no += 1
        print("  ... %d/%d done, %d mismatches" % (done, n, bad), flush=True)
    os.rmdir(tmp) if not os.listdir(tmp) else None
    print("RESULT %s: %d cases, %d mismatches" % ("PASS" if bad == 0 else "FAIL", done, bad))
    sys.exit(0 if bad == 0 else 1)

if __name__ == "__main__":
    main()
