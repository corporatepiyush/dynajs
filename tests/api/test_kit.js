// covers: c:tests/lib/kit.js
// tests/lib/kit.js proved against itself: a kit that cannot report a failure is
// worse than none, so every way a case can fail is driven through a child
// process and judged by what a RUNNER sees -- the exit status and the lines
// that start with FAIL -- never by the kit's own counters.
import { args, Exec } from "dyna:sys";
import { Path, makeTempDir, writeFile, removeAll } from "dyna:file";
import { suite } from "../lib/kit.js";

const t = suite("test_kit");
const T = String(makeTempDir("kit_"));
const BIN = args()[0].startsWith("/") ? args()[0] : String(Path.cwd()) + "/" + args()[0];
const KIT = String(Path.cwd()) + "/tests/lib/kit.js";
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

t.test("--list still runs the cleanups, so a suite holding a server or timer exits", ({ eq, match }) => {
    const r = run(`const h = setInterval(() => {}, 1000);\nt.after(() => { clearInterval(h); print("CLEANUP RAN"); });\nt.test("one", (a) => a.ok(true));`, ["--list"]);
    eq(r.code, 0, "exit status (a missed cleanup leaves the interval alive and the child times out)");
    match(r.out, /CLEANUP RAN/, "the cleanup ran");
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

function runS(body, extra = [], opts = "") {
    const p = T + "/s" + (seq++) + ".mjs";
    writeFile(new Path(p), `import { sections } from ${JSON.stringify(KIT)};\nconst S = sections("legacy"${opts});\nlet bad = 0;\nfunction ok(c, m) { if (!c) { bad++; print("FAIL: " + m); } }\n${body}\nprint("legacy summary bad=" + bad);\nS.end();\nif (bad) throw new Error("legacy failures");\n`);
    const r = Exec(BIN, [p, ...extra], { timeoutMs: 20000, encoding: "utf8" });
    return { code: r.code, out: r.stdout + r.stderr, fails: (r.stdout + r.stderr).split("\n").filter((l) => /^\s*FAIL([: ]|$)/.test(l)).length };
}
const LEGACY = `S.section("one", () => { print("ran one"); ok(1 === 1, "one"); });\nconst mid = "setup-between";\nawait S.section("two async", async () => { await null; print("ran two " + mid); ok(true, "two"); });\nS.section("three", () => { print("ran three"); ok(true, "three"); });`;

t.test("sections: without kit arguments a section is a plain call", ({ eq, match }) => {
    const r = runS(LEGACY);
    eq(r.code, 0, "exit status");
    eq(r.out, "ran one\nran two setup-between\nran three\nlegacy summary bad=0\n", "the output is exactly the legacy output, no kit line added");
    const t2 = runS(`S.section("boom", () => { throw new Error("legacy abort"); });\nprint("after");`);
    eq(t2.code !== 0 && !/after/.test(t2.out), true, "an exception aborts the script as the bare block did");
});

t.test("sections: --case runs only the selected sections, setup still runs", ({ eq, match }) => {
    const r = runS(LEGACY, ["--case=two"]);
    eq(r.code, 0, "exit status");
    eq(/ran one|ran three/.test(r.out), false, "the unselected sections did not run");
    match(r.out, /^ran two setup-between$/m, "the selected one ran, after the setup statement before it");
    match(r.out, /^legacy: 1 passed, 0 failed, 2 not selected/m, "kit summary");
});

t.test("sections: a FAIL line or a throw fails that section only", ({ ok, match }) => {
    const body = `S.section("prints fail", () => { ok(false, "bad value"); });\nS.section("throws", () => { throw new TypeError("kaput"); });\nawait S.section("rejects", async () => { await null; throw new Error("late"); });\nS.section("fine", () => { print("FINE RAN"); });`;
    const r = runS(body, ["--case=/./"]);
    ok(r.code !== 0, "the suite fails (exit " + r.code + ")");
    match(r.out, /^FAIL: \[throws\] threw TypeError: kaput/m, "a throw is reported against its section");
    match(r.out, /^FAIL: \[rejects\] threw Error: late/m, "so is an async rejection");
    match(r.out, /^FINE RAN$/m, "and the sections after them still ran");
    match(r.out, /^legacy: 1 passed, 3 failed/m, "three failing sections, one passing");
});

t.test("sections: --list names them and runs none", ({ eq, match }) => {
    const r = runS(`S.section("explodes", () => { throw new Error("must not run"); });\n` + LEGACY, ["--list"]);
    eq(r.code, 0, "exit status");
    match(r.out, /^explodes\none\ntwo async\nthree\n/m, "the names in order");
    match(r.out, /^legacy: 4 cases listed$/m, "count line");
    const tail = runS(LEGACY + `\nprint("FAIL: tail check that needs the sections");\nconsole.log("FAIL: via console.log");\nif (!S.listing) throw new Error("tail runs only when not listing");`, ["--list"]);
    eq(tail.code, 0, "code outside the sections is muted under --list and can test S.listing (exit " + tail.code + ")");
    eq(tail.out, "\none\ntwo async\nthree\nlegacy: 3 cases listed\n", "only a blank line (ends any unterminated suite output), the names and the count are printed");
    const after = runS(`S.section("a", () => {});\nS.end();\nprint("FAIL: shutdown code after end()");`.replace(/\nS\.end\(\);\n/, "\nS.end();\n"), ["--list"]);
    eq(/shutdown code after end/.test(after.out), false, "output after end() stays muted under --list");
    const plain = runS(`print("listing=" + S.listing);`);
    match(plain.out, /^listing=false$/m, "listing is false outside --list");
});

t.test("sections: a filter selecting nothing fails, duplicates are refused", ({ ok }) => {
    const r = runS(LEGACY, ["--case=zzz"]);
    ok(r.code !== 0 && /selects none of its 3 cases/.test(r.out), "empty selection (exit " + r.code + ")");
    const d = runS(`S.section("x", () => {});\nS.section("x", () => {});`);
    ok(d.code !== 0 && /declares the section "x" twice/.test(d.out), "duplicate name (exit " + d.code + ")");
});

t.test("sections: chain runs unselected sections as setup", ({ eq, match }) => {
    const body = `let state = 0;\nS.section("make", () => { state = 41; });\nS.section("use", () => { ok(state === 41, "state from make"); state++; });\nS.section("later", () => { print("LATER RAN"); });`;
    const plain = runS(body, ["--case=use"]);
    eq(plain.code !== 0, true, "without chain the dependent section fails alone");
    const chained = runS(body, ["--case=use"], ", { chain: true }");
    eq(chained.code, 0, "with chain it passes (exit " + chained.code + ")");
    match(chained.out, /^legacy: 1 passed, 0 failed, 2 not selected/m, "and only the selected section is reported");
});

t.after(() => removeAll(new Path(T)));
await t.run();
