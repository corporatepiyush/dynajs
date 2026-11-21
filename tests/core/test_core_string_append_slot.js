// covers: c:src/runtime/class.inc.c
// F24: s += E, where E is not a constant or a plain local, compiles to get_loc s; E; add; put_loc s
// (likewise for args, closure variables and s = s + E), so at the add the string has two owners: the
// variable and the operand stack. The old add could never append in place there and copied all of s
// on every step below 8192 chars (JS_ConcatString1 = 15-25% of a string-building profile; 3.6-4x
// slower than the add_loc form). The add now looks at the opcode that follows: when it stores into the
// very slot that holds the left operand and that slot plus the stack are the only two owners, the
// piece is appended in place, and when there is no spare room the new string gets 1.5x headroom so
// the next appends go in place. The add_loc form (s += local) takes the same headroom copy when its
// in-place append has no room (200-char pieces: 8.2 -> 5.1 ms). Headroom is only for the accumulator
// shape (left >= 4x the piece): unconditional headroom kept +52% RSS on a retained one-shot
// s = a + b; s += c of comparable sizes.
// Both interpreters carry the same path: the tail-call one (clang builds) and the switch loop (builds
// without preserve_none, e.g. gcc; mutation-proven on a build with EXTRA_CFLAGS=-UCONFIG_TAILCALL_INTERP).
// Aliasing is the risk: any other holder (another variable, an object field, an array slot, a Map
// key, a snapshot taken inside E, E reassigning s) must never see the string change.
// The oracle is Array.prototype.join over the same pieces.
import { suite } from "../lib/kit.js";
import { cpuNow } from "../lib/cpuclock.js";
import { perfCheck } from "../lib/perf.js";

const t = suite("test_core_string_append_slot");
const P = [];
for (let i = 0; i < 64; i++) P.push("item" + i);

t.test("local, arg, closure and s = s + E builds equal join", ({ eq }) => {
    for (const n of [1, 7, 100, 1500, 3000, 9000]) {
        const want = [];
        for (let i = 0; i < n; i++) want.push(P[i & 63] + ",");
        const w = want.join("");
        let s = "";
        for (let i = 0; i < n; i++) s += P[i & 63] + ",";
        eq(s, w, "local n=" + n);
        const viaArg = (acc) => { for (let i = 0; i < n; i++) acc += P[i & 63] + ","; return acc; };
        eq(viaArg(""), w, "arg n=" + n);
        let c = "";
        const add = (i) => { c += P[i & 63] + ","; };
        for (let i = 0; i < n; i++) add(i);
        eq(c, w, "closure n=" + n);
        let e = "";
        for (let i = 0; i < n; i++) e = e + P[i & 63] + ",";
        eq(e, w, "s = s + E n=" + n);
        let chained = "", last = "";
        for (let i = 0; i < n; i++) last = (chained += P[i & 63] + ",");
        eq(chained, w, "set form n=" + n);
        eq(last, w, "set form result n=" + n);
    }
});

t.test("generator build equals join", ({ eq }) => {
    function* g(n) {
        let s = "";
        for (let i = 0; i < n; i++) s += P[i & 63];
        yield s;
    }
    for (const n of [5, 2000, 9000]) {
        const want = [];
        for (let i = 0; i < n; i++) want.push(P[i & 63]);
        eq(g(n).next().value, want.join(""), "n=" + n);
    }
});

t.test("holders outside the slot never change", ({ eq }) => {
    let s = "start";
    const snaps = [], arr = [], obj = {}, m = new Map();
    for (let i = 0; i < 400; i++) {
        s += (snaps.push(s), P[i & 63]);
        if (i % 50 === 0) { arr.push(s); obj["k" + i] = s; m.set(s, i); }
    }
    let want = "start", bad = 0;
    for (let i = 0; i < 400; i++) {
        if (snaps[i] !== want) bad++;
        want += P[i & 63];
    }
    eq(bad, 0, "snapshots taken inside E");
    eq(s, want, "final");
    let w2 = "start", k = 0;
    for (let i = 0; i < 400; i++) {
        w2 += P[i & 63];
        if (i % 50 === 0) {
            eq(arr[k++], w2, "array slot " + i);
            eq(obj["k" + i], w2, "object field " + i);
            eq(m.get(w2), i, "map key " + i);
        }
    }
});

t.test("E that reassigns or aliases s", ({ eq }) => {
    let s = "ab", keep = "";
    s += (keep = s, "c");
    eq(s, "abc", "aliased in E: result");
    eq(keep, "ab", "aliased in E: alias unchanged");
    let r = "xy";
    r += (r = "zz", "q");
    eq(r, "xyq", "reassigned in E uses the old left value");
    let u = "p".repeat(300);
    const v = u;
    for (let i = 0; i < 50; i++) u += P[i];
    eq(v, "p".repeat(300), "second local sharing the string");
    let self = "ab";
    for (let i = 0; i < 5; i++) self += self + "";
    eq(self, "ab".repeat(32), "s += s");
});

t.test("UTF-16 pieces and width changes", ({ eq }) => {
    const pieces = [];
    for (let i = 0; i < 3000; i++) pieces.push(i % 40 === 9 ? "中" + i : "z" + i);
    let s = "";
    for (let i = 0; i < pieces.length; i++) s += pieces[i] + "";
    eq(s, pieces.join(""), "mixed width");
    let w = "€";
    for (let i = 0; i < 2000; i++) w += pieces[i] + "€";
    eq(w, "€" + pieces.slice(0, 2000).map((x) => x + "€").join(""), "wide start");
    eq(new Map([[s, 1]]).get(pieces.join("")), 1, "hash agrees after in-place growth");
});

function buildIdx(n, reps) {
    let t = 0;
    for (let r = 0; r < reps; r++) {
        let s = "";
        for (let i = 0; i < n; i++) s += P[i & 63];
        t += s.length;
    }
    return t;
}
function buildLoc(n, reps) {
    let t = 0;
    for (let r = 0; r < reps; r++) {
        let s = "";
        for (let i = 0; i < n; i++) {
            const p = P[i & 63];
            s += p;
        }
        t += s.length;
    }
    return t;
}
function* genIdx(n, reps) {
    let t = 0;
    for (let r = 0; r < reps; r++) {
        let s = "";
        for (let i = 0; i < n; i++) s += P[i & 63];
        t += s.length;
    }
    yield t;
}
const LONG = [];
for (let i = 0; i < 64; i++) LONG.push(("item" + i).padEnd(200, "x"));
function buildLong(reps) {
    let t = 0;
    for (let r = 0; r < reps; r++) {
        let s = "";
        for (let i = 0; i < 40; i++) s += LONG[i & 63];
        t += s.length;
    }
    return t;
}
function joinLong(reps) {
    let t = 0;
    for (let r = 0; r < reps; r++) {
        const a = [];
        for (let i = 0; i < 40; i++) a.push(LONG[i & 63]);
        t += a.join("").length;
    }
    return t;
}
function best(f) {
    let b = Infinity;
    for (let k = 0; k < 7; k++) {
        const c = cpuNow();
        f();
        b = Math.min(b, cpuNow() - c);
    }
    return b;
}

t.test("perf: s += a[i] costs about the same as s += local (function and generator)", ({ ok, skip }) => {
    const idx = best(() => buildIdx(2000, 150));
    const loc = best(() => buildLoc(2000, 150));
    const gen = best(() => genIdx(2000, 150).next());
    const r1 = idx / loc, r2 = gen / loc;
    print("  s += a[i] " + idx.toFixed(1) + " ms, s += local " + loc.toFixed(1) + " ms, generator " + gen.toFixed(1) + " ms");
    perfCheck(ok, r1 < 1.8, "plain function ratio " + r1.toFixed(2) + " < 1.8 (was 3.6-4 with a full copy per step)", skip);
    perfCheck(ok, r2 < 1.8, "generator ratio " + r2.toFixed(2) + " < 1.8 (was 3.6-4 with a full copy per step)", skip);
});

t.test("perf: 200-char pieces grow with headroom, cheaper than join", ({ ok, skip }) => {
    const b = best(() => buildLong(2000));
    const j = best(() => joinLong(2000));
    const r = b / j;
    print("  += " + b.toFixed(1) + " ms, join " + j.toFixed(1) + " ms");
    perfCheck(ok, r < 0.95, "+= / join " + r.toFixed(2) + " < 0.95 (exact-size copies: 1.2, before F24: 2.2)", skip);
});

await t.run();
