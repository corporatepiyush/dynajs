/* shared helpers (concatenated by the runner via include-like eval) */
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
function parent(n, seed) {
  var s = "";
  for (var i = 0; i < n; i++) s += String.fromCharCode(seed !== undefined ? seed : 97 + (i % 26));
  return s;
}
function parentWide(n, seed) {
  var s = "";
  for (var i = 0; i < n; i++) s += String.fromCharCode(0x100 + ((i * 7) % 500));
  return s;
}
