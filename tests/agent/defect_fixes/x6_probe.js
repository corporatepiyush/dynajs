Array.prototype[1] = "P1";
const c = [3, , 1, 2];
let log = [];
c.sort(function(x, y) { log.push([String(x), String(y)]); return (x > y ? 1 : x < y ? -1 : 0); });
for (let i = 0; i < c.length; i++) {
  log.push(i + ":" + String(c[i]) + ":own=" + Object.prototype.hasOwnProperty.call(c, i));
}
console.log(log.map(l => Array.isArray(l) ? l.join("<") : l).join(" "));
