const scope = { a: 10, b: 20 };
function w(n) {
  with (scope) { var t = a + b; }
  if (n === 0) return "done";
  return w(n - 1);
}
console.log("var-in-with", w(3));
