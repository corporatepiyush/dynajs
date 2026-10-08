// flags: --std
// tests/test_audit_budget_3.js -- P1-02: the native memory ceiling is
// OPERATOR-only, and the native ledger's fail direction (P1-03/04/05).
// build-note: needs dyna:sys + dyna:structures (build with CONFIG_NATIVE_MODULES=y)
// Child processes carry the --native-memory-limit rows; pass the binary under
// test as scriptArgs[1] to check a second build:
//     ./dynajs --std tests/test_audit_budget_3.js /path/to/other/dynajs
import { Exec, memoryUsage } from "dyna:sys";
import * as std from "std";

const BIN = scriptArgs[1] || "./dynajs";
let failures = 0;

function assert(cond, msg) {
    if (!cond) {
        failures++;
        print("  FAIL:", msg);
    }
}

// A probe child: every setNativeMemoryLimit call is logged as name=<limit read
// back>, or name!<ErrorName> when the call was refused. Both rows are compared
// against the exact expected string, so a policy change cannot pass silently.
function limitTrace(withCap) {
    return `
        import("dyna:sys").then(async (sys) => {
            const { setNativeMemoryLimit, memoryUsage } = sys;
            const log = [];
            const t = (n, v) => {
                try { setNativeMemoryLimit(v); log.push(n + "=" + memoryUsage().nativeLimit); }
                catch (e) { log.push(n + "!" + e.constructor.name); }
            };
            log.push("start=" + memoryUsage().nativeLimit);
            ${withCap ? `
            t("clear", 0);
            t("raise", 2097152);
            t("lower", 524288);
            t("reraise", 2097152);
            t("clear2", 0);
            ` : `
            t("set4k", 4096);
            t("raise1M", 1048576);
            t("clear", 0);
            t("set64M", 67108864);
            t("set4M", 4194304);
            const { BitSet } = await import("dyna:structures");
            let bigRefused = false;
            try { new BitSet(1 << 30); } catch (e) { bigRefused = e instanceof Error; }
            log.push("bigRefused=" + bigRefused);
            log.push("smallOk=" + (new BitSet(1024).get(0) === false));
            `}
            print(log.join(","));
        })`;
}

function runChild(args, label) {
    const r = Exec(BIN, args.concat(["-e", limitTrace(label === "cap")]), {
        timeoutMs: 20000,
        encoding: "utf8",
    });
    return {
        out: (r.stdout + "").trim(),
        info: " [code " + r.code + " timedOut " + r.timedOut + " stderr "
            + String(r.stderr).slice(0, 120) + "]",
    };
}

// ---- P1-02a: an operator cap is a ceiling a script cannot raise or clear ----
{
    const withCap = runChild(["--native-memory-limit", "1048576"], "cap");
    assert(withCap.out
        === "start=1048576,clear!RangeError,raise!RangeError,lower=524288,"
            + "reraise!RangeError,clear2!RangeError",
        "operator cap enforced, got: " + withCap.out + withCap.info);
    print("  ok  operator cap: only the operator installs it, a script may only lower");
}

// ---- P1-02b: with no operator cap a script owns its cap, and keeps owning it ----
// This row is the P1-02 regression gate: under the old first-nonzero-writer
// rule the 4096 below armed the process ceiling, so raise1M/clear/set64M all
// threw and the script locked ITSELF out of its own cap forever.
{
    const noCap = runChild([], "nocap");
    assert(noCap.out
        === "start=0,set4k=4096,raise1M=1048576,clear=0,set64M=67108864,set4M=4194304,"
            + "bigRefused=true,smallOk=true",
        "no operator cap: a script cap is the script's to change, got: " + noCap.out
            + noCap.info);
    print("  ok  script cap: settable, raisable, clearable, and still enforced");
}

// ---- tracker ledger: grow, refuse, shrink (P1-03/04/05) ----
//
// What this block can and cannot gate, stated plainly:
//
//   * Observable, and RED on the pre-fix tree: a script-installed cap stays the
//     script's to RAISE. The tracker work below needs to re-arm its own cap
//     between phases, so the pre-fix first-wonzero-writer rule throws right here
//     and the block dies. (asserted explicitly as "raise its own cap")
//   * Observable and green on both trees, but with real teeth: the ledger is
//     EXACT after grow/free, after a refused grow, and after realloc shrink
//     churn; and it never exceeds the cap while refusing. A tracker that leaked
//     a reservation (P1-04a: shrink untracking before, or not after, a
//     successful realloc) or that handed back a double untrack (P1-04b) would
//     fail these lines.
//   * NOT observable from script: P1-03's contended CAS. Overshooting the cap
//     needs two threads in dyn_nat_track at once; the counted allocator is
//     reached from worker threads only through worker message delivery, so this
//     suite cannot drive it deterministically. Same for an inconsistent untrack
//     (P1-04b's fail direction): no script-reachable path double-untracks --
//     every dyn_nat_untrack caller in dyna-nat.c is paired. Those two are
//     covered by code review plus the concurrent module suites, not here.
{
    const { setNativeMemoryLimit } = await import("dyna:sys");
    const { Deque } = await import("dyna:structures");

    setNativeMemoryLimit(2 * 1024 * 1024);
    const base = memoryUsage().nativeSize;

    // grow then free: the ledger must land exactly back on the baseline
    let d = new Deque();
    for (let i = 0; i < 50000; i++)
        d.pushBack(i);
    const grown = memoryUsage().nativeSize;
    assert(grown > base, "deque grow tracked (" + grown + " <= " + base + ")");
    d = null;
    std.gc();
    std.gc();
    assert(memoryUsage().nativeSize === base,
        "ledger balanced after grow+free: " + memoryUsage().nativeSize + " vs " + base);

    // realloc shrink: push/pop churn reallocs the block down repeatedly. A
    // ledger that untracked on the shrink path before/without a successful
    // realloc drifts here; the exact baseline is the assertion.
    {
        let c = new Deque();
        for (let i = 0; i < 40000; i++)
            c.pushBack(i);
        for (let i = 0; i < 39990; i++)
            c.popFront();
        c = null;
        std.gc();
        std.gc();
        assert(memoryUsage().nativeSize === base,
            "ledger balanced after realloc shrink churn: " + memoryUsage().nativeSize
                + " vs " + base);
    }

    // the script cap is still the script's to raise -- the pre-fix discriminator
    let raised = null;
    try {
        setNativeMemoryLimit(8 * 1024 * 1024);
        raised = memoryUsage().nativeLimit;
    } catch (e) {
        raised = "threw " + e.constructor.name;
    }
    assert(raised === 8 * 1024 * 1024,
        "a script-installed cap is still the script's to raise, got: " + raised);

    // refusal path: the ledger stops at the cap and stays balanced on release
    const cap = 8 * 1024 * 1024;
    let d2 = new Deque();
    let threw = false;
    let peak = 0;
    try {
        for (let i = 0; i < 10_000_000; i++) {
            d2.pushBack(i);
            if ((i & 1023) === 0) {
                const live = memoryUsage().nativeSize;
                if (live > peak)
                    peak = live;
            }
        }
    } catch (e) {
        threw = true;
    }
    assert(threw, "OOM thrown under the " + cap + " script cap");
    assert(peak > 0 && peak <= cap,
        "ledger never overshoots the cap under refusal (peak " + peak + ", cap " + cap + ")");
    d2 = null;
    std.gc();
    std.gc();
    assert(memoryUsage().nativeSize === base,
        "ledger balanced after OOM refusal: " + memoryUsage().nativeSize + " vs " + base);

    // the cap is still live after the refusal, and still the script's to drop
    assert(memoryUsage().nativeLimit === cap,
        "cap survives the refusal path: " + memoryUsage().nativeLimit);
    setNativeMemoryLimit(0);
    assert(memoryUsage().nativeLimit === 0, "script cap cleared");
    print("  ok  native ledger: grow/free, realloc shrink, refusal, exact baseline");
}

if (failures) {
    print("test_audit_budget_3: " + failures + " FAILURES");
    throw new Error("test_audit_budget_3 failed");
}
print("test_audit_budget_3: all tests passed");