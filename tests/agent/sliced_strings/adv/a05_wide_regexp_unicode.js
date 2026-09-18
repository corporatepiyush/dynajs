/* wide slices + unicode regexps + lastIndex edge behavior. */
var w = "\u4e16\u754c".repeat(500) + "X" + "\u4e16".repeat(500);
var t = w.slice(7, 1001);
var expected = t.split("\u4e16").length - 1;
var re = /\u4e16/g;
var n = 0, r;
while ((r = re.exec(t)) !== null) { n++; re.lastIndex = r.index + 1; if (n > 2000) break; }
if (n !== expected) throw new Error("adv: wide global count " + n + " want " + expected);
var re2 = /X/y;
re2.lastIndex = 993;
if (!re2.exec(t)) throw new Error("adv: sticky X");
var u = new RegExp("\u4e16", "gu");
var m3 = t.match(u);
if (m3.length !== expected) throw new Error("adv: unicode set match " + m3.length);
/* surrogate-boundary slicing on wide slices */
var em = "\ud83d\ude00".repeat(300);
var e1 = em.slice(1, 599);   /* starts mid-pair */
if (e1.charCodeAt(0) !== 0xde00) throw new Error("adv: mid-pair start");
if (e1.codePointAt(0) !== 0xde00) throw new Error("adv: lone low codepoint");
if (Array.from(e1).length !== 300) throw new Error("adv: from len");
console.log("PASS a05");
