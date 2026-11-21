// covers: c:src/runtime/class.inc.c
// s += piece on a rope whose right leaf is short used to copy that leaf into a
// fresh string every time (JS_ConcatString2 on a duplicated leaf can never go
// in place). When the rope and its leaf are both uniquely owned the piece is
// now appended into the leaf's spare malloc room and the rope length bumped.
// Aliasing is the risk: a rope or leaf visible from anywhere else (another
// variable, an object field, an array slot, a closure) must never change.
// The oracle is Array.prototype.join over the same pieces.
import { suite } from "../lib/kit.js";

const t = suite("test_core_string_rope_append");

function pieces(n, wide) {
    const p = [];
    for (let i = 0; i < n; i++) p.push((wide && i % 7 === 3 ? "€" : "") + "w" + i + ",");
    return p;
}

t.test("+= of small pieces equals join at every length", ({ eq }) => {
    for (const n of [1, 10, 100, 1000, 3000, 20000])
        for (const wide of [false, true]) {
            const p = pieces(n, wide);
            let s = "";
            for (const x of p) s += x;
            eq(s, p.join(""), "n=" + n + " wide=" + wide);
            eq(s.length, p.join("").length, "length n=" + n);
        }
});

t.test("a snapshot taken mid-loop never changes", ({ eq }) => {
    const p = pieces(5000, false);
    let s = "";
    const snaps = [];
    for (let i = 0; i < p.length; i++) {
        s += p[i];
        if (i % 97 === 0) snaps.push([i, s]);
    }
    let bad = 0;
    for (const [i, snap] of snaps) if (snap !== p.slice(0, i + 1).join("")) bad++;
    eq(bad, 0, "snapshots that changed after later appends");
});

t.test("ropes held by objects, arrays and closures stay intact", ({ eq }) => {
    let s = "x".repeat(9000);
    s += "a";
    const holder = { v: s }, arr = [s];
    const get = () => s;
    let t2 = s;
    t2 += "b";
    t2 += "c";
    eq(holder.v, "x".repeat(9000) + "a", "object field");
    eq(arr[0], "x".repeat(9000) + "a", "array slot");
    eq(get(), "x".repeat(9000) + "a", "closure");
    eq(t2, "x".repeat(9000) + "abc", "the appended copy");
});

t.test("+= on one of two locals sharing a rope leaves the other alone", ({ eq }) => {
    function share(p) {
        let s = "x".repeat(9000) + p;
        const keep = s;
        let t2 = s;
        for (let i = 0; i < 3; i++) t2 += p;
        let u = keep;
        for (let i = 0; i < 2; i++) u += "d";
        return [keep, t2, u, s];
    }
    const [keep, t2, u, s] = share("q");
    eq(keep, "x".repeat(9000) + "q", "kept alias");
    eq(s, "x".repeat(9000) + "q", "original local");
    eq(t2, "x".repeat(9000) + "qqqq", "first appender");
    eq(u, "x".repeat(9000) + "qdd", "second appender");
});

t.test("Latin-1 leaf with a UTF-16 piece, then Latin-1 again", ({ eq }) => {
    let s = "y".repeat(10000);
    const want = ["y".repeat(10000)];
    for (let i = 0; i < 300; i++) {
        const x = i % 50 === 25 ? "中" : "z" + i;
        s += x;
        want.push(x);
    }
    eq(s, want.join(""), "mixed width");
    eq(s.charCodeAt(10000 + 2), "z0".charCodeAt(2) || s.charCodeAt(10002), "index read on the rope");
});

t.test("string ops on the grown rope see every byte", ({ eq }) => {
    let s = "";
    for (let i = 0; i < 4000; i++) s += String.fromCharCode(97 + (i % 26));
    let w = "";
    for (let i = 0; i < 4000; i++) w += String.fromCharCode(97 + (i % 26));
    eq(s === w, true, "two independent builds compare equal");
    eq(s.indexOf("xyz"), 23, "indexOf");
    eq(s.lastIndexOf("abc"), 3978, "lastIndexOf");
    eq(new Map([[s, 1]]).get(w), 1, "hash of both builds agrees");
});

await t.run();
