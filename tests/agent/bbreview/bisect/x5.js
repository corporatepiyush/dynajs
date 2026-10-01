const scope = { a: 10, b: 20 };
function w4(n) {
  with (scope) { var t = a + b; }
  return w4(n - 1);
}
console.log("w4s", w4(3));
