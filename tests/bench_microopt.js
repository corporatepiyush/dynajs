var MIN_MS = 25;
var REPS = 5;

var EMPTY_NS = (function () {
    var best = Infinity;
    for (var r = 0; r < 7; r++) {
        var t0 = performance.now();
        for (var i = 0; i < 3000000; i++);
        var dt = performance.now() - t0;
        if (dt < best) best = dt;
    }
    return best * 1e6 / 3000000;
})();

var results = {};

function bench(name, fn, ops, opts) {
    opts = opts || {};
    var setup = opts.setup;
    var arg = setup ? setup() : undefined;
    fn(arg);

    var mult = 1, dt;
    for (;;) {
        arg = setup ? setup() : undefined;
        var t0 = performance.now();
        for (var m = 0; m < mult; m++) fn(arg);
        dt = performance.now() - t0;
        if (dt >= MIN_MS || mult >= (1 << 20)) break;
        mult = Math.max(mult * 2, Math.ceil(mult * MIN_MS / Math.max(dt, 0.001)));
    }

    var best = Infinity;
    for (var r = 0; r < REPS; r++) {
        arg = setup ? setup() : undefined;
        var s = performance.now();
        for (var m2 = 0; m2 < mult; m2++) fn(arg);
        var d = performance.now() - s;
        if (d < best) best = d;
    }

    var totalOps = ops * mult;
    var ns = best * 1e6 / totalOps;
    var overhead = opts.jsLoop ? (EMPTY_NS * opts.jsLoop * mult) / totalOps : 0;
    var net = ns - overhead;
    var flag = best < MIN_MS * 0.5 ? "  !! below resolution target" : "";
    results[name] = net;
    print(name.padEnd(28) + net.toFixed(2).padStart(9) + " ns/op" +
          (overhead > 0.005 ? ("   (raw " + ns.toFixed(2) + ", -" + overhead.toFixed(2) + " loop)") : "") +
          flag);
    print("#DATA\t" + name + "\t" + net.toFixed(4) + "\t" + mult + "\t" + best.toFixed(3));
    return net;
}

print("empty JS loop iteration: " + EMPTY_NS.toFixed(2) + " ns  (subtracted where it applies)");
print("");

var N = 20000;
var A = new Array(N);
for (var i = 0; i < N; i++) A[i] = i;
var addOne = function (v) { return v + 1; };
var isEven = function (v) { return (v & 1) === 0; };
var isPos = function (v) { return v >= 0; };
var sum2 = function (a, v) { return a + v; };
var sink = 0;
var acc = function (v) { sink += v; };

bench("array.forEach", function () { A.forEach(acc); }, N);
bench("array.map", function () { return A.map(addOne); }, N);
bench("array.filter", function () { return A.filter(isEven); }, N);
bench("array.every", function () { return A.every(isPos); }, N);
bench("array.reduce", function () { return A.reduce(sum2, 0); }, N);

var AS = new Array(N);
for (var i = 0; i < N; i++) AS[i] = "s" + i;
var AF = new Array(N);
for (var i = 0; i < N; i++) AF[i] = i + 0.5;

bench("array.indexOf(int miss)", function () { return A.indexOf(-1); }, N);
bench("array.includes(int miss)", function () { return A.includes(-1); }, N);
bench("array.lastIndexOf(int miss)", function () { return A.lastIndexOf(-1); }, N);
bench("array.indexOf(str miss)", function () { return AS.indexOf("nope"); }, N);
bench("array.indexOf(float miss)", function () { return AF.indexOf(-1.5); }, N);

var S = new Array(5000);
for (var i = 0; i < S.length; i++) S[i] = "item" + i;
var W = new Array(2000);
for (var i = 0; i < W.length; i++) W[i] = "☃snowman☄item" + i;
var SHORT = ["alpha", "beta", "gamma", "delta"];

bench("array.join(',') 5000", function () { return S.join(","); }, S.length);
bench("array.join wide 2000", function () { return W.join(","); }, W.length);
bench("array.join short x1", function () { return SHORT.join("-"); }, 1);

bench("String.padEnd(200)", function () { return "x".padEnd(200, " "); }, 1);
bench("String.padStart(200)", function () { return "x".padStart(200, " "); }, 1);
bench("String.repeat(100x2)", function () { return "ab".repeat(100); }, 1);
bench("String.repeat(1 char)", function () { return "a".repeat(200); }, 1);

var recs = [];
for (var i = 0; i < 3000; i++)
    recs.push({ id: i, name: "record-name-" + i,
                path: "/some/longer/url/path/segment/" + i,
                flag: (i & 1) === 0, score: i * 1.5 });
var recsJson = JSON.stringify(recs);
var escArr = [];
for (var i = 0; i < 2000; i++) escArr.push("abc\td\ne\"f\\g" + i);
var escDense = [];
for (var i = 0; i < 2000; i++) escDense.push("\t\"\\\n\t\"\\\n\t\"\\\n" + i);
var ctlArr = [];
for (var i = 0; i < 1000; i++)
    ctlArr.push(String.fromCharCode(1, 2, 3, 4, 5, 6, 14, 15, 16, 17) + i);
var dateArr = [];
for (var i = 0; i < 2000; i++) dateArr.push(new Date(1750000000000 + i * 1000));

var flatNums = [], flatStrs = [], wideStrs = [], wideEsc = [];
for (var q = 0; q < 20000; q++) {
  flatNums.push(q * 3);
  flatStrs.push("item" + q);
  wideStrs.push("\u4e2d\u6587" + q + "\u3042\u3044");
  wideEsc.push("\u4e2d\"\n\u6587\\" + q);
}
bench("JSON.stringify flat nums", function () { return JSON.stringify(flatNums); }, flatNums.length);
bench("JSON.stringify flat strs", function () { return JSON.stringify(flatStrs); }, flatStrs.length);
bench("JSON.stringify wide strs", function () { return JSON.stringify(wideStrs); }, wideStrs.length);
bench("JSON.stringify wide esc", function () { return JSON.stringify(wideEsc); }, wideEsc.length);
bench("JSON.stringify records", function () { return JSON.stringify(recs); }, recs.length);
bench("JSON.parse records", function () { return JSON.parse(recsJson); }, recs.length);
bench("JSON.stringify escapes", function () { return JSON.stringify(escArr); }, escArr.length);
bench("JSON.stringify esc dense", function () { return JSON.stringify(escDense); }, escDense.length);
bench("JSON.stringify ctl esc", function () { return JSON.stringify(ctlArr); }, ctlArr.length);
bench("JSON.stringify dates", function () { return JSON.stringify(dateArr); }, dateArr.length);

var batch = 0;
function freshKeys(n, prefix) {
    return function () {
        var b = batch++, a = new Array(n);
        for (var i = 0; i < n; i++) a[i] = prefix + b + "_" + i;
        return a;
    };
}
bench("atom intern (short key)", function (keys) {
    var o = {};
    for (var k = 0; k < keys.length; k++) o[keys[k]] = k;
    return o;
}, 5000, { setup: freshKeys(5000, "pn"), jsLoop: 5000 });

bench("atom intern (long key)", function (keys) {
    var o = {};
    for (var k = 0; k < keys.length; k++) o[keys[k]] = k;
    return o;
}, 5000, { setup: freshKeys(5000, "a_rather_long_property_name_for_hashing_"), jsLoop: 5000 });

var internedKeys = freshKeys(5000, "fixed")();
(function () { var o = {}; for (var k = 0; k < internedKeys.length; k++) o[internedKeys[k]] = k; })();
bench("interned key set (control)", function () {
    var o = {};
    for (var k = 0; k < internedKeys.length; k++) o[internedKeys[k]] = k;
    return o;
}, internedKeys.length, { jsLoop: internedKeys.length });

var jbatch = 0;
bench("JSON.parse fresh keys", function (src) { return JSON.parse(src); }, 2000, {
    setup: function () {
        var b = jbatch++, parts = [];
        for (var k = 0; k < 2000; k++) parts.push('"key' + b + "_" + k + '":' + k);
        return "{" + parts.join(",") + "}";
    }
});

function f0() { return 1; }
function f3(a, b, c) { return a + b + c; }
bench("call f0()", function () {
    var s = 0;
    for (var k = 0; k < 100000; k++) s += f0();
    return s;
}, 100000, { jsLoop: 100000 });
bench("call f3(a,b,c)", function () {
    var s = 0;
    for (var k = 0; k < 100000; k++) s += f3(k, 1, 2);
    return s;
}, 100000, { jsLoop: 100000 });

var F2000 = [];
for (var i = 0; i < 2000; i++) F2000.push(new Function("a", "return a+" + i));
bench("call 2000 distinct fns", function () {
    var s = 0;
    for (var i2 = 0; i2 < F2000.length; i2++) s += F2000[i2](i2);
    return s;
}, F2000.length, { jsLoop: F2000.length });

bench("object churn {a,b}", function () {
    var last = null;
    for (var k = 0; k < 100000; k++) last = { a: k, b: k + 1 };
    return last;
}, 100000, { jsLoop: 100000 });

var d = new Date(1750000000000);
bench("Date.toISOString", function () { return d.toISOString(); }, 1);
bench("Date.toUTCString", function () { return d.toUTCString(); }, 1);
bench("Date.toString", function () { return d.toString(); }, 1);
bench("Date.toLocaleString", function () { return d.toLocaleString(); }, 1);

bench("obj literal 9 fields", function () {
    var o = null;
    for (var k = 0; k < 20000; k++)
        o = { id: k, uuid: "u", name: "n", path: "p", email: "e",
              active: true, score: 1.5, tags: null, created: 0 };
    return o;
}, 20000, { jsLoop: 20000 });

bench("obj dynamic 9 fields", function () {
    var o = null;
    for (var k = 0; k < 20000; k++) {
        o = {};
        o.id = k; o.uuid = "u"; o.name = "n"; o.path = "p"; o.email = "e";
        o.active = true; o.score = 1.5; o.tags = null; o.created = 0;
    }
    return o;
}, 20000, { jsLoop: 20000 });

var recJson9 = JSON.stringify(recs.slice(0, 1000));
bench("JSON.parse 9-field recs", function () { return JSON.parse(recJson9); }, 1000);
