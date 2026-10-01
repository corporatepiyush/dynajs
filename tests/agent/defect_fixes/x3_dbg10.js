var actual = [];
function assertCompare(a, e) {
  if (JSON.stringify(a) !== JSON.stringify(e)) throw new Error("MISMATCH got " + JSON.stringify(a));
}
async function f() {
  var p = Promise.resolve(0);
  Object.defineProperty(p, "constructor", { get() { throw new Error(); } });
  actual.push("start");
  for await (var x of [p]);
  actual.push("never reached");
}
Promise.resolve(0)
  .then(() => actual.push("tick 1"))
  .then(() => actual.push("tick 2"))
  .then(() => { assertCompare(actual, ["start","tick 1","tick 2","catch"]); console.log("PASS"); })
  .then(()=>console.log("done-chain"),(e)=>console.log("CHAIN-ERR", e.message));
f().catch(() => actual.push("catch"));
setTimeout(()=>console.log("TICK", JSON.stringify(actual)), 300);
