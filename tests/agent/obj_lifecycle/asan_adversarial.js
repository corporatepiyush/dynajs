function log(s) { print(s); }

var o1 = {a: 1, b: 2, c: 3};
Object.defineProperty(o1, "b", {get() { return 42; }});
log("1 " + o1.a + " " + o1.b + " " + o1.c + " " + Object.keys(o1).join(","));

var g1 = 1, g2 = 2;
function redeclare() {}
var withObj = {vv: 5};
try {
    with (withObj) {
        var vvCapture = vv;
    }
    log("2 " + vvCapture);
} catch (e) {
    log("2 with-err " + e.message);
}

var o3 = {x: 1};
for (var i = 0; i < 50; i++) {
    o3["p" + i] = {deep: i};
    o3["q" + i] = [i, i + 1];
}
log("3 " + o3.x + " " + o3.p49.deep + " " + o3.q49[1] + " " + Object.keys(o3).length);

var wr1 = new WeakRef({w1: 1, w2: 2});
log("4 " + (wr1.deref() === undefined));

var fr = new FinalizationRegistry(function (h) { log("5 cleaned " + h); });
for (var j = 0; j < 100; j++) {
    var tmp = {k: j};
    fr.register(tmp, j);
}
log("5 registered");

(function () {
    var acc = 0;
    for (var i = 0; i < 2000; i++) {
        var tree = {
            n1: {a: i, b: {c: i + 1}},
            n2: {d: [i, {e: i + 2}]},
            n3: "str" + i
        };
        acc += tree.n1.a + tree.n1.b.c + tree.n2.d[1].e + tree.n3.length;
    }
    log("6 " + acc);
})();

(function () {
    var o7 = {};
    for (var i = 0; i < 64; i++) o7["f" + i] = i;
    for (var i = 0; i < 64; i += 2) delete o7["f" + i];
    log("7 " + Object.keys(o7).length + " " + o7.f1 + " " + o7.f63);
})();

var mk = function (x) { var o = {m1: x, m2: x * 2}; return o; };
var o8 = mk(3);
Object.defineProperty(o8, "m1", {get() { return 7; }});
log("8 " + o8.m1 + " " + o8.m2);
var acc8 = 0;
for (var i = 0; i < 10000; i++) {
    var t = mk(i);
    acc8 += t.m1 + t.m2;
}
log("8b " + acc8);

var acc9 = 0;
for (var i = 0; i < 5000; i++) {
    var f = Object.freeze({z1: i, z2: i + 1});
    acc9 += f.z1;
}
log("9 " + acc9);

(function () {
    var sym = Symbol("s10");
    var o10 = {p: 1, q: 2};
    o10[sym] = 3;
    Object.defineProperty(o10, "q", {set(v) {}});
    Object.seal(o10);
    log("10 " + o10.p + " " + (o10[sym] === 3));
})();

var o11 = {h1: 1};
log("11 done");

var u = {id: "u1"};
log("12 " + u.id + " " + typeof u.id);
