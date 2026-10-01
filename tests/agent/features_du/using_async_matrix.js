function __EV(name, fn) { test(name, fn); }
function __say(s) { (typeof print === "function" ? print : console.log)(s); }
function __res(id, log) {
  var o = {};
  o.id = id;
  o[Symbol.dispose] = function () { log.push("dispose:" + this.id + ":" + (this === o)); };
  return o;
}

var events;
var __PENDING = 3;
function __done() { if (--__PENDING === 0) summary("using_async_matrix"); }
function __guard(name, fn) {
  return function (v) {
    var save = __T.cur;
    __T.cur = name;
    try { fn(); __T.pass++; }
    catch (e) { __T.fail++; __T.fails.push(name + ": " + (e && e.message ? e.message : String(e))); }
    __T.cur = save;
    __done();
  };
}

var ev1 = [], ev2 = [], ev3 = [];

__EV("await-using-async-dispose", function () {
  var ev = ev1;
  async function f() {
    await using a = { id: "a", [Symbol.asyncDispose]: function () {
      ev.push("ad:" + this.id); return 42;  } };
    ev.push("body");
  }
  f().then(__guard("await-using-async-dispose", function () {
    assert_eq(ev.join("|"), "body|ad:a");
  }), __guard("await-using-async-dispose", function () {
    assert(false, "promise rejected");
  }));
});

__EV("await-using-sync-fallback", function () {
  var ev = ev2;
  async function f() {
    await using a = { id: "s", [Symbol.dispose]: function () { ev.push("sd:" + this.id); } };
    ev.push("body2");
  }
  f().then(__guard("await-using-sync-fallback", function () {
    assert_eq(ev.join("|"), "body2|sd:s");
  }), __guard("await-using-sync-fallback", function () {
    assert(false, "promise rejected");
  }));
});

__EV("await-using-null-still-awaits", function () {
  var ev = ev3;
  async function f() {
    await using a = null;
    ev.push("body");
  }
  f().then(__guard("await-using-null-still-awaits", function () {
    assert_eq(ev.join("|"), "body");
  }), __guard("await-using-null-still-awaits", function () {
    assert(false, "promise rejected");
  }));
});
