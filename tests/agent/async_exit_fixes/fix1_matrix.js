var pending = 5;
function say(s) { (typeof print === "function" ? print : console.log)(s); }
function done() { if (--pending === 0) say("MATRIX DONE"); }

function make_afas_ctor_iter() {
  var ret_calls = 0;
  var it = {
    [Symbol.iterator]() { return this; },
    next() {
      var p = Promise.resolve(0);
      Object.defineProperty(p, "constructor", {
        get() { throw new Error("ctor"); }
      });
      return { value: p, done: false };
    },
    return(v) { ret_calls++; return { value: v, done: true }; }
  };
  it.ret_calls = function () { return ret_calls; };
  return it;
}

function make_async_iter(mode) {
  var ret_calls = 0, next_calls = 0;
  var it = {
    [Symbol.asyncIterator]() { return this; },
    next() {
      next_calls++;
      if (mode === "sync-throw") throw new Error("next-sync");
      return Promise.reject(new Error("next-reject"));
    },
    return(v) { ret_calls++; return Promise.resolve({ value: v, done: true }); }
  };
  it.ret_calls = function () { return ret_calls; };
  return it;
}

async function drive(name, make, body_throws) {
  var actual = [];
  var it = make();
  try {
    for await (var x of it) {
      actual.push("body:" + String(x));
      if (body_throws) throw new Error("body");
    }
  } catch (e) {
    actual.push("catch:" + e.message);
  }
  actual.push("ret=" + it.ret_calls());
  say("CASE " + name + ": " + actual.join("|"));
  done();
}

drive("afas-ctor-getter", make_afas_ctor_iter);

drive("async-next-reject", function () { return make_async_iter("reject"); });

drive("async-next-sync-throw", function () { return make_async_iter("sync-throw"); });

drive("body-throw-closes", function () {
  var ret_calls = 0;
  var it = {
    [Symbol.asyncIterator]() { return this; },
    next() { return Promise.resolve({ value: 1, done: false }); },
    return(v) { ret_calls++; return Promise.resolve({ value: v, done: true }); }
  };
  it.ret_calls = function () { return ret_calls; };
  return it;
}, true);

drive("next-non-object", function () {
  var ret_calls = 0;
  var it = {
    [Symbol.asyncIterator]() { return this; },
    next() { return Promise.resolve(42); },
    return(v) { ret_calls++; return Promise.resolve({ value: v, done: true }); }
  };
  it.ret_calls = function () { return ret_calls; };
  return it;
});
