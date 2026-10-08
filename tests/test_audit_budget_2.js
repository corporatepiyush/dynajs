// timeout: 300
// tests/test_audit_budget_2.js -- E3-02/03/04 + E3-05: flatten work budget,
// __proto__ group-key pollution, and the uncapped superlinear output forms.
// Runs in-process; no child processes needed.
let failures = 0;

function assert(cond, msg) {
    if (!cond) {
        failures++;
        print("  FAIL:", msg);
    }
}

function throwsRange(fn, name) {
    try {
        fn();
        assert(false, name + ": did not throw");
    } catch (e) {
        assert(e instanceof RangeError,
            name + ": threw " + e + " (wanted RangeError)");
    }
}

// ---- E3-02: flatten work budget ----
{
    const a = [];
    a.push(a, a);
    throwsRange(() => a.flatten(), "flatten self-containing");
    throwsRange(() => a.flatten(Infinity), "flatten self-containing depth Inf");
    assert(JSON.stringify([1, [2, [3]]].flatten()) === "[1,2,3]",
        "flatten legit");
    assert(JSON.stringify([1, [2, [3, [4]]]].flatten(2)) === "[1,2,3,[4]]",
        "flatten depth semantics kept (deeper than depth left nested)");
    const deep = [];
    let cur = deep;
    for (let i = 0; i < 600; i++) {
        const next = [];
        next.push(cur, cur);
        cur = next;
    }
    throwsRange(() => cur.flatten(), "flatten deep self-containing");
    assert(JSON.stringify(Array.from({ length: 1000 }, (_, i) => i).flatten(1))
        .length > 0, "flatten large flat array");
    print("  ok  flatten budget");
}

// ---- E3-03/04: group keys must be own properties ----
{
    const r1 = [{ g: "__proto__", k: "polluted", v: 1 }]
        .reduceBy((acc, e) => { acc[e.k] = e.v; return acc; }, {}, e => e.g);
    assert(({}).polluted === undefined, "reduceBy Array polluted the realm");
    assert(Object.keys(r1).includes("__proto__"), "reduceBy Array kept the group");
    assert(r1.__proto__ && r1.__proto__.polluted === 1, "reduceBy Array group value");

    const r2 = [{ g: "__proto__", k: "polluted2", v: 1 }]
        .values().reduceBy((acc, e) => { acc[e.k] = e.v; return acc; }, {}, e => e.g);
    assert(({}).polluted2 === undefined, "reduceBy Iterator polluted the realm");

    const r3 = [["__proto__", { evil: 1 }]].into({}, x => x);
    assert(r3.evil === undefined, "into retargeted __proto__");
    assert(Object.keys(r3).includes("__proto__"), "into kept the own key");
    assert(Object.prototype.evil === undefined, "into polluted Object.prototype");

    const r4 = [{ g: "__proto__" }, { g: "a" }].values().groupBy(e => e.g);
    assert(Object.keys(r4).includes("__proto__"), "iterator groupBy dropped __proto__ group");
    assert(!Array.isArray(Object.getPrototypeOf(r4)), "iterator groupBy retargeted prototype");
    assert(r4.__proto__ && r4.__proto__.length === 1, "iterator groupBy __proto__ bucket");

    const r5 = [{ g: "__proto__" }, { g: "a" }].groupBy(e => e.g);
    assert(r5.__proto__ && r5.__proto__.length === 1, "array groupBy __proto__ bucket");
    assert(!Array.isArray(Object.getPrototypeOf(r5)), "array groupBy retargeted prototype");

    const r6 = [{ g: "__proto__" }, { g: "a" }].values().countBy(e => e.g);
    assert(r6.__proto__ === 1, "countBy __proto__ count");

    // non-__proto__ special keys behave as data keys
    const r7 = ["constructor", "toString", "valueOf", "hasOwnProperty", "__proto__"]
        .groupBy(x => x);
    assert(Array.isArray(r7.constructor) && Array.isArray(r7.toString),
        "groupBy special keys as own data keys");

    // normal behavior unchanged
    const src = [{ g: "a", v: 1 }, { g: "a", v: 2 }, { g: "b", v: 3 }];
    const g = src.groupBy(e => e.g);
    assert(JSON.stringify(g) === '{"a":[{"g":"a","v":1},{"g":"a","v":2}],"b":[{"g":"b","v":3}]}',
        "groupBy normal");
    const rb = src.reduceBy((acc, e) => { acc.v = (acc.v || 0) + e.v; return acc; }, {}, e => e.g);
    assert(rb.a.v === 3 && rb.b.v === 3, "reduceBy normal");
    const it = [["a", 1], ["b", 2]].into({}, x => x);
    assert(JSON.stringify(it) === '{"a":1,"b":2}', "into normal");
    print("  ok  group-key own-property semantics");
}

// ---- E3-05: superlinear output budgets (ARR_EXT_MAX-consistent) ----
{
    // the budget bounds the OUTPUT (the product of the input lengths), so it
    // cannot depend on how many inputs produced it: 2^27 rows is over,
    // 1024 rows is under, and neither verdict moves with the input count.
    throwsRange(() => Array.from({ length: 27 }, () => [0, 1]).sequence(Array.of),
        "sequence 27-way (2^27 rows > ARR_EXT_MAX)");
    throwsRange(() => [Array.from({ length: 100000 }, (_, i) => i),
        Array.from({ length: 100000 }, (_, i) => i)].sequence(Array.of),
        "sequence two 100k inputs (1e10 rows > ARR_EXT_MAX)");
    throwsRange(() => Array.from({ length: 30 }, (_, i) => i)
        .traverse(Array.of, x => [x, -x]), "traverse 30-way");
    assert(Array.from({ length: 10 }, () => [0, 1]).sequence(Array.of).length === 1024,
        "sequence under output budget ok (1024 rows)");
    throwsRange(() => [new Array(2 ** 32 - 1)].transpose(), "transpose huge row");
    assert(Array.from({ length: 100 }, () => Array.from({ length: 50 }, (_, i) => i))
        .transpose().length === 50, "transpose small ok");
    throwsRange(() => Array.from({ length: 40000 }, (_, i) => i).aperture(20000),
        "aperture 20001x20000");
    assert(Array.from({ length: 100 }, (_, i) => i).aperture(10).length === 91,
        "aperture small ok");
    // same boundary, sane size: the pair count is len*otherLen, checked
    // before anything is allocated, so the reject side costs nothing
    throwsRange(() => Array.from({ length: 5000 }, (_, i) => i)
        .xprod(Array.from({ length: 50000 }, (_, i) => i)),
        "xprod over budget (2.5e8 pairs > ARR_EXT_MAX)");
    assert(Array.from({ length: 1000 }, (_, i) => i)
        .xprod(Array.from({ length: 1000 }, (_, i) => i)).length === 1000000,
        "xprod under budget ok (1e6 pairs <= ARR_EXT_MAX)");
    print("  ok  superlinear output budgets");
}

if (failures) {
    print("test_audit_budget_2: " + failures + " FAILURES");
    throw new Error("test_audit_budget_2 failed");
}
print("test_audit_budget_2: all tests passed");
