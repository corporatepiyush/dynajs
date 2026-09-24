// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["rtfails=0 hash=8d34fa8e"];
// test_dtoa_shortest_roundtrip_10k.js — the shortest-repr correctness invariant:
// for 10k seeded random doubles, Number(String(v)) === v MUST hold on every
// engine (round-trip). Rolling hash of all reprs gives node-oracle coverage.
function mulberry32(seed){var s=seed>>>0;return function(){s=(s+0x6D2B79F5)|0;var t=Math.imul(s^(s>>>15),1|s);t=(t+Math.imul(t^(t>>>7),61|t))^t;return (t^(t>>>14))>>>0;};}
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var f64 = new Float64Array(1);
var u32 = new Uint32Array(f64.buffer);
function fromBits(hi, lo) { u32[1] = hi; u32[0] = lo; return f64[0]; }
var r = mulberry32(0xB105);
var rtfails = 0, feed = "";
for (var i = 0; i < 10000; i++) {
  var v = fromBits(r() % 0x100000, r());
  if (v !== v) { feed += "nan\n"; continue; }
  var s = String(v);
  if (Number(s) !== v) { rtfails++; if (rtfails < 6) __L(0, "RTFAIL bits=" + u32[1].toString(16) + u32[0].toString(16) + " s=" + s); }
  feed += s + "\n";
}
__L(1, "rtfails=" + rtfails + " hash=" + fnv(feed));

summary("builtins_ext");
