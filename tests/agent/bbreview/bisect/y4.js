function sloppyArgs(n) { return strictTail(n - 1, arguments.length); }
function strictTail(n, len) {
  "use strict";
  if (n === 0) return "sa:" + len;
  return sloppyArgs(n - 1);
}
console.log("ss7a", sloppyArgs(5));
console.log("ss7b-start");
console.log("ss7b", sloppyArgs(100000));
