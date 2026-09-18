#!/usr/bin/env python3
"""tools/check-dts-truth.py -- T5 from the security plan.

Locks the dynajs.d.ts signatures that were once WRONG (audit C4) to the
RUNTIME's actual behaviour, using two independent oracles:

  1. d.ts text assertions — the declarations must say what the binary does.
  2. runtime probes       — ./dynajs executes each contested call the way a
     user transpiled from the d.ts would write it.

Run: python3 tools/check-dts-truth.py [--dynajs PATH]
Exit 0 = truths hold; nonzero = drift (fix the d.ts, or fix the runtime).
"""
import subprocess
import sys

DTS = "dynajs.d.ts"
FAILURES = []

# ---- oracle 1: d.ts text must match runtime truth ------------------------
DTS_MUST_CONTAIN = [
    # (what, needle) — each needle is the CORRECT signature fragment
    ("Negotiate returns the candidate string",
     "function Negotiate(header: string, candidates: string[]): string | null;"),
    ("Object.pick runtime arg order is (keys, obj)",
     "pick(keys: string[], obj: object)"),
    ("Object.pickAll runtime arg order is (keys, obj)",
     "pickAll(keys: string[], obj: object)"),
    ("Object.omit runtime arg order is (keys, obj)",
     "omit(keys: string[], obj: object)"),
    ("String.splitN separator is a plain string",
     "splitN(sep: string, n: number): string[];"),
    ("Command.parse argv is optional",
     "parse(args?: string[]): unknown;"),
    ("Number.range returns a real array",
     "range(start: number, end?: number, step?: number): number[];"),
]

# ---- oracle 2: the runtime must actually behave that way -----------------
PROBES = r'''
import { Negotiate } from "dyna:http";
import { Object as _O } from "none";  /* deliberate fail-free import guard */
'''


def check_dts():
    src = open(DTS, encoding="utf-8", errors="replace").read()
    for what, needle in DTS_MUST_CONTAIN:
        if needle not in src:
            FAILURES.append("d.ts: %s -- missing %r" % (what, needle))


def check_runtime(dynajs):
    probe = """
import { Negotiate } from "dyna:http";
const out = [];
out.push(["Negotiate-string", Negotiate("text/html;q=1, text/plain;q=0.5", ["text/plain","text/html"]) === "text/html"]);
out.push(["pick-keys-obj", JSON.stringify(Object.pick(["a"], { a: 1, b: 2 })) === '{"a":1}']);
out.push(["splitN-string-sep", "a:b:c".splitN(":", 2)[1] === "b:c"]);
out.push(["range-is-array", Array.isArray(Number.range(1, 3))]);
out.push(["Command-parse-optional", (new (await import("dyna:cli")).Command("t")).parse() !== undefined]);
let bad = 0;
for (const [n, ok] of out) if (!ok) { bad++; console.log("RUNTIME-DRIFT: " + n); }
console.log("runtime-probes: " + (out.length - bad) + "/" + out.length);
if (bad) throw new Error("drift");
"""
    import tempfile, os
    fd, path = tempfile.mkstemp(suffix=".mjs")
    with os.fdopen(fd, "w") as fh:
        fh.write(probe)
    try:
        p = subprocess.run([dynajs, path], capture_output=True, text=True,
                           timeout=30)
        if p.returncode != 0:
            FAILURES.append("runtime probes failed: %s%s"
                            % (p.stdout[-300:], p.stderr[-300:]))
        elif "runtime-probes: 5/5" not in p.stdout + p.stderr:
            FAILURES.append("runtime probes incomplete: %r"
                            % ((p.stdout + p.stderr)[-200:],))
    finally:
        os.unlink(path)


def main():
    dynajs = "./dynajs"
    if len(sys.argv) > 2 and sys.argv[1] == "--dynajs":
        dynajs = sys.argv[2]
    check_dts()
    check_runtime(dynajs)
    if FAILURES:
        for f in FAILURES:
            print("FAIL " + f)
        return 1
    print("check-dts-truth: %d signatures + %d runtime probes all hold"
          % (len(DTS_MUST_CONTAIN), 5))
    return 0


if __name__ == "__main__":
    sys.exit(main())
