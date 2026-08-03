/* ECMAScript Explicit Resource Management test suite:
   Symbol.dispose / Symbol.asyncDispose, SuppressedError, DisposableStack,
   AsyncDisposableStack. Style follows tests/test_modern.js (no harness). */
"use strict";

function assert(actual, expected, message) {
    if (arguments.length === 1)
        expected = true;
    if (actual === expected)
        return;
    throw Error("assertion failed: got |" + actual + "|, expected |" +
                expected + "|" + (message ? " (" + message + ")" : ""));
}

function assert_throws(expected_error, func, message) {
    var err = false;
    try {
        func();
    } catch (e) {
        err = true;
        if (!(e instanceof expected_error))
            throw Error("unexpected exception type: " + e +
                        (message ? " (" + message + ")" : ""));
    }
    if (!err)
        throw Error("expected exception" + (message ? " (" + message + ")" : ""));
}

/*---- well-known symbols ----*/

function test_symbols() {
    assert(typeof Symbol.dispose, "symbol");
    assert(typeof Symbol.asyncDispose, "symbol");
    assert(Symbol.dispose.toString(), "Symbol(Symbol.dispose)");
    assert(Symbol.asyncDispose.description, "Symbol.asyncDispose");
    assert(Symbol.dispose !== Symbol.asyncDispose, true);
    var d = Object.getOwnPropertyDescriptor(Symbol, "dispose");
    assert(d.writable, false);
    assert(d.enumerable, false);
    assert(d.configurable, false);
}

/*---- SuppressedError ----*/

function test_suppressed_error() {
    assert(typeof SuppressedError, "function");
    assert(SuppressedError.length, 3);
    assert(SuppressedError.prototype.name, "SuppressedError");
    assert(SuppressedError.prototype.message, "");
    assert(Object.getPrototypeOf(SuppressedError.prototype), Error.prototype);

    var err = {}, sup = {};
    var e = new SuppressedError(err, sup, "boom");
    assert(e instanceof SuppressedError, true);
    assert(e instanceof Error, true);
    assert(e.error, err);
    assert(e.suppressed, sup);
    assert(e.message, "boom");
    assert(e.toString(), "SuppressedError: boom");
    assert(JSON.stringify(Object.getOwnPropertyNames(e).slice(0, 3)),
           JSON.stringify(["message", "error", "suppressed"]));

    var e2 = new SuppressedError();
    assert(Object.prototype.hasOwnProperty.call(e2, "message"), false);
    assert(Object.prototype.hasOwnProperty.call(e2, "error"), true);
    assert(e2.error, undefined);
    assert(e2.suppressed, undefined);
}

/*---- DisposableStack ----*/

function test_disposable_basic() {
    assert(typeof DisposableStack, "function");
    assert(DisposableStack.length, 0);
    assert(DisposableStack.name, "DisposableStack");
    assert(DisposableStack.prototype[Symbol.dispose],
           DisposableStack.prototype.dispose);
    assert(Object.prototype.toString.call(new DisposableStack()),
           "[object DisposableStack]");

    var log = [];
    var stack = new DisposableStack();
    assert(stack.disposed, false);
    var r1 = { [Symbol.dispose]() { log.push("r1"); } };
    assert(stack.use(r1), r1);
    assert(stack.adopt(42, function (v) { log.push("adopt" + v); }), 42);
    assert(stack.defer(function () { log.push("defer"); }), undefined);
    assert(stack.use(null), null);
    assert(stack.use(undefined), undefined);
    stack.dispose();
    assert(stack.disposed, true);
    assert(JSON.stringify(log), JSON.stringify(["defer", "adopt42", "r1"]));

    /* idempotent */
    log.length = 0;
    stack.dispose();
    assert(log.length, 0);
    /* disposed stack rejects mutation with ReferenceError */
    assert_throws(ReferenceError, function () { stack.use(r1); });
    assert_throws(ReferenceError, function () { stack.defer(function () {}); });
}

function test_disposable_use_errors() {
    assert_throws(TypeError, function () { new DisposableStack().use(true); });
    assert_throws(TypeError, function () { new DisposableStack().use(1); });
    assert_throws(TypeError, function () { new DisposableStack().use({}); });
    assert_throws(TypeError, function () {
        new DisposableStack().use({ [Symbol.dispose]: 42 });
    });
    assert_throws(TypeError, function () {
        DisposableStack.prototype.use.call({}, {});
    });
    assert_throws(TypeError, function () {
        new DisposableStack().adopt(1, "not a function");
    });
}

function test_disposable_move() {
    var log = [];
    var s1 = new DisposableStack();
    s1.defer(function () { log.push(1); });
    s1.defer(function () { log.push(2); });
    var s2 = s1.move();
    assert(s1.disposed, true);
    assert(s2.disposed, false);
    assert(s2 instanceof DisposableStack, true);
    assert(s2 !== s1, true);
    s1.dispose();
    assert(log.length, 0, "move did not dispose");
    s2.dispose();
    assert(JSON.stringify(log), JSON.stringify([2, 1]));

    class Sub extends DisposableStack {}
    var m2 = new Sub().move();
    assert(m2 instanceof DisposableStack && !(m2 instanceof Sub), true);
}

function test_disposable_suppressed_chain() {
    var e1 = new Error("1"), e2 = new Error("2"), e3 = new Error("3");
    var st = new DisposableStack();
    st.defer(function () { throw e1; });
    st.defer(function () { throw e2; });
    st.defer(function () { throw e3; });
    var caught;
    try { st.dispose(); } catch (e) { caught = e; }
    assert(caught instanceof SuppressedError, true);
    assert(caught.error, e1);
    assert(caught.suppressed instanceof SuppressedError, true);
    assert(caught.suppressed.error, e2);
    assert(caught.suppressed.suppressed, e3);

    /* single error passes through unwrapped */
    var st2 = new DisposableStack();
    st2.defer(function () { throw e1; });
    var c2;
    try { st2.dispose(); } catch (e) { c2 = e; }
    assert(c2, e1);
}

/*---- AsyncDisposableStack ----*/

async function test_async_disposable() {
    assert(typeof AsyncDisposableStack, "function");
    assert(AsyncDisposableStack.length, 0);
    assert(AsyncDisposableStack.prototype[Symbol.asyncDispose],
           AsyncDisposableStack.prototype.disposeAsync);
    assert(Object.prototype.toString.call(new AsyncDisposableStack()),
           "[object AsyncDisposableStack]");

    var log = [];
    var s = new AsyncDisposableStack();
    s.defer(async function () { await 0; log.push("a"); });
    s.defer(function () { log.push("b"); });
    s.use({ async [Symbol.asyncDispose]() { await 0; log.push("c"); } });
    /* sync-dispose fallback on an async stack */
    s.use({ [Symbol.dispose]() { log.push("d"); } });
    var ret = s.disposeAsync();
    assert(ret instanceof Promise, true);
    await ret;
    assert(s.disposed, true);
    assert(JSON.stringify(log), JSON.stringify(["d", "c", "b", "a"]));
}

async function test_async_suppressed_chain() {
    var e1 = new Error("1"), e2 = new Error("2"), e3 = new Error("3");
    var s = new AsyncDisposableStack();
    s.defer(async function () { await 0; throw e1; });
    s.defer(function () { throw e2; });
    s.defer(async function () { await 0; throw e3; });
    var caught;
    try { await s.disposeAsync(); } catch (e) { caught = e; }
    assert(caught instanceof SuppressedError, true);
    assert(caught.error, e1);
    assert(caught.suppressed.error, e2);
    assert(caught.suppressed.suppressed, e3);

    /* empty stack resolves; disposed stack is a no-op that resolves */
    var s2 = new AsyncDisposableStack();
    await s2.disposeAsync();
    assert(s2.disposed, true);
    await s2.disposeAsync();
}

/*---- run ----*/

test_symbols();
test_suppressed_error();
test_disposable_basic();
test_disposable_use_errors();
test_disposable_move();
test_disposable_suppressed_chain();

async function run_async_tests() {
    await test_async_disposable();
    await test_async_suppressed_chain();
    print("test_disposable: all tests passed");
}
run_async_tests().catch(function (e) {
    print("test_disposable FAILED:", e, "\n", e && e.stack);
    throw e;
});
