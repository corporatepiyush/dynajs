// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["fails=0 hash=84bd4018"];
// test_str_concat_cached_vs_literal.js — concat chains built from cached
// single-char strings vs literals: ===, charCodeAt walk, and hash must be
// byte-identical; also += accumulation and Array.prototype.join paths.
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var word = "The quick brown fox jumps over the lazy dog 0123456789";
var fails = 0;
function chk(c, l) { if (!c) { fails++; __L(0, "FAIL " + l); } }
// build via charAt chain
var built = "";
for (var i = 0; i < word.length; i++) built += word.charAt(i);
chk(built === word, "charAt-chain-eq");
// build via index reads
var built2 = "";
for (var i = 0; i < word.length; i++) built2 += word[i];
chk(built2 === word, "index-chain-eq");
// build via split("") + join
chk(word.split("").join("") === word, "split-join-eq");
// build via split("") + concat per element
var built3 = "";
var parts = word.split("");
for (var i = 0; i < parts.length; i++) built3 = built3.concat(parts[i]);
chk(built3 === word, "concat-chain-eq");
// wide chars
var wide = "h\u00e9llo w\u00f6rld \u4e16\u754c";
var wb = "";
for (var i = 0; i < wide.length; i++) wb += wide.charAt(i);
chk(wb === wide, "wide-chain-eq");
// charCodeAt hashes agree
chk(fnv(built) === fnv(word), "hash-eq");
// chained concats of cached chars vs literal, interleaved lengths
var a = "" + "a".charAt(0) + "b".charAt(0) + "c".charAt(0);
chk(a === "abc", "short-mix");
chk(("x" + parts[0]).length === 2, "len-mix");
// repeated single-char strings via repeat + split
var rep = "ab".repeat(1000).split("");
chk(rep.length === 2000 && rep[0] === "a" && rep[1999] === "b", "repeat-split");
chk(rep.join("").length === 2000, "repeat-join");
__L(1, "fails=" + fails + " hash=" + fnv(built + "|" + wb));

summary("builtins_ext");
