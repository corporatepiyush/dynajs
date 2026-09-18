/* T3 bench: search primitives over SLICES (windows into value parents).
 * Pseudorandom haystack (LCG) so needles are UNIQUE — periodic content
 * makes every alignment match and measures nothing.
 * Slices: 1KB/16KB/64KB. Needle matrices: present-at-start/mid/end,
 * absent, needle crossing the window edge (=> absent), zero-length.
 * Narrow + wide + astral-window cases. */
function mkRnd(n, seed, hi) {
  /* LCG -> lowercase letters (or offset alphabet if hi) */
  var s = "";
  var x = seed | 0;
  for (var i = 0; i < n; i++) {
    x = (x * 1103515245 + 12345) & 0x7fffffff;
    s += String.fromCharCode((hi ? 65 : 97) + (x >>> 16) % 26);
  }
  return s;
}
var sink = 0;
var P = mkRnd(66000, 12345, false);      /* narrow parent */
var PW = "";                              /* wide parent */
{
  var x = 777;
  for (var i = 0; i < 33000; i++) {
    x = (x * 1103515245 + 12345) & 0x7fffffff;
    PW += String.fromCharCode(0x100 + (x >>> 16) % 500);
  }
}
var S1K = P.slice(100, 1124);
var S16K = P.slice(0, 16384);
var S64K = P.slice(0, 65536);
var SW16K = PW.slice(10, 10 + 16384);

var N_START = P.substring(0, 30);         /* occurs at 0 */
var N_MID = P.substring(30000, 30030);    /* occurs at 30000 */
var N_END = P.substring(65500, 65530);    /* occurs at 65500 */
var N_ABSENT = mkRnd(30, 999, true);      /* uppercase: never in P */
var N_EDGE = P.substring(65520, 65536) + "zz"; /* crosses slice end: absent */
var NW_MID = PW.substring(8000, 8030);

bench("T3_idx_64K_unique_mid30", 2000, function (ops) {
  var s = 0;
  for (var i = 0; i < ops; i++) s += S64K.indexOf(N_MID);
  return s;
});
bench("T3_idx_64K_unique_absent", 200, function (ops) {
  var s = 0;
  for (var i = 0; i < ops; i++) s += S64K.indexOf(N_ABSENT);
  return s;
});
bench("T3_idx_1K_unique_mid", 20000, function (ops) {
  var s = 0;
  for (var i = 0; i < ops; i++) s += S1K.indexOf(N_MID);
  return s;
});
bench("T3_lidx_64K_absent", 100, function (ops) {
  var s = 0;
  for (var i = 0; i < ops; i++) s += S64K.lastIndexOf(N_ABSENT);
  return s;
});
bench("T3_lidx_64K_at_start", 100, function (ops) {
  var s = 0;
  for (var i = 0; i < ops; i++) s += S64K.lastIndexOf(N_START);
  return s;
});
bench("T3_lidx_64K_at_end", 1000, function (ops) {
  var s = 0;
  for (var i = 0; i < ops; i++) s += S64K.lastIndexOf(N_END);
  return s;
});
bench("T3_lidx_1K_absent", 2000, function (ops) {
  var s = 0;
  for (var i = 0; i < ops; i++) s += S1K.lastIndexOf(N_ABSENT);
  return s;
});
bench("T3_incl_64K_unique_mid30", 2000, function (ops) {
  var s = 0;
  for (var i = 0; i < ops; i++) s += S64K.includes(N_MID) ? 1 : 0;
  return s;
});
bench("T3_startend_16K", 20000, function (ops) {
  var s = 0;
  for (var i = 0; i < ops; i++) s += (S16K.startsWith(N_START) ? 1 : 0) + (S16K.endsWith(N_END) ? 2 : 0);
  return s;
});
bench("T3_repl_16K_unique", 500, function (ops) {
  var s = 0;
  for (var i = 0; i < ops; i++) s = S16K.replace(N_MID, "XX").length;
  return s;
});
bench("T3_replall_16K_unique", 500, function (ops) {
  var s = 0;
  for (var i = 0; i < ops; i++) s = S16K.replaceAll(N_MID, "XX").length;
  return s;
});
bench("T3_idx_16K_edge_cross", 20000, function (ops) {
  var s = 0;
  for (var i = 0; i < ops; i++) s += S16K.indexOf(N_EDGE);
  return s;
});
bench("T3_idx_16K_zerolen", 50000, function (ops) {
  var s = 0;
  for (var i = 0; i < ops; i++) s += S16K.indexOf("");
  return s;
});
bench("T3_idx_wide_16K_unique_mid", 1000, function (ops) {
  var s = 0;
  for (var i = 0; i < ops; i++) s += SW16K.indexOf(NW_MID);
  return s;
});
bench("T3_lidx_wide_16K_absent", 100, function (ops) {
  var s = 0;
  var na = PW.substring(0, 30) + "\u0378"; /* \u0378 unassigned => absent */
  for (var i = 0; i < ops; i++) s += SW16K.lastIndexOf(na);
  return s;
});
/* astral: slice splits a surrogate pair at the window edge; a needle with
 * the full pair must not match across the split */
var ASTRAL = "";
{
  var x2 = 4242;
  for (var i = 0; i < 2048; i++) {
    x2 = (x2 * 1103515245 + 12345) & 0x7fffffff;
    ASTRAL += String.fromCharCode(0xD83D, 0xDE00 + (x2 % 26));
  }
}
var ASLICE = ASTRAL.slice(1, 4097); /* splits first pair at window start */
var ANEEDLE = ASLICE.substring(10, 13); /* contains 1.5 pairs (unique) */
bench("T3_idx_astral_window", 5000, function (ops) {
  var s = 0;
  for (var i = 0; i < ops; i++) s += ASLICE.indexOf(ANEEDLE);
  return s;
});
print("DIGEST " + sink);
