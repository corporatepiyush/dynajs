Array.prototype[1] = "P1";
const c = [3, , 1, 2];
c.sort(function(x, y) {
  const r = (x > y ? 1 : x < y ? -1 : 0);
  console.log("cmp(" + String(x) + "," + String(y) + ")=" + r);
  return r;
});
console.log("out", JSON.stringify(c), c.map((v,i)=>i+":"+String(v)+":own="+Object.prototype.hasOwnProperty.call(c,i)).join(" "));
