/* Long-running single-workload driver for a sampling profiler.
 *
 * `sample` needs a process that stays in one workload long enough to collect
 * leaves; the microbench auto-scales its rep count and moves on. This runs one
 * named workload in a loop for a fixed wall-clock budget so the profile is
 * about that workload and nothing else.
 *
 *   ./dynajs tests/prof_driver.js <name> [seconds]
 */
"use strict";

var global_res = 0;

/* --- workloads, each shaped like the microbench row it mirrors --- */

function arguments_test() {
    return arguments[0] + arguments[1] + arguments[2];
}

function arguments_strict_test() {
    "use strict";
    return arguments[0] + arguments[1] + arguments[2];
}

function plain_test(a, b, c) {
    return a + b + c;
}

var W = {
    /* arguments object creation + read, against the same call without it */
    arguments_read: function (n) {
        var sum = 0;
        for (var j = 0; j < n; j++) sum += arguments_test(j, j, j);
        return sum;
    },
    arguments_strict_read: function (n) {
        var sum = 0;
        for (var j = 0; j < n; j++) sum += arguments_strict_test(j, j, j);
        return sum;
    },
    plain_call: function (n) {           /* control: same call, no arguments */
        var sum = 0;
        for (var j = 0; j < n; j++) sum += plain_test(j, j, j);
        return sum;
    },

    /* Map with string keys, split from the String() conversion the microbench
       row folds in -- that row measures both and cannot tell them apart. */
    map_set_string: function (n) {
        var keys = [], i;
        for (i = 0; i < 1000; i++) keys.push("k" + i);
        var sum = 0;
        for (var j = 0; j < n / 1000; j++) {
            var s = new Map();
            for (i = 0; i < 1000; i++) s.set(keys[i], i);
            for (i = 0; i < 1000; i++) if (s.has(keys[i])) sum++;
        }
        return sum;
    },
    map_set_int: function (n) {
        var sum = 0;
        for (var j = 0; j < n / 1000; j++) {
            var s = new Map();
            for (var i = 0; i < 1000; i++) s.set(i, i);
            for (i = 0; i < 1000; i++) if (s.has(i)) sum++;
        }
        return sum;
    },

    string_to_int: function (n) {
        var strs = [], i;
        for (i = 0; i < 100; i++) strs.push("" + (i * 7919 + 12345));
        var sum = 0;
        for (i = 0; i < n; i++) sum += parseInt(strs[i % 100]);
        return sum;
    },
    string_to_float: function (n) {
        var strs = [], i;
        for (i = 0; i < 100; i++) strs.push((i * 7919 + 12345) + ".5e2");
        var sum = 0;
        for (i = 0; i < n; i++) sum += parseFloat(strs[i % 100]);
        return sum;
    },
    int_toString: function (n) {
        var sum = 0;
        for (var i = 0; i < n; i++) sum += (i & 0xffff).toString().length;
        return sum;
    },
    float_toString: function (n) {
        var sum = 0, a = 1.0000001;
        for (var i = 0; i < n; i++) { sum += (a * i).toString().length; }
        return sum;
    },
    array_for_in: function (n) {
        var tab = [], i;
        for (i = 0; i < 100; i++) tab[i] = i;
        var sum = 0;
        for (var j = 0; j < n / 100; j++) for (var k in tab) sum += tab[k];
        return sum;
    },
    prop_delete: function (n) {
        var sum = 0;
        for (var j = 0; j < n / 100; j++) {
            var o = {};
            for (var i = 0; i < 100; i++) o["k" + i] = i;
            for (i = 0; i < 100; i++) { delete o["k" + i]; sum++; }
        }
        return sum;
    },
    prop_create: function (n) {
        var sum = 0;
        for (var j = 0; j < n / 100; j++) {
            var o = {};
            for (var i = 0; i < 100; i++) o["k" + i] = i;
            sum += o.k0;
        }
        return sum;
    }
};

var name = scriptArgs[1];
var secs = scriptArgs[2] ? +scriptArgs[2] : 6;
if (!name || !W[name]) {
    print("workloads: " + Object.keys(W).join(" "));
    throw Error("usage: prof_driver.js <name> [seconds]");
}
var fn = W[name];
var deadline = Date.now() + secs * 1000;
var iters = 0;
while (Date.now() < deadline) {
    global_res += fn(200000);
    iters++;
}
print(name + ": " + iters + " passes, res=" + (global_res | 0));
