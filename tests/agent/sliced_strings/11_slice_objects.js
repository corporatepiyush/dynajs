// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["PASS 11_slice_objects"];
/* String objects wrapping slices. */
var s = parent(800);
var t = s.slice(20, 400);
var o = new Object(t);
eq(o.length, 380, "wrapper length");
eq(o[0], s[20], "wrapper index");
eq(o.valueOf(), t, "wrapper valueOf");
eq(o.toString(), t, "wrapper toString");
eq(typeof o, "object", "wrapper typeof");
ok(o instanceof String, "instanceof String");
eq(Object.keys(o).length, 380, "Object.keys on String wrapper of slice");
var ks = Object.keys(o);
eq(ks[0], "0", "keys start");
eq(ks[ks.length - 1], "379", "keys end");
eq(JSON.parse(JSON.stringify(o)), t, "wrapper json");
/* primitive slice through String() and methods on wrapper */
eq(String(o).slice(1, 10), s.slice(21, 30), "wrapper->slice chain");
eq(o.charAt(3), s.charAt(23), "wrapper charAt");
eq(o.toUpperCase(), t.toUpperCase(), "wrapper toUpperCase");
/* property add on wrapper does not affect slice */
o.extra = 1;
eq(o.extra, 1, "wrapper prop");
eq(t, s.slice(20, 400), "slice unchanged");
/* concat of wrapper coerces */
eq(o + "!", t + "!", "wrapper concat");
__L(0, "PASS 11_slice_objects");

summary("sliced_strings");
