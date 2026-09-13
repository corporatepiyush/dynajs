// FIX1 differential matrix: next()-originated vs body-originated abrupt
// completions in for-await loops, across iterator kinds. Expectations baked
// from the node oracle (node = spec). Each case prints one deterministic line:
//   CASE <name>: <events joined by "|">
// Events include the caught error and how many times iterator.return() ran.
//
// DIVERGENCE (intentional, spec > node): "afas-ctor-getter" — node does NOT
// call the sync iterator's return() when PromiseResolve inside
// AsyncFromSyncIteratorContinuation throws (V8 ignores closeOnRejection),
// so node prints ret=0 while dynajs prints ret=1 per spec step 6
// (IteratorClose(syncIteratorRecord, valueWrapper); test262
// AsyncFromSyncIteratorPrototype/next/*rejected-promise-close* and
// *poisoned-wrapper* require it and PASS on dynajs). Everything else in
// this matrix is node-identical: the LOOP level never closes the iterator
// on a next()-originated rejection (the fix1 defect — x3 closed it there,
// one microtask late + spurious .return()), and the body-originated throw
// still awaits the close (ret=1).
var pending = 5;
function say(s) { (typeof print === "function" ? print : console.log)(s); }
function done() { if (--pending === 0) say("MATRIX DONE"); }

// ---- shapes --------------------------------------------------------------
// async-from-sync: sync iterator whose yielded value has a throwing
// `constructor` getter (PromiseResolve inside AsyncFromSyncIteratorContinuation)
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

// raw async iterator whose next() returns a rejected promise
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

// case 1: async-from-sync, ctor getter throws inside the wrapper's next
drive("afas-ctor-getter", make_afas_ctor_iter);

// case 2: raw async iterator, next() rejects
drive("async-next-reject", function () { return make_async_iter("reject"); });

// case 3: raw async iterator, next() throws synchronously
drive("async-next-sync-throw", function () { return make_async_iter("sync-throw"); });

// case 4 (control): body throw -> return() MUST be called (close awaited)
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

// case 5: next resolves to a non-object -> TypeError in machinery
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
