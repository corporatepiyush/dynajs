__EXP = {};
__EXP[0] = ["PASS 19_slice_generics"];
var s = parent(600);
var t = s.slice(10, 400);
eq(String.prototype.slice.call(t, 5, 10), s.slice(15, 20), "generic slice");
eq(String.prototype.charCodeAt.call(t, 3), s.charCodeAt(13), "generic charCodeAt");
eq(String.prototype.indexOf.call(t, s.slice(20, 25)), 10, "generic indexOf with slice needle");
eq(String.prototype.concat.call(t, "!", "!"), t + "!!", "generic concat");
eq(String.prototype.repeat.call(t.slice(0, 3), 4), s.slice(10, 13).repeat(4), "generic repeat");
var re = new RegExp(s.slice(15, 20));
ok(re.test(t), "regexp built from slice source");
eq(Math.max.apply(null, [1, 2, 3].map(String)), 3, "slices as args");
eq(typeof t.slice, "function", "slice method on string");
eq(t.constructor, String, "constructor");
eq(Object.prototype.toString.call(t), "[object String]", "toStringTag");
__L(0, "PASS 19_slice_generics");

summary("sliced_strings");
