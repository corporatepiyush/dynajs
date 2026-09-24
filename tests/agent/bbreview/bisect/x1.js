function w1(n, obj) {
  with (obj) { if (n === 0) return "w-done"; }
  return w1(n - 1, obj);
}
const o1 = { v: 1 };
console.log("w1s", w1(5, o1));
console.log("w1d-start");
console.log("w1d", w1(200000, o1));
