/* T4 bench: wide (UTF-16) strings/slices whose content is all-latin1.
 * A leading >0xFF char forces the parent to be stored WIDE; slicing past
 * it yields wide slices with narrowable content.
 * Cases: toLowerCase / compare / concat(+downstream search) / atom
 * creation / JSON / charCodeAt. */
function mkWideNarrowable(n) {
  /* parent stored wide (leading 0x1450), remaining content all <= 0xFF */
  var s = "\u1450";
  for (var i = 0; i < n; i++)
    s += String.fromCharCode(0xF7 - (i % 90));
  return s;
}
var sink = 0;
var W = mkWideNarrowable(25600);
var WS = W.slice(1, W.length - 100);       /* wide slice, narrowable */
var WS1K = W.slice(1, 1025);               /* wide slice 1024, narrowable */
var CONC = WS1K + "0123456789";            /* narrow after T4, wide before */
var NEEDLE = WS1K.substring(100, 113) + "qq"; /* absent in CONC */

bench("T4_tolower_wide_slice_25K", 50, function (ops) {
  var s = 0;
  for (var i = 0; i < ops; i++) s += WS.toLowerCase().length;
  return s;
});
bench("T4_compare_wide_slice_pair", 2000, function (ops) {
  var a = W.slice(0, 10000), b = W.slice(0, 10000);
  var s = 0;
  for (var i = 0; i < ops; i++) s += (a < b) ? 1 : (a === b ? 2 : 0);
  return s;
});
bench("T4_concat_short_wide_slice", 100000, function (ops) {
  var s = 0;
  for (var i = 0; i < ops; i++) s += (WS1K + "x").length;
  return s;
});
bench("T4_concat_then_search_1K", 2000, function (ops) {
  var s = 0;
  for (var i = 0; i < ops; i++) s += CONC.indexOf(NEEDLE);
  return s;
});
bench("T4_concat_slice_slice", 2000, function (ops) {
  var s = 0;
  for (var i = 0; i < ops; i++) s += (WS1K + WS1K).length;
  return s;
});
bench("T4_atom_from_wide_slice_1K", 20000, function (ops) {
  var o = {};
  var s = 0;
  for (var i = 0; i < ops; i++) { o[WS1K + i] = 1; s += 1; }
  return s;
});
bench("T4_json_wide_slice_25K", 100, function (ops) {
  var s = 0;
  for (var i = 0; i < ops; i++) s += JSON.stringify(WS).length;
  return s;
});
bench("T4_charcode_wide_slice_25K", 500, function (ops) {
  var s = 0;
  for (var i = 0; i < ops; i++) s += WS.charCodeAt(5000);
  return s;
});
print("DIGEST " + sink);
