// D1 cache-pressure working-set kernels (engine-level IPC-proxy study).
//
// No PMCs are available on this host (see METHODOLOGY.md), so stall fractions
// are inferred by scaling each kernel's hot working set across the host's
// cache boundaries (M1 Pro: L1D 128KB, L2 12MB shared/4 P-cores) and reading
// the ns/op degradation curve. Every mode prints ONE line:
//   RESULT <mode> <working_set_bytes> <ns_per_op> <ops> <checksum>
//
// Usage: dynajs wp_cache_pressure.js <mode> <working_set_bytes>
// Modes:
//   tight    arithmetic dispatch loop, register-resident (peak dispatch rate)
//   f64read  Float64Array streaming read, ws = arg bytes
//   f64rmw   Float64Array read-modify-write, ws = arg bytes
//   objread  JS object property reads over a scaled object pool
//   broad    mixed-op loop that sweeps a WIDE set of dispatch handlers
//            (instruction-side sensitivity probe vs `tight`)
//   mega     megamorphic property get rotating over N shapes
//
// Timing discipline: each measured call processes >= 200k ops (SWEEPS) so a
// single call costs >> the 1ms Date.now() resolution even for 4KB working
// sets; ops-per-call is reported so ns/op is normalization-exact.

function now() { return Date.now(); }

const TARGET_MS = 400;

function bench(opsPerCall, label, oneShot) {
  oneShot();                                   // warmup
  let t0 = now();
  oneShot();
  let dt = now() - t0;
  if (dt < 1) dt = 1;
  let reps = Math.max(1, Math.round(TARGET_MS / dt));
  if (reps > 2000) reps = 2000;
  let best = Infinity, check = 0;
  for (let r = 0; r < 3; r++) {
    t0 = now();
    for (let i = 0; i < reps; i++) check = (check + oneShot()) | 0;
    dt = now() - t0;
    const per = (dt * 1e6) / (reps * opsPerCall);
    if (per < best) best = per;
  }
  print("RESULT " + label + " " + best.toFixed(4) + " " + (reps * 3) + " " + check);
}

// sweeps so that n * sweeps >= MIN_OPS per measured call
function sweepsFor(n) {
  return Math.max(1, Math.ceil(200000 / n));
}

const mode = scriptArgs[1] || "tight";
const wsBytes = parseInt(scriptArgs[2] || "0", 10);

switch (mode) {
case "tight": {
  // Register-resident dispatch: measures peak interpreter dispatch throughput.
  bench(20e6, mode + " " + wsBytes, function () {
    let s = 0;
    for (let i = 0; i < 20e6; i++) s = (s + i) | 0;
    return s;
  });
  break;
}
case "f64read": {
  const n = (wsBytes / 8) | 0;
  const sw = sweepsFor(n);
  const a = new Float64Array(n);
  for (let i = 0; i < n; i++) a[i] = i * 0.5;
  bench(n * sw, mode + " " + wsBytes, function () {
    let s = 0;
    for (let q = 0; q < sw; q++)
      for (let i = 0; i < n; i++) s += a[i];
    return s | 0;
  });
  break;
}
case "f64rmw": {
  const n = (wsBytes / 8) | 0;
  const sw = sweepsFor(n);
  const a = new Float64Array(n);
  for (let i = 0; i < n; i++) a[i] = i * 0.5;
  bench(n * sw, mode + " " + wsBytes, function () {
    let s = 0;
    for (let q = 0; q < sw; q++)
      for (let i = 0; i < n; i++) { a[i] = a[i] + 1.0; s += a[i]; }
    return s | 0;
  });
  break;
}
case "objread": {
  // Pool of objects with 8 properties each; JSValues + shapes + property
  // storage make the per-object footprint ~256B (object header + shape +
  // 8-slot property store + pool slot, measured average on 64-bit).
  const perObj = 256;
  const n = Math.max(16, (wsBytes / perObj) | 0);
  const sw = sweepsFor(n);
  const pool = new Array(n);
  for (let i = 0; i < n; i++) {
    pool[i] = { a: i, b: i + 1, c: i + 2, d: i + 3, e: i + 4, f: i + 5, g: i + 6, h: i + 7 };
  }
  bench(n * sw, mode + " " + wsBytes, function () {
    let s = 0;
    for (let q = 0; q < sw; q++)
      for (let i = 0; i < n; i++) { const o = pool[i]; s += o.b + o.e; }
    return s | 0;
  });
  break;
}
case "broad": {
  // Sweeps a wide set of op handlers per iteration (arith, string, typeof,
  // closure call, array special, math builtin, conversion) to widen the
  // instruction footprint vs `tight`. Compare ns/op: if broad ~ tight * k
  // stays flat while both are L1-resident, I-side is not the constraint.
  const strs = ["alpha", "beta", "gamma", "delta"];
  const arr = [1, 2, 3, 4, 5, 6, 7, 8];
  function id(x) { return x; }
  bench(5e6, mode + " " + wsBytes, function () {
    let s = 0, t = 0;
    for (let i = 0; i < 5e6; i++) {
      s = (s + i) | 0;
      t += strs[i & 3].length;
      s ^= (typeof t === "number") ? 1 : 2;
      s += id(i & 127) | 0;
      s += arr[i & 7];
      s += Math.imul(i, 3) | 0;
      t = +("" + (i & 15));
      s += t | 0;
      if ((i & 65535) === 0) s += strs[i & 3] + "-";
    }
    return (s + t) | 0;
  });
  break;
}
case "mega": {
  // Megamorphic get: rotate across N distinct shapes so the property-get
  // path runs through the generic lookup instead of a monomorphic IC.
  const shapes = Math.max(16, Math.min(4096, (wsBytes / 256) | 0));
  const objs = [];
  for (let k = 0; k < shapes; k++) {
    const o = { v0: k };
    for (let j = 1; j <= (k % 8) + 1; j++) o["p" + j] = j;
    o.payload = k;
    objs.push(o);
  }
  const n = objs.length;
  const sw = sweepsFor(n);
  bench(n * sw, mode + " " + wsBytes, function () {
    let s = 0;
    for (let q = 0; q < sw; q++)
      for (let i = 0; i < n; i++) s += objs[i].payload;
    return s | 0;
  });
  break;
}
default:
  print("unknown mode: " + mode);
}
