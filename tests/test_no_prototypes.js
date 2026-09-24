/* test_no_prototypes.js --: the prototype-extension opt-out.
 *
 * The engine ships ~100 project-added extensions on the Array/String/Number/
 * Date/Function/Iterator prototypes plus Array/Object/Function statics.
 * `--no-prototypes` (or DYNAJS_NO_PROTOTYPES=1) skips EXACTLY those tiers at
 * context creation; every standard ES builtin is untouched and the engine
 * must run normally without them.
 *
 * Shape: this file runs in the DEFAULT binary (extensions installed) and
 * spawns child processes for the opt-out modes, because the flag is armed
 * before context creation -- it cannot be toggled inside a live context.
 *
 *   - `--no-prototypes`               -> extensions absent, standard core green
 *   - DYNAJS_NO_PROTOTYPES=1          -> same, via the environment
 *   - flag + DYNAJS_NO_PROTOTYPES=0   -> flag wins: extensions still absent
 *   - DYNAJS_NO_PROTOTYPES=<junk>     -> truthy junk opts out, "0" does not
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_no_prototypes.js
 */
import { Exec, cwd } from "dyna:sys";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ")"); }

const DYN_BIN = cwd() + "/dynajs";

/* ---- 1. default mode: extensions installed (behavior unchanged) --------- */
{
    ok(typeof [].sum === "function", "default: Array.prototype.sum installed");
    ok(typeof "".isEmpty === "function", "default: String.prototype.isEmpty installed");
    ok(typeof (5).clamp === "function", "default: Number.prototype.clamp installed");
    ok(typeof Array.repeat === "function", "default: Array.repeat static installed");
    ok(typeof (1).upto === "function", "default: Number.prototype.upto installed");
    const d = new Date();
    ok(typeof d.isLeapYear === "function", "default: Date.prototype.isLeapYear installed");
    ok(typeof Object.assign !== "undefined", "default: standard Object statics intact");
    ok([3, 1, 2].sort().join(",") === "1,2,3", "default: standard Array core intact");
}

/* ---- 2. the no-prototypes probe script ----------------------------------
 * Runs in every opt-out child. It must pass WITHOUT any project-added
 * extension: only standard ES builtins plus dyna:sys (native, not a
 * prototype extension). Each line names a core area the tier-less engine
 * still has to serve. */
const PROBE = `
const results = [];
const check = (name, fn) => {
    try { fn(); results.push(name + "=ok"); }
    catch (e) { results.push(name + "=FAIL:" + e.message); }
};

check("array-core", () => {
    const a = [5, 3, 1, 4];
    if (JSON.stringify(a.sort((x, y) => x - y)) !== "[1,3,4,5]") throw new Error("sort");
    if (a.filter(x => x > 2).map(x => x * 2).reduce((s, x) => s + x, 0) !== 24) throw new Error("chain");
    if (a.find(x => x === 3) !== 3 || a.includes(4) !== true) throw new Error("find/includes");
    const f = a.flatMap(x => [x, x]);
    if (f.length !== 8 || a.at(-1) !== 5) throw new Error("flatMap/at"); /* sort mutated a in place */
});
check("string-core", () => {
    if ("héllo wörld".toUpperCase() !== "HÉLLO WÖRLD") throw new Error("case");
    if ("a,b,c".split(",").length !== 3 || "  x  ".trim() !== "x") throw new Error("split/trim");
    if (!"abc".includes("b") || "abc".replace(/b/g, "X") !== "aXc") throw new Error("incl/repl");
    if (Array.from("ab").length !== 2) throw new Error("Array.from");
});
check("number-math", () => {
    if (Math.round(2.5) !== 3 || (0.1 + 0.2).toFixed(2) !== "0.30") throw new Error("math");
    if (Number.parseInt("42") !== 42 || Number.isFinite(1 / 0) !== false) throw new Error("numstatic");
    if ((255).toString(16) !== "ff") throw new Error("radix");
});
check("object-core", () => {
    const o = Object.fromEntries([["a", 1], ["b", 2]]);
    if (Object.keys(o).join("") !== "ab" || !Object.hasOwn(o, "a")) throw new Error("statics");
    if (JSON.stringify({ x: [1, { y: null }] }) !== '{"x":[1,{"y":null}]}') throw new Error("json");
    if ({ ...o, c: 3 }.b !== 2) throw new Error("spread");
});
check("date-core", () => {
    const d = new Date(0);
    if (d.getTime() !== 0 || d.toISOString() !== "1970-01-01T00:00:00.000Z") throw new Error("iso");
});
check("regexp-core", () => {
    if (!/^(\\d{3})-(\\d+)$/.exec("123-4567")) throw new Error("exec");
    if ("aaa".replaceAll("a", "b") !== "bbb") throw new Error("replaceAll");
});
check("promise-async", async () => {
    const v = await Promise.all([1, 2].map(async x => x * 10));
    if (v.join(",") !== "10,20") throw new Error("all");
});
check("mapset-iter", () => {
    const m = new Map([["k", 1]]);
    if (m.get("k") !== 1 || [...new Set([1, 2, 2])].length !== 2) throw new Error("mapset");
    if ([...m.entries()].flat().join("") !== "k1") throw new Error("entries");
});
check("bigint-symbol", () => {
    if (2n ** 63n !== 9223372036854775808n) throw new Error("bigint");
    if (Symbol.iterator.toString() !== "Symbol(Symbol.iterator)") throw new Error("symbol");
});

/* the actual CC-3 claim: every project-added tier is ABSENT */
const absent = [];
const expectAbsent = [
    ["Array.prototype.sum", () => [].sum],
    ["Array.prototype.first", () => [].first],
    ["Array.prototype.lazy", () => [].lazy],
    ["Array.prototype.isEmpty", () => [].isEmpty],
    ["Array.prototype.splitEvery", () => [].splitEvery],
    ["Array.repeat", () => Array.repeat],
    ["String.prototype.isEmpty", () => "".isEmpty],
    ["String.prototype.lazy", () => "".lazy],
    ["String.prototype.words", () => "".words],
    ["String.prototype.encodeBase64", () => "".encodeBase64],
    ["Number.prototype.clamp", () => (1).clamp],
    ["Number.prototype.upto", () => (1).upto],
    ["Number.prototype.abs", () => (1).abs],
    ["Date.prototype.isLeapYear", () => new Date().isLeapYear],
    ["Date.prototype.isValid", () => new Date().isValid],
    ["Object.identity", () => Object.identity],
    ["Object.pick", () => Object.pick],
    ["Function.prototype.curry", () => Function.prototype.curry],
    ["Function.identity", () => Function.identity],
    ["Iterator.prototype.takeWhile", () => Iterator.prototype.takeWhile],
    ["Iterator.prototype.scan", () => Iterator.prototype.scan],
    ["Iterator.prototype.tee", () => Iterator.prototype.tee],
];
for (const [name, get] of expectAbsent)
    if (get() !== undefined) absent.push(name);
if (absent.length) throw new Error("still installed: " + absent.join(","));

/* ...while the STANDARD members of the same prototypes survive (note:
 * Iterator helpers map/filter/take/DROP are ES2025 standard, NOT the ext tier) */
const mustStay = [
    () => [].map, () => [].filter, () => "".split, () => "".trim,
    () => (1).toFixed, () => Date.prototype.getTime,
    () => Object.keys, () => Function.prototype.call,
    () => Iterator.prototype.drop, () => Iterator.prototype.map,
];
for (const f of mustStay)
    if (typeof f() !== "function" && f() !== true) throw new Error("standard member missing");

print(results.join("\\n"));
if (results.some(r => r.includes("FAIL"))) throw new Error("core battery failed");
print("PROBE-OK " + results.length);
`;

function child(argv, what) {
    let r;
    try {
        r = Exec(argv[0], argv.slice(1), { timeoutMs: 30000 });
    } catch (e) {
        r = { code: -1, stdout: "", stderr: String(e.message) };
    }
    return { what, code: r.code, out: r.stdout || "", err: r.stderr || "" };
}

/* ---- 3. --no-prototypes: tiers absent, standard core green --------------- */
{
    const r = child([DYN_BIN, "--no-prototypes", "-m", "-e", PROBE], "flag");
    eq(r.code, 0, "--no-prototypes child exits 0");
    ok(r.out.includes("PROBE-OK 9"), "--no-prototypes: 9-area core battery green");
    if (r.code !== 0 || !r.out.includes("PROBE-OK")) {
        print("  stdout: " + r.out);
        print("  stderr: " + r.err);
    }
}

/* ---- 4. DYNAJS_NO_PROTOTYPES=1: same outcome via the environment --------- */
{
    /* /usr/bin/env sets the variable then execs the binary (Exec has no env
     * option, and the parent must not export the variable into itself). */
    const r = child(["/usr/bin/env", "DYNAJS_NO_PROTOTYPES=1", DYN_BIN, "-m", "-e", PROBE], "env");
    eq(r.code, 0, "DYNAJS_NO_PROTOTYPES=1 child exits 0");
    ok(r.out.includes("PROBE-OK 9"), "env opt-out: core battery green");
    if (r.code !== 0 || !r.out.includes("PROBE-OK")) {
        print("  stdout: " + r.out);
        print("  stderr: " + r.err);
    }
}

/* ---- 5. flag wins over an explicitly disabling env value ----------------- */
{
    const r = child(["/usr/bin/env", "DYNAJS_NO_PROTOTYPES=0", DYN_BIN,
                     "--no-prototypes", "-m", "-e", PROBE], "flag-wins");
    eq(r.code, 0, "--no-prototypes + env=0 still opts out (flag wins)");
    ok(r.out.includes("PROBE-OK 9"), "flag-wins: core battery green");
    if (r.code !== 0 || !r.out.includes("PROBE-OK")) {
        print("  stdout: " + r.out);
        print("  stderr: " + r.err);
    }
}

/* ---- 6. env value semantics ---------------------------------------------- */
{
    const junk = child(["/usr/bin/env", "DYNAJS_NO_PROTOTYPES=yes", DYN_BIN, "-e",
                        "print(typeof [].sum)"], "env-junk");
    eq(junk.out.trim(), "undefined", "env 'yes' opts out (truthy junk)");

    const off = child(["/usr/bin/env", "DYNAJS_NO_PROTOTYPES=0", DYN_BIN, "-e",
                       "print(typeof [].sum)"], "env-0");
    eq(off.out.trim(), "function", "env '0' does NOT opt out");

    const empty = child(["/usr/bin/env", "DYNAJS_NO_PROTOTYPES=", DYN_BIN, "-e",
                         "print(typeof [].sum)"], "env-empty");
    eq(empty.out.trim(), "function", "env empty does NOT opt out");
}

/* ---- 7. default mode has no opt-out: the probe's absence checks must FAIL
 * in a plain child (proves the probe script is actually sensitive, not
 * vacuously green). */
{
    const r = child([DYN_BIN, "-m", "-e", PROBE], "default-sensitivity");
    ok(r.code !== 0 && !r.out.includes("PROBE-OK"),
       "sensitivity: the probe FAILS in a default (extensions-on) child");
    if (r.code === 0) print("  stdout: " + r.out);
}

/* ---- 8. --std + opt-out: the classic std/os modules are C code and must
 * keep working without the JS extension tiers. */
{
    const r = child(["/usr/bin/env", "DYNAJS_NO_PROTOTYPES=1", DYN_BIN, "--std", "-m", "-e",
                     `import * as os from "os";
                      import { ASN1 } from "dyna:serialize";
                      if (typeof os.getcwd !== "function") throw new Error("os missing");
                      if (ASN1.encode(ASN1.int(7))[0] !== 2) throw new Error("ASN1");
                      print("STD-OK");`], "std-modules");
    eq(r.code, 0, "--std modules load with the opt-out armed");
    ok(r.out.includes("STD-OK"), "std/dyna native modules functional without prototypes");
    if (r.code !== 0) print("  stderr: " + r.err);
}

if (fails) {
    print("test_no_prototypes: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_no_prototypes failed");
}
print("test_no_prototypes: " + n + " assertions, 0 failures");
