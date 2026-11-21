// covers: c:src/libregexp.c
// tests/core/test_regexp_no_step_limit.js -- a regular expression has no step
// limit of its own (B10, SEC-14): the old 100M-step default killed slow but
// finite matches with an uncatchable "interrupted", indistinguishable from
// --timeout-ms. Now a slow match runs to its answer, and --timeout-ms is the
// one bound on catastrophic backtracking.
// build-note: needs dyna:sys (build with CONFIG_NATIVE_MODULES=y)
import { Exec } from "dyna:sys";
import { sections } from "../lib/kit.js";

const BIN = scriptArgs[1] || "./dynajs";
let n = 0, failures = 0;
function assert(cond, msg) { n++; if (!cond) { failures++; print("  FAIL:", msg); } }

const S = sections("test_regexp_no_step_limit");
S.section("a slow match past the old step budget returns its answer", () => {
    // 2^25 backtracking paths: past the old budget (it threw at 25 characters,
    // about 1 s), and a finite, well-defined answer.
    const t0 = performance.now();
    assert(/(a+)+$/.test("a".repeat(25) + "b") === false, "a slow match past the old step budget returns its answer");
    print("  25-character catastrophic match took " + Math.round(performance.now() - t0) + " ms");
});

S.section("a catastrophic match under --timeout-ms is stopped near the deadline, uncatchably", () => {
    // 2^40 paths: hours of work. --timeout-ms must end it near the deadline,
    // uncatchably, with a nonzero exit.
    const js = "try { /(a+)+$/.test('a'.repeat(40) + 'b'); print('finished'); } catch (e) { print('caught ' + e); }";
    const t0 = performance.now();
    const r = Exec(BIN, ["--timeout-ms", "300", "-e", js], { timeoutMs: 30000 });
    const ms = performance.now() - t0;
    const out = String(r.stdout) + String(r.stderr);
    assert(r.code !== 0, "a catastrophic regexp under --timeout-ms exits nonzero (code " + r.code + ")");
    assert(/interrupted/.test(out), "the exit names the interrupt: " + JSON.stringify(out.slice(0, 120)));
    assert(!/finished|caught/.test(out), "the interrupt is not catchable and the match never finishes");
    // wall-clock: a deadline/timeout check; the bound sits far above the deadline and below the hang it guards against, so load only narrows the gap
    assert(ms < 10000, "it stops near the 300 ms deadline, not after the match (" + Math.round(ms) + " ms)");
});
S.end();

print("test_regexp_no_step_limit: " + n + " assertions, " + failures + " failures");
if (failures) throw new Error("test_regexp_no_step_limit: " + failures + " failures");
