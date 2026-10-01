function now_ns() {
  if (typeof performance !== "undefined" && performance.now)
    return performance.now() * 1e6;
  return Date.now() * 1e6;
}
function bench(name, ops, fn) {
  var t0 = now_ns();
  var sink = fn(ops);
  var t1 = now_ns();
  var ns = (t1 - t0) / ops;
  print("BENCH " + name + " " + ns.toFixed(1) + " sink=" + sink);
  return sink;
}
function mkval(n, seed) {
  var s = "";
  for (var i = 0; i < n; i++)
    s += String.fromCharCode(seed + (i % 26));
  return s;
}
function digest(x) {
  var s = "" + x, h = 0, i;
  for (i = 0; i < s.length; i++) { h = (h * 31 + s.charCodeAt(i)) | 0; }
  return h;
}
