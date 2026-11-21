// timeout: 600
// covers: integration c:src/builtins/builtin-array.c c:src/builtins/builtin-regexp.c c:src/vm/interpreter.inc.c -- callback-free native loops honor --timeout-ms, each form in its own child process
// tests/core/test_audit_budget_1.js -- E3-01: callback-free native loops must honor
// --timeout-ms (regression for the alarm-kill bypass). Each form is run in a
// child dynajs so the deadline can be armed per form.
// build-note: needs dyna:sys (build with CONFIG_NATIVE_MODULES=y)
import { sections as __kitSections } from "../lib/kit.js";
import { Exec } from "dyna:sys";
const __kitS = __kitSections("test_audit_budget_1", { chain: true });

const BIN = scriptArgs[1] || "./dynajs";
const HERE = scriptArgs[2] || ".";
let failures = 0;

function assert(cond, msg) {
    if (!cond) {
        failures++;
        print("  FAIL:", msg);
    }
}

const FORMS = [
    // "" because a non-empty separator over 2^32-1 slots is refused before the
    // walk (the result cannot fit a string); "" has no such bound, so it walks.
    // join skips hole runs when no prototype has indexed properties, so an
    // indexed Array.prototype element forces the per-index walk that must stay
    // interruptible.
    ["join", "Array.prototype[7] = \"\"; new Array(2**32-1).join(\"\")"],
    ["fill", "new Array(2**32-1).fill(0)"],
    ["indexOf", "new Array(2**32-1).indexOf(1)"],
    ["includes", "new Array(2**32-1).includes(1)"],
    ["lastIndexOf", "new Array(2**32-1).lastIndexOf(1)"],
    ["reverse", "new Array(2**32-1).reverse()"],
    ["sort", "new Array(2**32-1).sort()"],
    ["sum", "Array.prototype.sum.call({length:2**53-1})"],
    ["count", "Array.prototype.count.call({length:2**53-1}, 1)"],
    ["unique", "Array.prototype.unique.call({length:2**53-1})"],
    ["json-replacer", "JSON.stringify({}, new Array(2**32-1))"],
    ["json-array", "JSON.stringify(new Array(2**32-1))"],
    ["replaceAll", 'let s="a".repeat(2**29); s.replaceAll("a","bb")'],
    ["build-range", "(1e8).times()"],
];

__kitS.section("child exited 0 with no interrupt", () => {
for (const [name, form] of FORMS) {
    const t0 = performance.now();
    const r = Exec(BIN, ["--timeout-ms", "300", "-e", form], {
        timeoutMs: 20000,
        encoding: "utf8",
    });
    const ms = performance.now() - t0;
    const out = r.stdout + r.stderr;
    assert(r.code !== 0 || r.signal !== null,
        name + ": child exited 0 with no interrupt (out=" + out.slice(0, 80) + ")");
    assert(/interrupt/i.test(out),
        name + ": no interrupted error (out=" + out.slice(0, 80) + ")");
    // wall-clock: a deadline/timeout check; the bound sits far above the deadline and below the hang it guards against, so load only narrows the gap
    assert(!r.timedOut && ms < 10000,
        name + ": took " + (ms | 0) + "ms, deadline not enforced promptly");
    print("  ok  " + name + " interrupted in " + (ms | 0) + "ms");
}
});

// The interpreter loop form must still work (control the suite shares with
// test_exec_timeout).
{
let r;
__kitS.section("for(;;) control interrupted", () => {
    r = Exec(BIN, ["--timeout-ms", "300", "-e", "for(;;){}"], {
        timeoutMs: 20000,
        encoding: "utf8",
    });
    assert(/interrupt/i.test(r.stderr + r.stdout), "for(;;) control interrupted");
    print("  ok  for(;;) control");
});
}
__kitS.end();
if (!__kitS.listing) {

if (failures) {
    print("test_audit_budget_1: " + failures + " FAILURES");
    throw new Error("test_audit_budget_1 failed");
}
print("test_audit_budget_1: all tests passed");
}
