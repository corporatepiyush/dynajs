function now() { return Date.now(); }

var src1k = new Array(1000); for (var i = 0; i < 1000; i++) src1k[i] = i;
var best = 1e9, reps = 300;
for (var r = 0; r < 3; r++) {
  var t0 = now();
  for (var i = 0; i < reps; i++) { var a = Array.from(src1k); if (a.length !== 1000) throw "bad"; }
  var t1 = now();
  var us = (t1 - t0) * 1000 / reps;
  if (us < best) best = us;
}
print("Array.from(dense1k): " + best.toFixed(3) + " us/call");

var u8 = new Uint8Array(100000); for (var i = 0; i < 100000; i++) u8[i] = i & 255;
best = 1e9; reps = 20;
for (var r = 0; r < 3; r++) {
  var t0 = now();
  for (var i = 0; i < reps; i++) { var a = Uint8Array.from(u8); if (a.length !== 100000) throw "bad"; }
  var t1 = now();
  var us = (t1 - t0) * 1000 / reps;
  if (us < best) best = us;
}
print("Uint8Array.from(u8_100k): " + best.toFixed(1) + " us/call");

var f64 = new Float64Array(100000); for (var i = 0; i < 100000; i++) f64[i] = i * 0.5;
best = 1e9; reps = 20;
for (var r = 0; r < 3; r++) {
  var t0 = now();
  for (var i = 0; i < reps; i++) { var a = Float64Array.from(f64); if (a.length !== 100000) throw "bad"; }
  var t1 = now();
  var us = (t1 - t0) * 1000 / reps;
  if (us < best) best = us;
}
print("Float64Array.from(f64_100k): " + best.toFixed(1) + " us/call");

best = 1e9; reps = 20;
for (var r = 0; r < 3; r++) {
  var t0 = now();
  for (var i = 0; i < reps; i++) { var a = Array.from(u8); if (a.length !== 100000) throw "bad"; }
  var t1 = now();
  var us = (t1 - t0) * 1000 / reps;
  if (us < best) best = us;
}
print("Array.from(u8_100k): " + best.toFixed(1) + " us/call");

var arr = new Array(1000); for (var i = 0; i < 1000; i++) arr[i] = i;
function bench_iter(name, make) {
  best = 1e9;
  var steps = 1000, rounds = 200;
  for (var r = 0; r < 3; r++) {
    var t0 = now();
    var acc = 0;
    for (var j = 0; j < rounds; j++) {
      var it = make();
      for (var i = 0; i < steps; i++) { var o = it.next(); if (o.done) throw "early"; acc += o.value; }
      var last = it.next(); if (!last.done) throw "not done";
    }
    var t1 = now();
    var ns = (t1 - t0) * 1e6 / (rounds * steps);
    if (ns < best) best = ns;
  }
  print(name + " per-step: " + best.toFixed(1) + " ns (acc=" + (acc & 1) + ")");
}
bench_iter("keys()",   function () { return arr.keys(); });
bench_iter("values()", function () { return arr.values(); });
bench_iter("entries()",function () { return arr.entries(); });

best = 1e9; var steps = 1000, rounds = 200;
for (var r = 0; r < 3; r++) {
  var t0 = now(); var acc = 0;
  for (var j = 0; j < rounds; j++) { for (var x of arr) acc += x; }
  var t1 = now();
  var ns = (t1 - t0) * 1e6 / (rounds * steps);
  if (ns < best) best = ns;
}
print("for-of per-step: " + best.toFixed(1) + " ns");

var best2 = 1e9;
for (var r = 0; r < 3; r++) {
  var t0 = now();
  for (var i = 0; i < 2000; i++) { var a = [...arr]; if (a.length !== 1000) throw "bad"; }
  var t1 = now();
  var us = (t1 - t0) * 1000 / 2000;
  if (us < best2) best2 = us;
}
print("spread 1k: " + best2.toFixed(3) + " us/call");
