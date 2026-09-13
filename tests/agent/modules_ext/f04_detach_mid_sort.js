// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[2] = [["detached: len=0 calls=6 sawUndef=false no-throw", "detached: len=0 calls=5 sawUndef=false no-throw"]];
__EXP[4] = ["DONE"];
__REQ = {"dynajs": {"2": 1, "4": 1}, "node": {"2": 1, "4": 1}};
// modules_ext f04: comparator DETACHES the buffer mid-sort (ArrayBuffer.transfer(0)).
// Spec: reads go through the live buffer; after detach, reads yield undefined ->
// comparator receives undefined, sort continues without error, final content per
// node parity. Guarded on transfer support.
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
if (typeof ArrayBuffer.prototype.transfer !== 'function') {
  __L(1, 'SKIP no-transfer');
} else {
  const ab = new ArrayBuffer(32);
  const f = new Float64Array(ab);
  f[0] = 4; f[1] = 1; f[2] = 3; f[3] = 2;
  let sawUndefined = false;
  let calls = 0;
  try {
    f.sort(function (a, b) {
      calls++;
      if (a === undefined || b === undefined) sawUndefined = true;
      if (calls === 3) { ab.transfer(0); }
      return (a === undefined ? 0 : a) - (b === undefined ? 0 : b);
    });
    __L(2, 'detached: len=' + f.length + ' calls=' + calls + ' sawUndef=' + sawUndefined + ' no-throw');
  } catch (e) {
    __L(3, 'detached threw: ' + (e && e.name));
  }
}
__L(4, 'DONE');

summary("modules_ext");
