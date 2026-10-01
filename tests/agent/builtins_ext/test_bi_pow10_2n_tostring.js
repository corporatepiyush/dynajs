__EXP = {};
__EXP[1] = ["10^1000 d10 len=1001 hash=ab1bcd1c", "10^4999 d10 len=5000 hash=331527c4", "2^10000 d10 len=3011 hash=b1aac44", "2^10000-1 d10 len=3011 hash=e1ab0fd", "3^5000 d10 len=2386 hash=eb470e7a", "999..x9 d10 len=1001 hash=816edc97"];
__EXP[2] = ["2^10000 digitCount=3011 head=1995063116 tail=2596709376"];
__EXP[3] = ["fails=0 hash=ef253f32"];
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var fails = 0, feed = "";
var vals = [
  [10n ** 1000n, "10^1000"],
  [10n ** 4999n, "10^4999"],
  [2n ** 10000n, "2^10000"],
  [2n ** 10000n - 1n, "2^10000-1"],
  [3n ** 5000n, "3^5000"],
  [(10n ** 1000n - 1n) * 9n, "999..x9"]
];
for (var i = 0; i < vals.length; i++) {
  var v = vals[i][0], name = vals[i][1];
  var d10 = v.toString(10);
  if (BigInt(d10) !== v) { fails++; __L(0, "RT-FAIL " + name + " r10"); }
  __L(1, name + " d10 len=" + d10.length + " hash=" + fnv(d10));
  feed += name + ":" + d10.length + ":" + fnv(v.toString(16)) + ":" + fnv(v.toString(7)) + "\n";
}
var d = String(2n ** 10000n);
__L(2, "2^10000 digitCount=" + d.length + " head=" + d.slice(0, 10) + " tail=" + d.slice(-10));
feed += "pow2\n";
__L(3, "fails=" + fails + " hash=" + fnv(feed));

summary("builtins_ext");
