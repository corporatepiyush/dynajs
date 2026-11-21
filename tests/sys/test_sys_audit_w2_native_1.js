// timeout: 300
// covers: c:src/dyna-cli.c
// Split from tests/test_audit_w2_native_1.js: the sys sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_sys_audit_w2_native_1");
import { Exec, args } from "dyna:sys";

const BIN = args()[0];
let failures = 0, checks = 0;
function ok(cond, msg) {
    checks++;
    if (!cond) {
        failures++;
        print("  FAIL:", msg);
    }
}
function eq(got, want, msg) {
    ok(Object.is(got, want), msg + " (got " + String(got) + ", want " + String(want) + ")");
}
async function errName(fn) {
    try { await fn(); return "none"; } catch (e) { return e.name; }
}
async function errNameAsync(fn) {
    try { await fn(); return "none"; } catch (e) { return e.name; }
}
// ---- M1c-24 / budgets: child processes -----------------------------------------------
await __kitS.section("M1c-24 / budgets: child processes", async () => {
    const run = (args, src) => Exec(BIN, args.concat(["-e", src]), { timeoutMs: 60000, encoding: "utf8" });
    const dec = run(["--module", "--native-memory-limit", "8000000"],
        'import { Decimal } from "dyna:decimal"; const keep=[]; const big="7".repeat(50000);' +
        'try { for(;;) keep.push(new Decimal(big)); } catch(e) {}' +
        'try { for(;;) keep.push(new Decimal("1")); } catch(e) {}' +
        'const one = keep[keep.length-1]; let out=[];' +
        'for (const op of ["divmod","add","mul","sub","div"]) { try { one[op](3); out.push("ok"); } catch(e) { out.push(e.name); } }' +
        'print(out.join(","));');
    ok(dec.signal === null && dec.code === 0, "Decimal at the native limit throws instead of crashing (exit " + dec.code + ", signal " + dec.signal + ") (M1c-24)");
    ok(/^(ok|InternalError|RangeError)(,(ok|InternalError|RangeError)){4}/.test((dec.stdout || "").trim()), "Decimal ops report a catchable error at the limit (" + (dec.stdout || "").trim() + ")");

    const t0 = performance.now();
    const fit = run(["--module", "--timeout-ms", "400"],
        // An Argon2id of 1 GiB x 16 passes: several seconds in one native call that never looks at the deadline. Since T2 (owner
        // 11-26) it runs on the io pool, so the script reaches its deadline while awaiting it; exit then drains cancelled
        // offloaded work, and the watchdog caps that drain at the grace window instead of the 10 s teardown, so the hard
        // stop (113) still lands near the deadline. (This used to be a
        // LogisticRegression fit; fits poll the deadline since wave 3 and end
        // with "interrupted", which tests/net/test_audit_w3_native_4.js pins.)
        'import { Argon2id } from "dyna:crypto";' +
        'await Argon2id.hash("pw", new Uint8Array(16), { memory: 1048576, iterations: 16, encoded: false }); print("fit returned");');
    const ms = performance.now() - t0;
    ok(fit.code !== 0 && !fit.timedOut, "a native loop that never polls is still stopped by --timeout-ms (exit " + fit.code + ") (P1-08)");
    // wall-clock: a deadline/timeout check; the bound sits far above the deadline and below the hang it guards against, so load only narrows the gap
    ok(ms < 8000, "the hard stop lands near the deadline (" + (ms | 0) + "ms for a 400ms budget)");
    ok(!/fit returned/.test(fit.stdout || ""), "the fit did not run to completion");
    eq(fit.code, 113, "the hard stop has its own exit code (113)");

    const quick = run(["--timeout-ms", "20000"], 'print(1+1)');
    ok(quick.code === 0 && (quick.stdout || "").trim() === "2", "a script inside its budget is unaffected by the watchdog (control)");

    const tla = run(["--module"], 'await new Promise(() => {}); print("unreachable");');
    ok(tla.code !== 0 && !tla.timedOut && /never settle/.test(tla.stderr || ""), "an await that can never settle is an error, not a spin (C1-13)");
});
__kitS.end();
if (failures) {
    print("test_sys_audit_w2_native_1: " + failures + " of " + checks + " FAILED");
    throw new Error("test_sys_audit_w2_native_1 failed");
}
print("test_sys_audit_w2_native_1: " + checks + " checks passed");
