/* sliced_waveB suite-local helpers (concatenated between _h/h.js and probe) */
function eq(a, b, msg) {
  if (a !== b) throw new Error((msg || "eq") + ": got " + JSON.stringify(a) + " want " + JSON.stringify(b));
}
function ok(cond, msg) {
  if (!cond) throw new Error("failed: " + msg);
}
function deepEq(a, b, msg) {
  var ja = JSON.stringify(a), jb = JSON.stringify(b);
  if (ja !== jb) throw new Error((msg || "deepEq") + ": got " + ja + " want " + jb);
}
function mkvalRnd(n, seed, hi) {
  /* LCG pseudorandom string: unique-ish content, no short-period aliasing */
  var s = "";
  var x = seed | 0;
  for (var i = 0; i < n; i++) {
    x = (x * 1103515245 + 12345) & 0x7fffffff;
    s += String.fromCharCode((hi ? 65 : 97) + (x >>> 16) % 26);
  }
  return s;
}
function mkWideRnd(n, seed) {
  var s = "";
  var x = seed | 0;
  for (var i = 0; i < n; i++) {
    x = (x * 1103515245 + 12345) & 0x7fffffff;
    s += String.fromCharCode(0x100 + (x >>> 16) % 500);
  }
  return s;
}
/* wide parent with all-latin1 tail: for narrowable-wide-slice matrices */
function mkWideNarrowable(n) {
  var s = "\u1450"; /* forces wide storage */
  for (var i = 0; i < n; i++)
    s += String.fromCharCode(0xF7 - (i % 90));
  return s;
}
/* ES-spec reference for String.prototype.lastIndexOf (no coercion drama:
   inputs are already strings/numbers we control) */
function refLastIndexOf(s, needle, from) {
  var n = s.length, m = needle.length;
  if (arguments.length < 3 || from === undefined || from !== from)
    from = Infinity;
  var k = Math.min(Math.max(from, 0), n - m);
  if (n - m < 0) return -1; /* needle longer than haystack */
  while (k >= 0) {
    if (s.substring(k, k + m) === needle) return k;
    k--;
  }
  return -1;
}
