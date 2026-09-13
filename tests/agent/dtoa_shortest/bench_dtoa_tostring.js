// dtoa perf bench: bench_dtoa_tostring.js <case>
// Portable across dynajs (scriptArgs) and node (process.argv).
// Cases: big | ord | small | e21 | json | tofix
// Prints "<case> ms=<ms> ops=<n> ns_per_op=<ns> sink=<len>".
var out = (typeof print === "function") ? print : console.log;
var A = (typeof scriptArgs !== "undefined") ? scriptArgs : process.argv.slice(1);
var which = (A.length > 1) ? A[1] : "big";

var t0 = Date.now();
var sink = 0;
var n = 0;
if (which === "big") {
    n = 300000;
    for (var i = 0; i < n; i++) { sink += (1e300).toString().length; }
} else if (which === "ord") {
    var u32 = new Uint32Array(2), f64 = new Float64Array(u32.buffer);
    var s = 42 >>> 0;
    var arr = [];
    for (var i2 = 0; i2 < 4096; i2++) {
        s = (Math.imul(s, 1664525) + 1013904223) >>> 0; u32[1] = (s & 0x7fffffff) | 0x30000000;
        s = (Math.imul(s, 1664525) + 1013904223) >>> 0; u32[0] = s;
        arr.push(f64[0]);
    }
    n = 400000;
    for (var i3 = 0; i3 < n; i3++) { sink += arr[i3 & 4095].toString().length; }
} else if (which === "small") {
    n = 2000000;
    for (var i4 = 0; i4 < n; i4++) { sink += (0.1).toString().length; }
} else if (which === "e21") {
    n = 2000000;
    for (var i5 = 0; i5 < n; i5++) { sink += String(1e21).length; }
} else if (which === "json") {
    var u322 = new Uint32Array(2), f642 = new Float64Array(u322.buffer);
    var s2 = 7 >>> 0;
    var arr2 = [];
    for (var i6 = 0; i6 < 60000; i6++) {
        s2 = (Math.imul(s2, 1664525) + 1013904223) >>> 0; u322[1] = (s2 & 0x7fffffff) | 0x38000000;
        s2 = (Math.imul(s2, 1664525) + 1013904223) >>> 0; u322[0] = s2;
        arr2.push(f642[0]);
    }
    var str = JSON.stringify(arr2);
    sink = str.length;
    n = 60000;
} else if (which === "tofix") {
    n = 2000000;
    for (var i7 = 0; i7 < n; i7++) { sink += (1234.5678).toFixed(2).length; }
} else {
    out("unknown case: " + which);
}
var t1 = Date.now();
out(which + " ms=" + (t1 - t0) + " ops=" + n + " ns_per_op=" + (n > 0 ? Math.round((t1 - t0) * 1e6 / n) : 0) + " sink=" + sink);
