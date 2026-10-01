function keys(o) { return Object.keys(o).join(","); }
function desc(o, k) {
    var d = Object.getOwnPropertyDescriptor(o, k);
    if (d === undefined) return "none";
    return (d.writable ? "w" : "-") + (d.enumerable ? "e" : "-") +
           (d.configurable ? "c" : "-") + ":" + typeof d.value + ":" +
           (d.get ? "G" : "") + (d.set ? "S" : "");
}

var a = {x: 1, y: 2, z: 3};
print("1 " + keys(a) + " " + desc(a, "x") + " " + a.x + a.y + a.z);

var v = {p: 1, q: 2};
var b = {m: v.p + 1, n: v.q * 2, o: v.p * v.q};
print("2 " + keys(b) + " " + b.m + "," + b.n + "," + b.o + " " + desc(b, "m"));

var c = {outer: {inner: 1}, after: 2};
print("3 " + keys(c) + " " + keys(c.outer) + " " + c.outer.inner + c.after);

var xs = 1;
var d = {xs, ys: xs + 1, f() { return this.xs + this.ys; }};
print("4 " + keys(d) + " " + d.f());

var e = {
    _v: 1,
    get g() { return this._v + 1; },
    set g(x) { this._v = x; },
    h: 2
};
e.g = 10;
print("5 " + keys(e) + " " + e.g + " " + desc(e, "g") + " " + desc(e, "_v"));

var kn = "ke" + "y";
var f = {[kn]: 1, other: 2};
print("6 " + keys(f) + " " + f[kn]);

var g = {...v, extra: 3};
print("7 " + keys(g) + " " + g.p + g.q + g.extra);

var h = {0: "a", 1: "b", normal: 1};
print("8 " + keys(h) + " " + h[0] + h[1] + h.normal);

var i = {dup: 1, mid: 2, dup: 3};
print("9 " + keys(i) + " " + i.dup + " " + i.mid);

var protoMark = {pm: 42};
var j = {first: 1, __proto__: protoMark, second: 2};
print("10 " + keys(j) + " " + j.first + j.second + " " + j.pm + " " +
      Object.getPrototypeOf(j) === protoMark ? "protoOK" : "protoBAD");
print("10b " + (Object.getPrototypeOf(j) === protoMark));

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

var acc = "";
for (var loop = 0; loop < 3; loop++) {
    var L = {q: loop, r: loop * 2, s: "s" + loop};
    acc += keys(L) + ":" + L.q + L.r + L.s + ";";
}
print("12 " + acc);

var m = {u: 1, w: 2};
Object.defineProperty(m, "u", {get() { return 99; }});
print("13 " + keys(m) + " " + m.u + m.w + " " + desc(m, "u"));

(function () {
    var n = {u: 1, w: 2};
    n.child = {deep: true};
    n.getter = function () {};
    Object.defineProperty(n, "acc", {get() { return 1; }});
    print("14 " + keys(n));
})();

var o = {u: 1, w: 2};
Object.freeze(o);
try { o.u = 5; } catch (err) {}
print("15 " + o.u + " " + desc(o, "u"));

var p = {q1: 1, q2: 2};
print("16 " + keys(p) + " " + JSON.stringify(p));

var r = {f1: 1, f2: 2, f3: 3, f4: 4, f5: 5, f6: 6, f7: 7, f8: 8, f9: 9};
print("17 " + keys(r) + " " + (r.f1 + r.f9));

var s = {a1: 1, b2: 2};
print("18 " + keys(s) + " " + (Object.getPrototypeOf(s) === Object.prototype));

var t;
try {
    t = {k1: 1, k2: missing(), k3: 3};
} catch (err) {
    t = "thrown";
}
print("19 " + t);

var u = eval("({ev1: 7, ev2: 8})");
print("20 " + keys(u) + " " + u.ev1 + u.ev2);

var wr = {alive: 1};
var ref = new WeakRef(wr);
print("21 " + (ref.deref() === wr) + " " + keys(ref.deref()));

var w = {zz: 1, 2: 2, 0: 0, aa: 3, get gg() { return 4; }};
print("22 " + Object.getOwnPropertyNames(w).join(",") + " " + w.gg);

var mk = function (x) { return {x1: x, y1: x * 2, z1: x + x}; };
var acc23 = "";
for (var q = 0; q < 3; q++) {
    var o23 = mk(q);
    acc23 += keys(o23) + ":" + o23.x1 + o23.y1 + o23.z1 + ";";
}
print("23 " + acc23);

var x = {a: 1, __proto__: null};
print("24 " + keys(x) + " " + (Object.getPrototypeOf(x) === null) + " " + x.a);

function f25(x, y) {
    var o = {ax: x, ay: y, asu: x + y};
    return keys(o) + " " + o.asu;
}
print("25 " + f25(3, 4));
