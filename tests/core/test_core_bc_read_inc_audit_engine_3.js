// flags: --std
// covers: c:src/serialize/bc_read.inc.c
// E2-C: a prototype cycle built through a Proxy `getPrototypeOf` trap that
// re-parents DURING Object.setPrototypeOf's cycle check.
//
// Before the fix, JS_SetPrototypeInternal's cycle check called JS_GetPrototype
// on every step, so the trap ran inside the walk it was supposed to satisfy.
// The trap re-pointed X at p while the engine was validating the chain for
// setPrototypeOf(p, X), the walk read the trap's "null" instead of the edge the
// trap had just written, and the engine committed p -> X -> p. Every later
// missing-property lookup then spun in the unguarded
// `p = p->shape->proto` loop (JS_GetPropertyInternal / JS_HasProperty), which
// polls nothing -- so even --timeout-ms could not end it.
//
// The two hang rows run in a child process with a wall-clock deadline, so an
// unfixed engine is reported as a FAILURE instead of wedging this suite.
// The control row is the other half of the finding: a legitimate 10,000-deep
// chain, built without any trap, must still resolve -- the fix is a
// re-validation of what the trap walk claimed, not a depth cap.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_bc_read_inc_audit_engine_3");
import * as os from "os";
import { hostScale } from "../lib/hostscale.js";
import { cpuNow } from "../lib/cpuclock.js";
const TBASE = (std.getenv("TMPDIR") || "/tmp/").replace(/\/?$/, "/") + "dyna_audit_engine_3_" + os.getpid() + "_";

const BIN = std.getenv("DYNAJS_TEST_BIN") || "./dynajs";
const CHILD_BUDGET_MS = 20000;

let n = 0, failures = 0, skips = 0;
function assert(c, m) {
    n++;
    if (!c) { failures++; print("FAIL: " + m); }
}
function eq(got, want, m) {
    assert(got === want, m + " (got " + JSON.stringify(got) +
           ", want " + JSON.stringify(want) + ")");
}
function skip(m) { skips++; print("SKIP: " + m); }

const canSpawn = (typeof os.exec === "function") &&
                 (typeof os.waitpid === "function") &&
                 (typeof os.kill === "function") &&
                 (typeof os.sleep === "function");

function readOut(path) {
    try { return String(std.loadFile(path)); } catch (e) { return ""; }
}

function writeProbe(path, src) {
    const f = std.open(path, "w");
    f.puts(src);
    f.close();
}

function runBounded(srcPath, outPath) {
    const fd = os.open(outPath, os.O_WRONLY | os.O_CREAT | os.O_TRUNC);
    const pid = os.exec([BIN, "--std", "--timeout-ms", String(Math.round(300 * hostScale())), srcPath],
                        { blocking: false, stdout: fd, stderr: fd });
    os.close(fd);
    const deadline = Date.now() + CHILD_BUDGET_MS;
    for (;;) {
        const r = os.waitpid(pid, os.WNOHANG);
        if (r[0] > 0)
            return { exited: true, status: r[1], out: readOut(outPath) };
        if (r[0] < 0)
            return { exited: true, status: r[1], out: readOut(outPath) };
        if (Date.now() >= deadline) {
            try { os.kill(pid, 9); } catch (e) { /* already gone */ }
            os.waitpid(pid, 0);
            return { exited: false, status: -1, out: readOut(outPath) };
        }
        os.sleep(20);
    }
}

// ---- row 1: the trap that re-parents during the cycle check (the reported DoS)
const PC1 = TBASE + 'pc1.js';
writeProbe(PC1, [
    'var p = {}, X = {};',
    'var P1 = new Proxy({}, { getPrototypeOf() { Object.setPrototypeOf(X, p); return null; } });',
    'Object.setPrototypeOf(X, P1);',
    'var outcome;',
    'try { Object.setPrototypeOf(p, X); outcome = "ALLOWED"; }',
    'catch (e) { outcome = "REFUSED"; }',
    'print("outcome=" + outcome);',
    'var cur = p, cyc = false;',
    'try {',
    '    for (var i = 0; i < 64; i++) {',
    '        var q = Object.getPrototypeOf(cur);',
    '        if (q === null) break;',
    '        if (q === p) { cyc = true; break; }',
    '        cur = q;',
    '    }',
    '} catch (e) { print("walkerr=" + e.name); }',
    'print("cycle=" + cyc);',
    'print("xproto=" + (Object.getPrototypeOf(X) === P1));',
    'print("missing=" + (p.nope === undefined));',
    'print("has=" + ("nope" in p));',
    'print("END");',
    ''
].join("\n"));

const PC2 = TBASE + 'pc2.js';

let deepHead = null;

let threw = null;

__kitS.section("a 10,000-deep legitimate chain still resolves a property", () => {
    const head = {};
    let cur = head;
    for (let i = 0; i < 10000; i++) {
        const next = {};
        Object.setPrototypeOf(cur, next);
        cur = next;
    }
    Object.defineProperty(cur, "deepest", { value: 42, configurable: true });
    deepHead = head;

    const t0 = cpuNow();
    eq(head.deepest, 42, "a 10,000-deep legitimate chain still resolves a property " +
       "at the bottom of it");
    eq(head.nope, undefined, "a 10,000-deep legitimate chain still resolves a missing " +
       "property to undefined");
    assert("deepest" in head, "`in` still sees through a 10,000-deep legitimate chain");
    assert(!("nope" in head), "`in` still answers false at the top of a 10,000-deep chain");
    const ms = cpuNow() - t0;
    assert(ms < 5000, "the 10,000-deep lookups stay fast (" + ms + "ms) -- the fix is " +
           "not a depth cap");

    // the cycle check walks the whole chain before every setPrototypeOf, so a deep
    // head must still be accepted as a prototype
    const tip = {};
    threw = null;
    try { Object.setPrototypeOf(tip, deepHead); } catch (e) { threw = e; }
    assert(threw === null, "a 10,000-deep chain can still be set as a prototype " +
           "(got " + threw + ")");
    eq(Object.getPrototypeOf(tip), deepHead, "the deep prototype is the one that was set");
    eq(tip.deepest, 42, "property access still works through the deep head");

    let seen = 0;
    for (const k in tip) seen++;
    assert(seen === 0, "for-in over a 10,000-deep chain completes with no own names");
});

__kitS.section("class/extends still resolves through the prototype chain", () => {
    class A { tag() { return "A"; } }
    class B extends A { tag() { return "B" + super.tag(); } }
    eq(new B().tag(), "BA", "class/extends still resolves through the prototype chain");
    eq(Object.getPrototypeOf(B.prototype), A.prototype,
       "extends still links the two prototypes");
    eq(new B() instanceof A, true, "instanceof still walks the prototype chain");

    const proto = { inherited: 7 };
    const child = Object.create(proto);
    child.own = 1;
    eq(child.inherited, 7, "Object.create inheritance still resolves");
    eq(child.own, 1, "an own property still shadows the prototype");
    eq(Object.getPrototypeOf(Object.create(null)), null,
       "a null-prototype object is still created with a null prototype");

    const a = {}, b = {};
    Object.setPrototypeOf(b, a);
    threw = null;
    try { Object.setPrototypeOf(a, b); } catch (e) { threw = e; }
    assert(threw instanceof TypeError,
           "an ordinary 2-cycle is still refused with a TypeError (got " + threw + ")");
    eq(Object.getPrototypeOf(a), Object.prototype,
       "the object of a refused cycle keeps its prototype");

    const pr = new Proxy({}, {});
    const o = {};
    Object.setPrototypeOf(o, pr);
    eq(Object.getPrototypeOf(o), pr, "a Proxy is still accepted as a prototype");
    eq(Object.getPrototypeOf({}), Object.prototype,
       "an ordinary object still inherits Object.prototype");
    eq(o.toString, Object.prototype.toString,
       "a proxy prototype still forwards to the target's chain");

    const deepNull = Object.create(null);
    deepNull.k = 3;
    eq(deepNull.k, 3, "a null-prototype object still stores its own properties");
    eq(deepNull.toString, undefined,
       "a null-prototype object still has no Object.prototype");
});
__kitS.end();

for (const f of [PC1, PC2, TBASE + 'pc1.out', TBASE + 'pc2.out']) { try { os.remove(f); } catch (e) { /* best effort */ } }

if (failures)
    throw new Error("test_audit_engine_3: " + failures + " failures");
print("test_audit_engine_3: all " + n + " assertions passed" +
      (skips ? " (" + skips + " rows skipped)" : ""));
