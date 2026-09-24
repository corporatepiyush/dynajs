const o1 = { v: 1 };
function w2(n, obj) {
  with (obj) {
    if (n === 0) return "w2-done";
    return w2(n - 1, obj);
  }
}
console.log("w2s", w2(5, o1));
console.log("w2d", w2(200000, o1));
