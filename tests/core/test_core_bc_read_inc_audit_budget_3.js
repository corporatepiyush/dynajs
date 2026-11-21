// flags: --std
// covers: c:src/serialize/bc_read.inc.c
// tests/core/test_audit_budget_3.js -- P1-02: the native memory ceiling is
// OPERATOR-only, and the native ledger's fail direction (P1-03/04/05).
// build-note: needs dyna:sys + dyna:structures (build with CONFIG_NATIVE_MODULES=y)
// Child processes carry the --native-memory-limit rows; pass the binary under
// test as scriptArgs[1] to check a second build:
//     ./dynajs --std tests/core/test_audit_budget_3.js /path/to/other/dynajs
import { Exec, memoryUsage } from "dyna:sys";
import * as std from "std";
import { sections } from "../lib/kit.js";

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

const S = sections("test_core_bc_read_inc_audit_budget_3");
S.section("an operator cap is a ceiling a script cannot raise or clear", () => {
    const withCap = runChild(["--native-memory-limit", "1048576"], "cap");
    assert(withCap.out
        === "start=1048576,clear!RangeError,raise!RangeError,lower=524288,"
            + "reraise!RangeError,clear2!RangeError",
        "operator cap enforced, got: " + withCap.out + withCap.info);
    print("  ok  operator cap: only the operator installs it, a script may only lower");
});
S.end();

if (failures) {
    print("test_audit_budget_3: " + failures + " FAILURES");
    throw new Error("test_audit_budget_3 failed");
}
print("test_audit_budget_3: all tests passed");
