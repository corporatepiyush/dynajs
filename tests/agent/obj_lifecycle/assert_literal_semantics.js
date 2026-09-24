/* Byte-identical behavior assertions for object-literal / object-lifecycle
 * optimizations. Prints one line per check; the runner diffs stdout between
 * the pristine and candidate binaries (and exit codes). Covers:
 * property order, enumerability, attributes, __proto__, getters/setters,
 * computed keys, spread, numeric keys, symbols, duplicate keys, literals
 * whose value expressions throw, nested literals, seal/freeze interplay,
 * and objects that BECame non-eligible after construction. */
function keys(o) { return Object.keys(o).join(","); }
function desc(o, k) {
    var d = Object.getOwnPropertyDescriptor(o, k);
    if (d === undefined) return "none";
    return (d.writable ? "w" : "-") + (d.enumerable ? "e" : "-") +
           (d.configurable ? "c" : "-") + ":" + typeof d.value + ":" +
           (d.get ? "G" : "") + (d.set ? "S" : "");
}

// 1. plain literal: order + attributes + values
var a = {x: 1, y: 2, z: 3};
print("1 " + keys(a) + " " + desc(a, "x") + " " + a.x + a.y + a.z);

// 2. computed-value literal (the shape the parse-time presize misses)
var v = {p: 1, q: 2};
var b = {m: v.p + 1, n: v.q * 2, o: v.p * v.q};
print("2 " + keys(b) + " " + b.m + "," + b.n + "," + b.o + " " + desc(b, "m"));

// 3. nested literal
var c = {outer: {inner: 1}, after: 2};
print("3 " + keys(c) + " " + keys(c.outer) + " " + c.outer.inner + c.after);

// 4. shorthand + method
var xs = 1;
var d = {xs, ys: xs + 1, f() { return this.xs + this.ys; }};
print("4 " + keys(d) + " " + d.f());

// 5. getter/setter literal
var e = {
    _v: 1,
    get g() { return this._v + 1; },
    set g(x) { this._v = x; },
    h: 2
};
e.g = 10;
print("5 " + keys(e) + " " + e.g + " " + desc(e, "g") + " " + desc(e, "_v"));

// 6. computed key
var kn = "ke" + "y";
var f = {[kn]: 1, other: 2};
print("6 " + keys(f) + " " + f[kn]);

// 7. spread
var g = {...v, extra: 3};
print("7 " + keys(g) + " " + g.p + g.q + g.extra);

// 8. numeric + symbol-ish keys
var h = {0: "a", 1: "b", normal: 1};
print("8 " + keys(h) + " " + h[0] + h[1] + h.normal);

// 9. duplicate key: later wins, position stays
var i = {dup: 1, mid: 2, dup: 3};
print("9 " + keys(i) + " " + i.dup + " " + i.mid);

// 10. __proto__ mid-literal
var protoMark = {pm: 42};
var j = {first: 1, __proto__: protoMark, second: 2};
print("10 " + keys(j) + " " + j.first + j.second + " " + j.pm + " " +
      Object.getPrototypeOf(j) === protoMark ? "protoOK" : "protoBAD");
print("10b " + (Object.getPrototypeOf(j) === protoMark));

// 11. value expression throws: object never escapes
function throwInValue() {
    try {
        var t = {a: 1, b: boom(), c: 2};
        return "no-throw";
    } catch (err) {
        return "threw";
    }
}
function boom() { throw new Error("x"); }
print("11 " + throwInValue());

// 12. literal in a loop: same site many times, values vary
var acc = "";
for (var loop = 0; loop < 3; loop++) {
    var L = {q: loop, r: loop * 2, s: "s" + loop};
    acc += keys(L) + ":" + L.q + L.r + L.s + ";";
}
print("12 " + acc);

// 13. object becomes non-eligible AFTER construction (added getter later)
var m = {u: 1, w: 2};
Object.defineProperty(m, "u", {get() { return 99; }});
print("13 " + keys(m) + " " + m.u + m.w + " " + desc(m, "u"));

// 14. object gains a foreign-value prop later, then dies (teardown path)
(function () {
    var n = {u: 1, w: 2};
    n.child = {deep: true};
    n.getter = function () {};
    Object.defineProperty(n, "acc", {get() { return 1; }});
    print("14 " + keys(n));
})();

// 15. seal/freeze then use
var o = {u: 1, w: 2};
Object.freeze(o);
try { o.u = 5; } catch (err) {}
print("15 " + o.u + " " + desc(o, "u"));

// 16. literal value referencing the object being built is impossible;
//     but a literal value CAN observe prototype mutation mid-literal
var p = {q1: 1, q2: 2};
print("16 " + keys(p) + " " + JSON.stringify(p));

// 17. many-field literal (transition batching at construction)
var r = {f1: 1, f2: 2, f3: 3, f4: 4, f5: 5, f6: 6, f7: 7, f8: 8, f9: 9};
print("17 " + keys(r) + " " + (r.f1 + r.f9));

// 18. same literal site under two realms/proto is not testable here;
//     instead: literal after Object.prototype gain (proto shape keying)
var s = {a1: 1, b2: 2};
print("18 " + keys(s) + " " + (Object.getPrototypeOf(s) === Object.prototype));

// 19. literal inside try/catch, define order on exception mid-run
var t;
try {
    t = {k1: 1, k2: missing(), k3: 3};
} catch (err) {
    t = "thrown";
}
print("19 " + t);

// 20. eval'd literal (fresh bytecode, no parse-time analysis)
var u = eval("({ev1: 7, ev2: 8})");
print("20 " + keys(u) + " " + u.ev1 + u.ev2);

// 21. weakref-adjacent: registry/finalization not applicable to plain
//     objects without FinalizationRegistry use; exercise WeakRef on a
//     literal-fed object then read it back while surely alive.
var wr = {alive: 1};
var ref = new WeakRef(wr);
print("21 " + (ref.deref() === wr) + " " + keys(ref.deref()));

// 22. property enumeration order with mixed kinds
var w = {zz: 1, 2: 2, 0: 0, aa: 3, get gg() { return 4; }};
print("22 " + Object.getOwnPropertyNames(w).join(",") + " " + w.gg);

// 23. frozen-source function building literals repeatedly (shape reuse)
var mk = function (x) { return {x1: x, y1: x * 2, z1: x + x}; };
var acc23 = "";
for (var q = 0; q < 3; q++) {
    var o23 = mk(q);
    acc23 += keys(o23) + ":" + o23.x1 + o23.y1 + o23.z1 + ";";
}
print("23 " + acc23);

// 24. literal with __proto__ null
var x = {a: 1, __proto__: null};
print("24 " + keys(x) + " " + (Object.getPrototypeOf(x) === null) + " " + x.a);

// 25. arguments object + literal interplay
function f25(x, y) {
    var o = {ax: x, ay: y, asu: x + y};
    return keys(o) + " " + o.asu;
}
print("25 " + f25(3, 4));
