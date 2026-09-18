// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[5] = ["c06 55 77"];
__EXP[6] = ["c07 ma mb mc md"];
// tp10: function-local const objects behave with plain (unfolded)
// semantics: writable, shadowable, closure-visible. 10 checks.
function makeVM() {
    const LOCAL = { INIT: 0, RUN: 1, DONE: 2 };
    LOCAL.state = LOCAL.INIT;
    return {
        run() { LOCAL.state = LOCAL.RUN; return LOCAL.state; },
        done() { LOCAL.state = LOCAL.DONE; return LOCAL.state; },
        state() { return LOCAL.state; },
        raw: LOCAL
    };
}
const vm = makeVM();
__A("tp10_func_local.js:c01", function () { assert_eq(vm.state(), 0, "c01"); });
__A("tp10_func_local.js:c02", function () { assert_eq(vm.run(), 1, "c02"); });
__A("tp10_func_local.js:c03", function () { assert_eq(vm.done(), 2, "c03"); });
__A("tp10_func_local.js:c04", function () { assert_eq(vm.raw.state, 2, "c04"); });

// writes via the returned alias are visible through the closure
vm.raw.state = 77;
__A("tp10_func_local.js:c05", function () { assert_eq(vm.state(), 77, "c05"); });

// per-invocation locals are distinct objects
const vm2 = makeVM();
vm2.raw.state = 55;
__L(5, "c06", vm2.state(), vm.state());

// local const used in a switch: correct dispatch
function localSwitch(mode) {
    const M = { A: 1, B: 2, C: 3 };
    switch (mode) {
        case M.A: return "ma";
        case M.B: return "mb";
        case M.C: return "mc";
        default: return "md";
    }
}
__L(6, "c07", localSwitch(1), localSwitch(2), localSwitch(3), localSwitch(4));

// local const mutated inside the switch body
function loopSwitch(n) {
    const L = { GO: 0 };
    let hits = 0;
    for (let i = 0; i < n; i++) {
        switch (L.GO) {
            case L.GO: hits++; L.GO = i; break;
            default: hits += 10; break;
        }
    }
    return hits;
}
__A("tp10_func_local.js:c08", function () { assert_eq(loopSwitch(4), 4, "c08"); });

// closure captures local const object; later mutation visible
function counterFactory() {
    const OPS = { INC: 1, AMT: 10 };
    return {
        inc() { OPS.AMT++; return OPS.AMT; },
        amt() { return OPS.AMT; }
    };
}
const c1 = counterFactory();
c1.inc(); c1.inc();
__A("tp10_func_local.js:c09", function () { assert_eq(c1.amt(), 12, "c09"); });

// local object identity stable across reads
function identityCheck() {
    const O = { X: 1 };
    return (function (g) { return g() === g(); })(function () { return O; });
}
__A("tp10_func_local.js:c10", function () { assert_eq(identityCheck(), true, "c10"); });

summary("constprop");
