// F1 stale-cache probe payload (staged to scratch/ by the driver).
// Distinguishes correct for-await semantics from pre-patch stale bytecode:
// a BODY-originated throw must await the iterator close (ret=1). Executing a
// pre-fix1 .qbc blob on the fixed runtime leaves the loop's catch marker at
// -1 through the body (the missing OP_for_await_catch_body restore), so the
// close is skipped and this prints ret=0.
var rets = 0;
var it = {
    [Symbol.asyncIterator]: function () { return this; },
    next: function () { return Promise.resolve({ value: 1, done: false }); },
    return: function (v) {
        rets++;
        return Promise.resolve({ value: v, done: true });
    }
};
async function f() {
    for await (var x of it) {
        throw new Error("body");
    }
}
f().catch(function () {
    (typeof print === "function" ? print : console.log)("ret=" + rets);
});
