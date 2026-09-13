/* slices as eval / Function source (ToCString boundary). */
var src = ("(1+2)  ".repeat(100)).slice(0, 5);
eq2(eval(src), 3);
eq2(new Function(("return 42;//" + "x".repeat(500)).slice(0, 12))(), 42);
var big = ("3+4; ".repeat(1000)).slice(2, 4999);
eq2(eval(big), 7);
function eq2(a, b) { if (a !== b) throw new Error("adv mismatch: " + a + " vs " + b); }
console.log("PASS a02");
