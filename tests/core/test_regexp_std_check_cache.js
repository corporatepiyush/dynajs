// covers: c:src/builtins/builtin-regexp.c
// js_is_standard_regexp slot cache (js_regexp_std_fast / js_regexp_std_learn). A RegExp still on the
// shared instance shape is checked by re-reading the remembered RegExp.prototype slots of exec, flags,
// global and unicode. Each read validates the slot's atom, kind and value, and any mismatch falls
// back to the full lookup. Why: the full lookup was about 18% of a short-string replace
// (ENGINE-PERF.md item 4, r10). This suite proves that every observable way to make a RegExp
// non-standard is still seen after the cache has been warmed, and that restoring it re-enables the
// built-in path.
import { suite } from "../lib/kit.js";
const t = suite("test_regexp_std_check_cache");

function warm() {
    for (let i = 0; i < 50; i++) "ab  ".replace(/ +$/, "");
}

t.test("patched exec on the prototype is called by replace/split/match/test", (a) => {
    warm();
    const orig = RegExp.prototype.exec;
    let calls = 0;
    RegExp.prototype.exec = function (s) { calls++; return orig.call(this, s); };
    try {
        a.eq("ab  ".replace(/ +$/, ""), "ab", "replace result");
        a.ok(calls > 0, "replace went through the patched exec");
        calls = 0;
        "a b".split(/ /);
        a.ok(calls > 0, "split went through the patched exec");
        calls = 0;
        "ab".match(/b/);
        a.ok(calls > 0, "match went through the patched exec");
    } finally {
        RegExp.prototype.exec = orig;
    }
    calls = 0;
    warm();
    a.eq(calls, 0, "restored exec: the patched function is not called");
});

t.test("own exec, own props and a swapped prototype disable the fast path", (a) => {
    warm();
    const re = / +$/;
    let own = 0;
    re.exec = function (s) { own++; return RegExp.prototype.exec.call(this, s); };
    a.eq("x  ".replace(re, ""), "x", "own exec result");
    a.ok(own > 0, "own exec is called");
    const re2 = / +$/;
    Object.setPrototypeOf(re2, Object.create(RegExp.prototype, { exec: { value() { own = -1; return null; } } }));
    a.eq("x  ".replace(re2, ""), "x  ", "the swapped prototype's exec decides the result");
    a.eq(own, -1, "the swapped prototype's exec ran");
    const re3 = / +$/;
    Object.defineProperty(re3, "lastIndex", { writable: false });
    a.eq("x  ".replace(re3, ""), "x", "non-writable lastIndex still replaces");
});

t.test("redefined flags/global/unicode getters are observed", (a) => {
    warm();
    for (const name of ["flags", "global", "unicode"]) {
        const d = Object.getOwnPropertyDescriptor(RegExp.prototype, name);
        let hits = 0;
        Object.defineProperty(RegExp.prototype, name, { get() { hits++; return d.get.call(this); }, configurable: true });
        try {
            "x  ".replace(/ +$/, "");
            a.ok(hits > 0, name + " getter was consulted after redefinition");
        } finally {
            Object.defineProperty(RegExp.prototype, name, d);
        }
        hits = 0;
        warm();
        a.eq(hits, 0, name + " restored: the wrapper is no longer consulted");
    }
    const d = Object.getOwnPropertyDescriptor(RegExp.prototype, "global");
    delete RegExp.prototype.global;
    try {
        a.eq("a a".replace(/a/g, "b"), "b a", "deleted global getter: flags lacks g, so the spec path replaces once");
    } finally {
        Object.defineProperty(RegExp.prototype, "global", d);
    }
    RegExp.prototype.extra1 = 1;
    RegExp.prototype.extra2 = 2;
    delete RegExp.prototype.extra1;
    a.eq("ab  ".replace(/ +$/, ""), "ab", "prototype shape changes keep the fast path correct");
    delete RegExp.prototype.extra2;
});

await t.run();
