import { sort } from "dyna:semver";
import { eq as EQ, ok as OK, throws as TH, done as DONE } from "./harness.js";

// subclass array
class VArr extends Array {}
var sa = new VArr("2.0.0", "1.0.0");
var ret = sort(sa);
OK(ret === sa, "subclass array returned as-is");
EQ(ret.length, 2, "subclass length");
EQ(ret[0], "1.0.0", "subclass sorted [0]");
OK(sa instanceof VArr, "subclass identity kept");

// sparse array: holes are rejected (elements must be strings)
var sp = ["2.0.0", "1.0.0"];
sp[4] = "0.5.0";              // holes at 2,3
TH(function () { sort(sp); }, "TypeError", "sparse array (holes) rejected");

// non-array receivers rejected before any write-back
TH(function () { sort("1.0.0,2.0.0"); }, "TypeError", "string receiver");
TH(function () { sort({ length: 2, 0: "1.0.0", 1: "2.0.0" }); }, "TypeError", "array-like object receiver");
TH(function () { sort(); }, "TypeError", "no arg");
TH(function () { sort(["1.0.0", 2]); }, "TypeError", "non-string element");
TH(function () { sort(["1.0.0", "x.y.z"]); }, "TypeError", "invalid version element");

// proxy over array: trap runs during write-back; engine must not crash and trap sees writes
var log = [];
var target = ["2.0.0", "1.0.0"];
var prox = new Proxy(target, {
  defineProperty(t, k, d) { log.push(String(k)); return Reflect.defineProperty(t, k, d); }
});
var r = sort(prox);
OK(r === prox, "proxy receiver returned as-is");
EQ(target, ["1.0.0", "2.0.0"], "proxy target actually sorted");
OK(log.indexOf("0") >= 0 && log.indexOf("1") >= 0, "defineProperty traps fired for indices");

// huge-length edge: dense 1000-element sort then re-sort is stable/no corruption
var many = [];
for (var i = 0; i < 1000; i++) many.push(String(1000 - i) + ".0.0");
var mret = sort(many);
OK(mret === many, "1000 elems identity");
EQ(many[0], "1.0.0", "1000 elems first");
EQ(many[999], "1000.0.0", "1000 elems last");
EQ(many.length, 1000, "1000 elems length");
DONE("p04_semver_sort_shapes");
