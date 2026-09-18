// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["fails=0 hash=c463f53f"];
// test_str_latin1_keys_delete.js — cached single-char strings used as property
// keys, deleted, re-added (the latin1-cache atom-donation scenario): key order,
// value integrity, and Map-key equivalence must all survive.
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var src = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+-*/%&@#!?";
var fails = 0;
// pass 1: add all
var o = {};
for (var i = 0; i < src.length; i++) { var k = src.charAt(i); o[k] = i; }
// pass 2: delete every other
for (var i = 0; i < src.length; i += 2) delete o[src.charAt(i)];
// pass 3: re-add deleted ones with new values
for (var i = 0; i < src.length; i += 2) o[src.charAt(i)] = i * 7 + 1;
// pass 4: delete again in reverse, re-add
for (var i = src.length - 1; i >= 0; i -= 2) delete o[src.charAt(i)];
for (var i = src.length - 1; i >= 0; i -= 2) o[src.charAt(i)] = i * 3;
// verify values
for (var i = 0; i < src.length; i++) {
  var expect = (i % 2 === 0) ? i * 7 + 1 : i * 3;
  if (o[src.charAt(i)] !== expect) { fails++; if (fails < 5) __L(0, "VAL-FAIL i=" + i + " got=" + o[src.charAt(i)] + " want=" + expect); }
}
var ks = Object.keys(o);
if (ks.length !== src.length) fails++;
// key order after delete/re-add: re-added keys go to the back (insertion order)
var feed = ks.join("") + "\n";
// delete ALL then re-add in reverse order — full key-order determinism
var o2 = {};
for (var i = 0; i < src.length; i++) o2[src.charAt(i)] = i;
var all = Object.keys(o2);
for (var i = 0; i < all.length; i++) delete o2[all[i]];
for (var i = src.length - 1; i >= 0; i--) o2[src.charAt(i)] = src.length - i;
feed += Object.keys(o2).join("") + "\n";
// split("")-derived chars as keys (bulk-cached strings as atoms)
var o3 = {};
var parts = src.split("");
for (var i = 0; i < parts.length; i++) o3[parts[i]] = i;
for (var i = 0; i < src.length; i++) if (o3[src.charAt(i)] !== i) fails++;
feed += Object.keys(o3).join("") + "\n";
__L(1, "fails=" + fails + " hash=" + fnv(feed));

summary("builtins_ext");
