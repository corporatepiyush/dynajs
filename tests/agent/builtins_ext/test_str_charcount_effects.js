__EXP = {};
__EXP[0] = ["fails=0 hash=373b579c"];
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var src = "0123456789abcdef";
var fails = 0, feed = "";
for (var iter = 0; iter < 2000; iter++) {
  var c = src.charAt(iter % 16);
  var o = {};
  o[c] = iter;
  delete o[c];
  o[c] = iter * 2;
  var arr = [];
  arr[c] = "x";
  if (o[c] !== iter * 2) fails++;
  var m = new Map();
  m.set(c, iter);
  if (m.get(c) !== iter) fails++;
  if (iter % 499 === 0) {
    feed += iter + ":" + JSON.stringify(o) + ":" + m.size + ":" + Object.keys(o).length + "\n";
  }
}
for (var iter = 0; iter < 200; iter++) {
  var a = src.split("");
  if (a.join("") !== src) fails++;
}
__L(0, "fails=" + fails + " hash=" + fnv(feed));

summary("builtins_ext");
