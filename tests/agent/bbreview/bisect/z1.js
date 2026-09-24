function sloppyArgs(n) { return strictTail(n - 1, arguments.length); }
function strictTail(n, len) {
  "use strict";
  if (n === 0) return "sa:" + len;
  return sloppyArgs(n - 1);
}
console.log("n10", sloppyArgs(10));
console.log("n100-start");
console.log("n100", sloppyArgs(100));
