var expected = ["constructor","constructor","tick 1","tick 2","loop","constructor","tick 3","tick 4","post"];
var actual = [];
async function f() {
  var p = Promise.resolve(0);
  actual.push("pre");
  for await (var x of [p]) { actual.push("loop"); }
  actual.push("post");
}
Promise.resolve(0)
  .then(() => actual.push("tick 1"))
  .then(() => actual.push("tick 2"))
  .then(() => actual.push("tick 3"))
  .then(() => actual.push("tick 4"))
  .then(() => { console.log("ACTUAL", JSON.stringify(actual)); }).then(()=>console.log("done"),(e)=>console.log("ERR",e.message));
Object.defineProperty(Promise.prototype, "constructor", {
  get() { actual.push("constructor"); return Promise; },
  configurable: true,
});
f();
setTimeout(()=>console.log("TICK"), 300);
