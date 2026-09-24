/* object key insertion order with slice-built keys (interning determinism). */
var o = {};
var names = [];
for (var i = 0; i < 40; i++) {
  var k = (("key" + i + "_").padEnd ? ("key" + i + "_").padEnd(80, "z") : ("key" + i + "_" + "z".repeat(70))).slice(0, 80);
  o[k] = i; names.push(k);
}
var got = Object.keys(o);
if (got.length !== 40) throw new Error("adv: key count " + got.length);
for (var j = 0; j < 40; j++) if (got[j] !== names[j]) throw new Error("adv: key order @" + j);
/* delete + re-add keeps canonical behavior */
delete o[names[3]];
o[names[3]] = 99;
if (o[names[3]] !== 99) throw new Error("adv: re-add");
if (Object.keys(o)[39] !== names[3]) throw new Error("adv: re-add order");
console.log("PASS a03");
