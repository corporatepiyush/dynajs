// flags: --std
// tests/kit.js proved against itself: a kit that cannot report a failure is
// worse than none, so every way a case can fail is driven through a child
// process and judged by what a RUNNER sees -- the exit status and the lines
// that start with FAIL -- never by the kit's own counters.
import { args, Exec } from "dyna:sys";
import { Path, makeTempDir, writeFile, removeAll } from "dyna:file";
import { suite } from "./kit.js";

const t = suite("test_kit");
const T = String(makeTempDir("kit_"));
const BIN = args()[0].startsWith("/") ? args()[0] : String(Path.cwd()) + "/" + args()[0];
const KIT = String(Path.cwd()) + "/tests/kit.js";
let seq = 0;
function run(body, extra = []) {
    const p = T + "/s" + (seq++) + ".mjs";
    writeFile(new Path(p), `import { suite } from ${JSON.stringify(KIT)};\nconst t = suite("child");\n${body}\nawait t.run();\n`);
    const r = Exec(BIN, [p, ...extra], { timeoutMs: 20000, encoding: "utf8" });
    return { code: r.code, out: r.stdout + r.stderr, fails: (r.stdout + r.stderr).split("\n").filter((l) => /^\s*FAIL([: ]|$)/.test(l)).length };
}
const PASSING = `t.test("alpha one", (a) => { a.eq(1 + 1, 2); a.ok(true); });\nt.test("beta two", (a) => { a.deep({ x: [1, new Uint8Array([2])] }, { x: [1, new Uint8Array([2])] }); });`;

t.test("a passing suite exits 0 with the summary line", ({ eq, match }) => {
    const r = run(PASSING);
    eq(r.code, 0, "exit status");
    eq(r.fails, 0, "FAIL lines");
    match(r.out, /^child: 2 passed, 0 failed \(3 checks, \d+ ms\)$/m, "summary");
});

t.test("every kind of failure is a FAIL line and a non-zero exit", ({ ok }) => {
    const kinds = {
        "a false ok": `t.test("c", (a) => { a.ok(false, "nope"); });`,
        "an unequal eq": `t.test("c", (a) => { a.eq(-0, 0); });`,
        "a deep mismatch": `t.test("c", (a) => { a.deep({ x: 1 }, { x: 2 }); });`,
        "a near miss": `t.test("c", (a) => { a.near(1, 1.1, 1e-6); });`,
        "an uncaught throw": `t.test("c", (a) => { a.ok(true); throw new Error("boom"); });`,
        "throws() that does not throw": `t.test("c", (a) => { a.throws(() => 1); });`,
        "throws() of the wrong type": `t.test("c", (a) => { a.throws(() => { throw new RangeError("x"); }, TypeError); });`,
        "a rejected async case": `t.test("c", async (a) => { a.ok(true); await Promise.reject(new Error("late")); });`,
        "rejects() that resolves": `t.test("c", async (a) => { await a.rejects(Promise.resolve(1)); });`,
        "a case with no assertion": `t.test("c", () => {});`,
        "a failed must()": `t.test("c", (a) => { a.must(false, "stop"); a.ok(true); });`,
        "an async case past its timeout": `t.test("c", async (a) => { a.ok(true); await new Promise(() => {}); }, { timeoutMs: 200 });`,
        "a throwing cleanup": `t.test("c", (a) => { a.ok(true); }); t.after(() => { throw new Error("cleanup"); });`,
    };
    for (const [what, body] of Object.entries(kinds)) {
        const r = run(body);
        ok(r.code !== 0 && r.fails >= 1, what + " fails the suite (exit " + r.code + ", " + r.fails + " FAIL lines)");
    }
});

t.test("one failing case does not hide the others", ({ eq, match }) => {
    const r = run(`t.test("bad", (a) => { a.ok(false); });\n` + PASSING);
    eq(r.code !== 0, true, "the suite fails");
    match(r.out, /^child: 2 passed, 1 failed/m, "and still ran and counted the passing cases");
});

t.test("--case selects by substring and by regular expression", ({ eq, match }) => {
    let r = run(PASSING, ["--case=beta"]);
    eq(r.code, 0, "substring: exit status");
    match(r.out, /^child: 1 passed, 0 failed, 1 not selected/m, "substring: one case ran");
    r = run(PASSING, ["--case=/^ALPHA/i", "--verbose"]);
    match(r.out, /^ok +alpha one \(2 checks/m, "regular expression: the matching case ran");
    eq(/beta two/.test(r.out), false, "and the other did not");
    r = run(PASSING + `\nt.test("bad", (a) => { a.ok(false); });`, ["--case=alpha"]);
    eq(r.code, 0, "an unselected failing case is not run");
});

t.test("a filter that selects nothing fails", ({ ok, match }) => {
    const r = run(PASSING, ["--case=nothing-like-this"]);
    ok(r.code !== 0 && r.fails >= 1, "exit " + r.code + ", " + r.fails + " FAIL lines");
    match(r.out, /selects none of its 2 cases/, "and says why");
});

t.test("--list names the cases and runs none", ({ eq, match }) => {
    const r = run(`t.test("explodes", () => { throw new Error("must not run"); });\n` + PASSING, ["--list"]);
    eq(r.code, 0, "exit status");
    match(r.out, /^explodes\nalpha one\nbeta two\nchild: 3 cases listed$/m, "the names, in order");
});

t.test("a skip is printed and counted, never a pass", ({ eq, match }) => {
    const r = run(`t.test("needs a tool", (a) => { a.skip("tool absent"); });\nt.skip("declared off", "not on this platform");\n` + PASSING);
    eq(r.code, 0, "exit status");
    match(r.out, /^SKIP: \[needs a tool\] tool absent$/m, "the in-case skip and its reason");
    match(r.out, /^SKIP: \[declared off\] not on this platform$/m, "the declared skip and its reason");
    match(r.out, /^child: 2 passed, 0 failed, 2 SKIPPED/m, "counted apart from the passes");
});

t.test("--bail stops at the first failing case", ({ eq }) => {
    const r = run(`t.test("first", (a) => { a.ok(false); });\nt.test("second", (a) => { console.log("SECOND RAN"); a.ok(true); });`, ["--bail"]);
    eq(/SECOND RAN/.test(r.out), false, "the case after the failure did not run");
});

t.test("--json ends with one machine-readable line", ({ deep }) => {
    const r = run(PASSING + `\nt.test("bad", (a) => { a.ok(false); });`, ["--json"]);
    const line = r.out.split("\n").find((l) => l.startsWith("KIT "));
    deep(JSON.parse(line.slice(4)), { suite: "child", passed: 2, failed: 1, skipped: 0, notSelected: 0, checks: 4, failedCases: ["bad"] }, "the record");
});

t.test("duplicate case names are refused", ({ ok }) => {
    const r = run(`t.test("same", (a) => a.ok(true));\nt.test("same", (a) => a.ok(true));`);
    ok(r.code !== 0 && /declares the case "same" twice/.test(r.out), "exit " + r.code);
});

t.after(() => removeAll(new Path(T)));
await t.run();
