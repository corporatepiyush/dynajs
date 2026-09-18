/* shared bench helpers for sliced_waveB (concatenated by bench runs) */
function now_ns() {
  if (typeof performance !== "undefined" && performance.now)
    return performance.now() * 1e6;
  return Date.now() * 1e6;
}
function bench(name, ops, fn) {
  /* fixed-op loop, caller reps externally; measures with the highest
   * resolution clock available (performance.now where present) */
  var t0 = now_ns();
  var sink = fn(ops);
  var t1 = now_ns();
  var ns = (t1 - t0) / ops;
  print("BENCH " + name + " " + ns.toFixed(1) + " sink=" + sink);
  return sink;
}
/* build a long runtime (value) string: non-atom parent */
function mkval(n, seed) {
  var s = "";
  for (var i = 0; i < n; i++)
    s += String.fromCharCode(seed + (i % 26));
  return s;
}
/* force-flatten a value through JSON round-trip so we control representation */
function digest(x) {
  var s = "" + x, h = 0, i;
  for (i = 0; i < s.length; i++) { h = (h * 31 + s.charCodeAt(i)) | 0; }
  return h;
}
