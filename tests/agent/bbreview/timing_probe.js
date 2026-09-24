// TIMING PROBE (not diffed): localeCompare fast path + split cache + spread + sqrt
function now() { return Date.now(); }
const w1 = "\u00e9\u4e2d\u6587".repeat(70).slice(0, 200);
const w2 = "\u00e9\u4e2d\u6587".repeat(70).slice(0, 200);
let sink = 0;
let t0 = now();
for (let i = 0; i < 10000; i++) sink += w1.localeCompare(w2);
let t1 = now();
console.log("localeCompare 200-wide equal: " + ((t1 - t0)) + "ms/10k (" + ((t1 - t0) * 100) + "ns/call) sink=" + sink);
const e1 = "e\u0301".repeat(100);
const e2 = "\u00e9".repeat(100);
t0 = now();
for (let i = 0; i < 10000; i++) sink += e1.localeCompare(e2);
t1 = now();
console.log("localeCompare NFC/NFD-200: " + ((t1 - t0)) + "ms/10k sink=" + sink);
const big = "abcdefghij".repeat(10000);
t0 = now();
const p1 = big.split("");
t1 = now();
const p2 = big.split("");
const t2 = now();
console.log("split 100k: first=" + (t1 - t0) + "ms second=" + (t2 - t1) + "ms lens=" + p1.length + "/" + p2.length);
const arr10k = new Array(10000).fill("x");
t0 = now();
JSON.stringify(arr10k);
t1 = now();
console.log("JSON 10k chars: " + (t1 - t0) + "ms");
const bigA = new Array(100000).fill(7);
t0 = now();
const cp = [...bigA];
t1 = now();
console.log("spread 100k: " + (t1 - t0) + "ms len=" + cp.length);
t0 = now();
let acc = 0;
let x = 5e-324;
const f64 = new Float64Array(1);
for (let e = 0; e < 2098; e++) { f64[0] = Math.sqrt(x); acc += f64[0] > 0 ? 1 : 0; x *= 2; }
t1 = now();
console.log("sqrt 2098 powers: " + (t1 - t0) + "ms acc=" + acc);
