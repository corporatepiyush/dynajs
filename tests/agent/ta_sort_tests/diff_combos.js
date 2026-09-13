/* Differential driver: prints one canonical line per (array, comparator)
   combo. Run under ./dynajs and under the pristine control binary; the
   outputs must be byte-identical (proves exact revert-to-pre-15f3b24
   semantics). */
var out = (typeof print === 'function') ? print : console.log;

var arrays = {
  a5:  [3.5, NaN, -1, 2, 0],
  a3:  [1, NaN, 2],
  aNN: [NaN, NaN, 1]
};

var cmps = {
  'x-y':    function (a, b) { return a - b; },
  'neg1':   function () { return -1; },
  'zero':   function () { return 0; },
  'nan':    function () { return NaN; },
  'undef':  undefined,
  'nanFirst': function (a, b) {
    var an = (a !== a), bn = (b !== b);
    if (an && bn) return 0;
    if (an) return -1;
    if (bn) return 1;
    return a < b ? -1 : (a > b ? 1 : 0);
  }
};

function fmt(v) {
  if (v !== v) return "NaN";
  if (v === 0) return (1 / v === -Infinity) ? "-0" : "0";
  return String(v);
}

Object.keys(arrays).forEach(function (an) {
  Object.keys(cmps).forEach(function (cn) {
    var ta = new Float64Array(arrays[an]);
    ta.sort(cmps[cn]);
    var parts = [];
    for (var i = 0; i < ta.length; i++) parts.push(fmt(ta[i]));
    out(an + " x " + cn + " => [" + parts.join(", ") + "]");
  });
});
/* default-sort spot on plain numeric array for sanity (NaN last) */
out("plain-default => [" + [3, 1, 2].sort().join(", ") + "]");
