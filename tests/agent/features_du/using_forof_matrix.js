function __EV(name, fn) { test(name, fn); }
function __say(s) { (typeof print === "function" ? print : console.log)(s); }
function __res(id, log) {
  var o = {};
  o.id = id;
  o[Symbol.dispose] = function () { log.push("dispose:" + this.id + ":" + (this === o)); };
  return o;
}

var events;

__EV("per-iteration-registration-and-disposal", function () {
  events = [];
  for (using x of [__res("i1", events), __res("i2", events)]) {
    events.push("body:" + x.id);
  }
  assert_eq(events.join("|"), "body:i1|dispose:i1:true|body:i2|dispose:i2:true");
});

__EV("continue-disposes-current", function () {
  events = [];
  for (using x of [__res("c1", events), __res("c2", events)]) {
    events.push("it");
    continue;
  }
  assert_eq(events.join("|"), "it|dispose:c1:true|it|dispose:c2:true");
});

__EV("break-known-gap-no-disposal", function () {
  events = [];
  for (using x of [__res("b1", events), __res("b2", events)]) {
    events.push("it:" + x.id);
    break;
  }
  assert_eq(events.join("|"), "it:b1");
});

__EV("empty-iterable-no-disposal", function () {
  events = [];
  for (using x of []) events.push("never");
  assert_eq(events.join("|"), "");
});

__EV("binding-holds-the-resource", function () {
  var seen = null;
  for (using x of [{ id: "v", [Symbol.dispose]: function () {} }]) seen = x;
  assert(seen && seen.id === "v", "binding holds the resource");
});

__EV("no-dispose-method-rejected-per-iteration", function () {
  var err = __mustThrow(function () {
    for (using x of [42]) events = events;
  });
  assert(err, "expected throw");
  assert_eq(err.constructor.name, "TypeError");
});

summary("using_forof_matrix");
