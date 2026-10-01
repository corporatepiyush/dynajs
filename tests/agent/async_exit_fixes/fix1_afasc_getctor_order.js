var out = [];
function say(s) { (typeof print === "function" ? print : console.log)(s); }
var actual = [];

async function f() {
  var p = Promise.resolve(0);
  Object.defineProperty(p, "constructor", {
    get() {
      throw new Error();
    }
  });
  actual.push("start");
  for await (var x of [p]);
  actual.push("never reached");
}

Promise.resolve(0)
  .then(() => actual.push("tick 1"))
  .then(() => actual.push("tick 2"))
  .then(() => {
    say("OUTER: " + actual.join("|"));
  })
  .then(() => {
    say("LATE: " + actual.join("|"));
  })
  .catch(e => { say("OUTER-ERR: " + (e && e.message)); });

f().catch(() => actual.push("catch"));
