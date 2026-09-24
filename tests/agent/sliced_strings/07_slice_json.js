// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["PASS 07_slice_json"];
/* JSON.stringify/parse over slices. */
var s = parent(1000);
var t = s.slice(50, 800);
eq(JSON.stringify(t), JSON.stringify(s.slice(50, 800)), "stringify slice");
eq(JSON.parse(JSON.stringify(t)), t, "stringify/parse roundtrip");
var obj = { key: t, nested: [t, { v: t.slice(5, 50) }] };
var str = JSON.stringify(obj);
eq(JSON.parse(str).key, t, "object field roundtrip");
eq(JSON.parse(str).nested[1].v, t.slice(5, 50), "nested slice roundtrip");
eq(JSON.parse(str).nested[1].v.length, 45, "nested len");
/* quotes/escapes inside slices */
var esc = 'a"b\\c\nd\te'.repeat(200);
var es = esc.slice(3, 900);
eq(JSON.parse(JSON.stringify(es)), es, "escaped slice roundtrip");
ok(JSON.stringify(es).indexOf("\\n") > 0, "escape present");
/* unicode escapes in slices */
var uni = "\u00e9\u4e16\ud83d\ude00".repeat(300).slice(2, 800);
eq(JSON.parse(JSON.stringify(uni)), uni, "unicode slice roundtrip");
/* JSON.stringify indent gap path uses substrings internally */
eq(JSON.stringify({ a: 1, b: t }, null, "0123456789abc").length > 0, true, "long indent");
/* toJSON skip */
eq(JSON.stringify({ toJSON: function () { return t; } }), JSON.stringify(t), "toJSON slice");
__L(0, "PASS 07_slice_json");

summary("sliced_strings");
