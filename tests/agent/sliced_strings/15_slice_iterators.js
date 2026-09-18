// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["PASS 15_slice_iterators"];
/* string iterator over wide slices; well-formed/normalize on slices. */
var w = parentWide(500) + "\ud83d\ude00" + parentWide(500);
var t = w.slice(10, 1000);
var cp = [];
for (var c of t) cp.push(c.codePointAt(0));
var arr = Array.from(t);
eq(cp.length, arr.length, "iter vs Array.from count");
ok(arr.some(function (c) { return c.codePointAt(0) === 0x1f600; }), "emoji via iter");
/* toWellFormed on slices (copy path with invalid surrogates) */
var badStr = "a" + String.fromCharCode(0xd800) + "b" + String.fromCharCode(0xdc00) + "c".repeat(300);
var bs = badStr.slice(1, 250);
eq(typeof bs.toWellFormed === "function" ? bs.toWellFormed().charCodeAt(0) : bs.charCodeAt(0), 0xfffd, "wellformed head");
/* normalize on slices */
var nfc = "e\u0301".repeat(300);
eq(nfc.slice(10, 200).normalize("NFC"), "\u00e9".repeat(95), "normalize NFC");
/* split into code points keeps pairs */
var em = "a\ud83d\ude00b".repeat(100).slice(1, 100);
eq(Array.from(em).length >= 49, true, "astral Array.from");
/* slice iterator done state and reuse */
var it = t[Symbol.iterator]();
it.next(); it.next();
eq(it.next().done, false, "iterator mid");
__L(0, "PASS 15_slice_iterators");

summary("sliced_strings");
