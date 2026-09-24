/* NUL bytes and borrow-boundary safety on slices. */
var w = "";
for (var i = 0; i < 300; i++) w += "ab";
var t = ("\0" + w + "\0ok").slice(1, w.length + 1);
eq2(t.length, w.length);
eq2(t, w);
var t2 = ("x".repeat(200) + "a\0b").slice(200);
eq2(t2.length, 3);
eq2(t2.charCodeAt(1), 0);
eq2(Number("12\034".slice(100).length), 0);
var enc = encodeURIComponent(("\0" + "x".repeat(300)).slice(1));
eq2(enc, "x".repeat(300));
function eq2(a, b) { if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error("adv mismatch: " + JSON.stringify(a) + " vs " + JSON.stringify(b)); }
console.log("PASS a01");
