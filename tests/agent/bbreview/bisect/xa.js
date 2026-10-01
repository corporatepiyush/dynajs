const scope = { a: 10, b: 20 };
function w(n) {
  with (scope) { var t = a + b; }
  return n === 0 ? "done" : w(n - 1);
}
console.log("ternary", w(3));
