const scope = { a: 10, b: 20 };
function w(n) {
  with (scope) { var t = n * 2; }
  return w(n - 1);
}
console.log("no-base", w(3));
