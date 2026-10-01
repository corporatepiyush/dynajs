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
