/* Regression suite for the "await using .then double-fire" lead (flagged by
 * the warnings lane; repros preserved as .swarm/ws-warn/sm10.js and
 * smoke_using2.js) and for the async-disposal sync-fallback hang found while
 * running it to ground:
 *
 * 1. DOUBLE-FIRE LEAD -- NOT A BUG. Every reaction attached to an
 *    `await using` chain -- including chains created inside function calls
 *    -- settles EXACTLY ONCE. The sm10 harness prints "ok <name>" from T()
 *    synchronously AND again from .then, so each line appears twice by
 *    construction; the second batch's reverse order is spec microtask
 *    ordering, byte-identical to bun 1.4.0 / deno 2.9.6. This suite pins
 *    single-fire with counters and pins the reference fulfillment order.
 *
 * 2. SYNC-FALLBACK HANG -- REAL BUG (fixed). A sync @@dispose method found
 *    by an async hint (await using / AsyncDisposableStack.use) whose return
 *    value is a promise used to be AWAITED by the dispose driver, so a
 *    never-settling promise deadlocked the disposal. Spec GetDisposeMethod
 *    step 1.b.ii requires the fallback to be wrapped so "a Promise returned
 *    from a synchronous @@dispose method will not be awaited". The last two
 *    tests pin the fixed behavior; on the pre-fix engine they fail via
 *    watchdog instead of hanging the suite.
 *
 * The `using`/`await using` SYNTAX is behind CONFIG_USING (the
 * DisposableStack builtins are unconditional), so every `await using` here
 * is compiled at runtime through `new Function`, gated by a probe: on a
 * build without the syntax the suite skips cleanly (a default `make test`
 * must stay green). Style follows tests/test_disposable.js (no harness). */
"use strict";

function assert(actual, expected, message) {
    if (arguments.length === 1)
        expected = true;
    if (actual === expected)
        return;
    throw Error("assertion failed: got |" + actual + "|, expected |" +
                expected + "|" + (message ? " (" + message + ")" : ""));
}

/*---- feature detect: `await using` syntax (CONFIG_USING) ----*/

var USING_SYNTAX = (function () {
    try {
        new Function("(async function(){ await using d = null; })()");
        return true;
    } catch (e) {
        return false;
    }
})();

/* Compile an async chain body containing `await using`. Called only after
   USING_SYNTAX passed. args is a comma-separated parameter list made visible
   to the body; the returned factory forwards its arguments positionally. */
function using_chain(args, body) {
    var f = new Function(args,
        "return (async function () {" + body + "})();");
    return function () {
        return f.apply(null, arguments);
    };
}

/*---- sm10.js shape: two chains created inside function calls; each .then
   fires exactly once ----*/

function test_sm10_single_fire() {
    var fired = { basic: 0, basic_rej: 0, sync: 0, sync_rej: 0 };
    var log = [], log2 = [];
    var p1, p2;

    function T(name, fn) {
        try {
            fn();
        } catch (e) {
            throw Error("T(" + name + ") threw synchronously: " + e);
        }
    }

    T("async-basic", function () {
        class A { constructor(n){ this.n = n; } [Symbol.asyncDispose](){ log.push(this.n); } }
        p1 = using_chain("A, log",
            "    { await using a = new A('x'), b = new A('y'); }" +
            "    if (log.join() !== 'y,x') throw Error(log.join());")(A, log);
        p1.then(function () { fired.basic++; },
                function (e) { fired.basic_rej++; });
    });
    T("async-sync-disposer", function () {
        class A { [Symbol.dispose](){ log2.push("sync"); } }
        p2 = using_chain("A, log",
            "    { await using a = new A(); }" +
            "    if (log.join() !== 'sync') throw Error(log.join());")(A, log2);
        p2.then(function () { fired.sync++; },
                function (e) { fired.sync_rej++; });
    });

    return (async function () {
        /* deterministic drain: the counter .then handlers were registered
           on p1/p2 BEFORE this await registers its reaction, so each has
           already run by the time the await resumes */
        await p1;
        await p2;
        assert(fired.basic, 1, "async-basic .then fired " +
               fired.basic + "x (must be exactly 1)");
        assert(fired.basic_rej, 0, "async-basic rejected");
        assert(fired.sync, 1, "async-sync-disposer .then fired " +
               fired.sync + "x (must be exactly 1)");
        assert(fired.sync_rej, 0, "async-sync-disposer rejected");
    })();
}

/*---- smoke_using2.js #8 shape: body throw after `await using` -- the outer
   promise rejects ONCE and the sync disposer still ran ----*/

function test_body_throw_single_fire() {
    var fired = { rej: 0, ok: 0 }, log = [];
    class A { [Symbol.dispose](){ log.push("d"); } }
    var p = using_chain("A, log",
        "    await using a = new A();" +
        "    throw new Error('aboom');")(A, log);
    p.then(function () { fired.ok++; },
           function (e) {
               fired.rej++;
               if (e.message !== "aboom")
                   throw Error("wrong reason " + e);
           });
    return (async function () {
        await p.catch(function (e) {});   /* p settles exactly once */
        assert(fired.rej, 1, "rejection handler fired " +
               fired.rej + "x (must be exactly 1)");
        assert(fired.ok, 0, "fulfillment handler fired on rejection");
        assert(log.join(), "d", "disposer ran");
    })();
}

/*---- fulfillment ORDER of two chains (the "second batch in reverse order"
   the warnings lane saw): chain A (two async disposers) needs two dispose
   microtask turns, chain B (sync-disposer fallback) needs one, so B's tail
   runs first -- matches bun 1.4.0 / deno 2.9.6 byte-for-byte ----*/

function test_fulfillment_order() {
    var order = [];
    class A { constructor(n){ this.n = n; } [Symbol.asyncDispose](){} }
    class B { [Symbol.dispose](){} }
    var pa = using_chain("A, order",
        "    { await using a = new A(1), b = new A(2); }" +
        "    order.push('A');")(A, order);
    var pb = using_chain("B, order",
        "    { await using c = new B(); }" +
        "    order.push('B');")(B, order);
    return (async function () {
        await pa; await pb;
        assert(order.join(), "B,A",
               "async fulfillment order (spec microtask order, ref-parity)");
    })();
}

/*---- for-of per-iteration dispose: one await-using scope per iteration,
   LIFO inside one iteration; each iteration's chain settles once ----*/

function test_for_of_iteration() {
    var log = [];
    class R { constructor(n){ this.n = n; } [Symbol.asyncDispose](){ log.push(this.n); } }
    var p = using_chain("R, log",
        "    for (var x of [1, 2])" +
        "    { await using r = new R(x); }" +
        "    for (var y of [1])" +
        "    { await using r1 = new R(1), r2 = new R(2); }" +
        "    if (log.join() !== '1,2,2,1') throw Error(log.join());")(R, log);
    return p.then(function () {
        assert(log.join(), "1,2,2,1",
               "per-iteration + intra-iteration LIFO order");
    });
}

/*---- a REAL async disposer (pending promise) is awaited exactly once,
   and the block does not continue before it settles ----*/

function test_awaited_disposer() {
    var log = [];
    var resolve_now;
    var gate = new Promise(function (res) { resolve_now = res; });
    class R { [Symbol.asyncDispose](){ return gate.then(function () { log.push("slow"); }); } }
    var p = using_chain("R, log",
        "    { await using a = new R(); }" +
        "    if (log.join() !== 'slow') throw Error('disposal did not await the async disposer');")(R, log);
    resolve_now();
    return p;
}

/*---- non-disposable value under `await using`: TypeError rejection, and
   each attached .then handler fires exactly once ----*/

function test_non_disposable_once_per_then() {
    var fired = { e1: 0, e2: 0, ok1: 0, ok2: 0 };
    var p = using_chain("",
        "    { await using a = {}; }")();
    p.then(function () { fired.ok1++; },
           function (e) {
               fired.e1++;
               if (!(e instanceof TypeError))
                   throw Error("not a TypeError: " + e);
           });
    p.then(function () { fired.ok2++; },
           function (e) {
               fired.e2++;
               if (!(e instanceof TypeError))
                   throw Error("not a TypeError: " + e);
           });
    return (async function () {
        await p.catch(function (e) {});
        assert(fired.e1, 1, "handler 1 fired " + fired.e1 + "x");
        assert(fired.e2, 1, "handler 2 fired " + fired.e2 + "x");
        assert(fired.ok1 + fired.ok2, 0, "fulfilled on bad resource");
    })();
}

/*---- chains created inside (possibly recursive) function calls: each
   settles exactly once (the exact configuration the lead described) ----*/

function test_nested_function_calls() {
    var fired = 0;
    /* recursive chain factory: every level creates an `await using` scope
       inside a function call, exactly the configuration the lead described */
    var mk = new Function("mkself", "depth",
        "return (async function () {" +
        "    { await using a = { [Symbol.dispose]: function () {} }; }" +
        "    if (depth > 0) await mkself(mkself, depth - 1);" +
        "})();");
    var p = mk(mk, 3);
    p.then(function () { fired++; }, function (e) { fired = -100; });
    return (async function () {
        await p;
        assert(fired, 1, "outer .then fired " + fired + "x (must be exactly 1)");
    })();
}

/*---- a SYNC @@dispose method returning a promise must NOT hang the async
   disposal: per GetDisposeMethod step 1.b.ii the fallback is wrapped so its
   return value is discarded ("a Promise returned from a synchronous
   @@dispose method will not be awaited"). Before the fix this test
   deadlocked; reference engines (V8/JSC) complete. Both `await using` and
   AsyncDisposableStack.use shapes. ----*/

function test_sync_dispose_returning_promise_not_awaited() {
    var done = {};
    var never = Promise.withResolvers().promise;

    var p1 = using_chain("never, done",
        "    { await using x = { [Symbol.dispose]: function () { return never; } }; }" +
        "    done.s1 = true;")(never, done);
    var p2 = (async function () {
        var st = new AsyncDisposableStack();
        st.use({ [Symbol.dispose]: function () { return never; } });
        await st.disposeAsync();
        done.s2 = true;
    })();
    /* watchdog: on the pre-fix engine the disposal deadlocks on the
       never-resolving promise and p1/p2 never settle; reject loudly so the
       suite fails instead of exiting silently with a drained job queue.
       Generous timeout: under a parallel `make test` even microtask work
       can see hundreds of ms of scheduler delay. */
    var timer;
    var watchdog = new Promise(function (resolve, reject) {
        timer = setTimeout(function () {
            reject(new Error("async disposal hung: sync @@dispose " +
                             "return value was awaited"));
        }, 10000);
    });
    return (async function () {
        await Promise.race([Promise.all([p1, p2]), watchdog]);
        clearTimeout(timer);
        assert(done.s1, true, "await using: sync dispose return hung disposal");
        assert(done.s2, true, "AsyncDisposableStack.use: sync dispose return hung disposal");
    })();
}

/*---- a THROW from a sync @@dispose fallback under `await using` becomes a
   rejection of the disposal (same SuppressedError folding as before), and
   fires exactly once ----*/

function test_sync_dispose_throw_rejection() {
    var fired = { rej: 0, ok: 0 };
    var p = using_chain("",
        "    await using x = { [Symbol.dispose]: function () { throw new TypeError('sync-throw'); } };")();
    p.then(function () { fired.ok++; },
           function (e) {
               fired.rej++;
               if (!(e instanceof TypeError))
                   throw Error("expected TypeError, got " + e);
           });
    return (async function () {
        await p.catch(function (e) {});
        assert(fired.rej, 1, "rejection fired " + fired.rej + "x");
        assert(fired.ok, 0, "fulfilled despite sync throw");
    })();
}

/*---- run ----*/

function run_all_tests() {
    if (!USING_SYNTAX) {
        print("test_using_double_fire: skipped (await using syntax not " +
              "compiled in; build with CONFIG_USING=y)");
        return Promise.resolve();
    }
    var tests = [
        test_sm10_single_fire,
        test_body_throw_single_fire,
        test_fulfillment_order,
        test_for_of_iteration,
        test_awaited_disposer,
        test_non_disposable_once_per_then,
        test_nested_function_calls,
        test_sync_dispose_returning_promise_not_awaited,
        test_sync_dispose_throw_rejection,
    ];
    var i = 0;
    function next() {
        if (i >= tests.length)
            return Promise.resolve();
        return Promise.resolve().then(tests[i++]).then(next);
    }
    return next().then(function () {
        print("test_using_double_fire: all tests passed");
    });
}

run_all_tests().catch(function (e) {
    print("test_using_double_fire FAILED:", e, "\n", e && e.stack);
    throw e;
});
