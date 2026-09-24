const scope = { a: 10, b: 20 };
function w4(n) {
  with (scope) { var t = a + b; }
  return "no-tail-" + n;
}
console.log("nt", w4(3));
