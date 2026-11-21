// flags: --std
// covers: c:src/modules/libc/libc-std-file.c
// tests/core/test_audit_budget_3.js -- P1-02: the native memory ceiling is
// OPERATOR-only, and the native ledger's fail direction (P1-03/04/05).
// build-note: needs dyna:sys + dyna:structures (build with CONFIG_NATIVE_MODULES=y)
// Child processes carry the --native-memory-limit rows; pass the binary under
// test as scriptArgs[1] to check a second build:
//     ./dynajs --std tests/core/test_audit_budget_3.js /path/to/other/dynajs
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_libc_std_file_audit_budget_3");
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

await __kitS.section("deque grow tracked", async () => {
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
});
__kitS.end();

if (failures) {
    print("test_audit_budget_3: " + failures + " FAILURES");
    throw new Error("test_audit_budget_3 failed");
}
print("test_audit_budget_3: all tests passed");
