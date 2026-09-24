import { sort } from "dyna:semver";
import { eq as EQ, ok as OK, done as DONE } from "./harness.js";

// contract: returns the SAME array, sorted in place (API.md)
var a = ["1.10.0", "1.2.0", "1.9.0"];
var ret = sort(a);
OK(ret === a, "returns the same array object");
EQ(a, ["1.2.0", "1.9.0", "1.10.0"], "caller's array is sorted in place");
EQ(a.length, 3, "length preserved");

EQ(sort([]), [], "empty array: same empty back");
var one = ["2.0.0"]; EQ(sort(one), ["2.0.0"], "single");
OK(sort(one) === one, "single: identity");

var dup = ["1.0.0", "1.0.0", "0.9.0"];
EQ(sort(dup), ["0.9.0", "1.0.0", "1.0.0"], "duplicates sorted");
var rev = ["3.0.0", "2.0.0", "1.0.0"];
EQ(sort(rev), ["1.0.0", "2.0.0", "3.0.0"], "reverse order");
var pre = ["1.0.0-alpha", "1.0.0", "1.0.0-beta", "1.0.0-rc.1", "0.9.0"];
EQ(sort(pre), ["0.9.0", "1.0.0-alpha", "1.0.0-beta", "1.0.0-rc.1", "1.0.0"], "prerelease precedence (semver.org §11)");
var num = ["1.0.0-2", "1.0.0-10"];
EQ(sort(num), ["1.0.0-2", "1.0.0-10"], "numeric prerelease ids compare numerically");
var big = ["10.0.0", "9.0.0", "2.0.0", "100.0.0"];
EQ(sort(big), ["2.0.0", "9.0.0", "10.0.0", "100.0.0"], "numeric field order");
var bld = ["1.0.0+b", "1.0.0+a"];
EQ(sort(bld).length, 2, "build metadata ignored in precedence");

// original element STRINGS are preserved verbatim (no normalization on write-back)
var v = ["v1.0.0", "1.0.0"];
EQ(sort(v), ["v1.0.0", "1.0.0"], "equal elements keep input order");
DONE("p02_semver_sort_inplace");
