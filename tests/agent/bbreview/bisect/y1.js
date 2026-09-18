function strictRec(n) {
  "use strict";
  if (n === 0) return "s-done";
  return sloppyRec(n - 1);
}
function sloppyRec(n) {
  if (n === 0) return "sl-done";
  return strictRec(n - 1);
}
console.log("ss1", strictRec(5), sloppyRec(5));
console.log("ss2-start");
console.log("ss2", strictRec(300000));
