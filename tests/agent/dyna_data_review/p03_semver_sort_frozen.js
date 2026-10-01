import { sort } from "dyna:semver";
import { eq as EQ, ok as OK, throws as TH, done as DONE } from "./harness.js";

var f = Object.freeze(["2.0.0", "1.0.0"]);
TH(function () { sort(f); }, "TypeError", "frozen array throws");
EQ(f, ["2.0.0", "1.0.0"], "frozen array untouched after throw");

var s = Object.seal(["2.0.0", "1.0.0"]);
EQ(sort(s), ["1.0.0", "2.0.0"], "sealed array sorts (elements still writable)");

var p = Object.preventExtensions(["3.0.0", "1.0.0", "2.0.0"]);
EQ(sort(p), ["1.0.0", "2.0.0", "3.0.0"], "non-extensible sorts within length");

var g = ["2.0.0", "1.0.0"];
Object.defineProperty(g, "0", { get: function () { return "2.0.0"; }, configurable: false });
var threw = false;
try { sort(g); } catch (e) { threw = true; }
OK(threw, "getter-only element throws on write-back");
var d0 = Object.getOwnPropertyDescriptor(g, "0");
OK(d0 && d0.get, "accessor not clobbered");

var w = ["2.0.0", "1.0.0"];
Object.defineProperty(w, "0", { value: "2.0.0", writable: false, configurable: false });
try { sort(w); OK(false, "non-writable element throws"); }
catch (e) { OK(true, "non-writable element throws"); }
EQ(w[1], "1.0.0", "non-writable element content intact");
DONE("p03_semver_sort_frozen");
