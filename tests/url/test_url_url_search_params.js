// covers: dyna:url.URLSearchParams
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_url_url_search_params");
import { URL, URLSearchParams, formEncode, formDecode, encodeURIComponentStrict } from "dyna:url";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}

eq(new URLSearchParams({ k: "a+b" }).toString(), "k=a%2Bb", "a literal + encodes to %2B (WHATWG form set)");

eq(new URLSearchParams("a=a+b").get("a"), "a b", "...and therefore re-decodes as a space (spec-lossy)");

eq(new URLSearchParams({ k: "a&b=c" }).toString(), "k=a%26b%3Dc", "delimiters & and = encode (WHATWG form set)");

eq(new URLSearchParams({ a: "1~2*x'y" }).toString(), "a=1~2*x%27y",
   "SP: ~ and * verbatim, ' encodes (WHATWG form set)");

eq(new URLSearchParams({ a: "b c" }).toString(), "a=b+c", "SP: space is +");

eq(new URLSearchParams({ a: "b#c" }).toString(), "a=b%23c", "SP: # is %23");

eq(new URLSearchParams({ a: "x&y=z" }).toString(), "a=x%26y%3Dz",
   "SP: = and & encode (WHATWG form set)");

__kitS.section("<!--[if<img src=x onerror=javascript:alert(205)//]> -->", () => {
    const inner2 = ["<!--[if<img src=x onerror=javascript:alert(205)//]> -->", -0.5, "`\"'><img src=xxx:x \\x22onerror=javascript:alert(119)>"];
    const wrapper = [inner2, "\\", "\u0000"];
    const outer = [wrapper, {toString: () => "x"}, {"0":128,"1":65534,"2":65535,"3":1,"4":65534,"5":65535}];
    for (let i = 0; i < 100; i++) {
        const a = [outer, {}, function(){}];
        const sp = new URLSearchParams(...a);
        assert(typeof sp.toString() === "string", "URLSearchParams spread should not crash");
        const sp2 = new URLSearchParams(outer);
        assert(typeof sp2.toString() === "string", "direct outer");
    }
    for (let i = 0; i < 200; i++) {
        const sp = new URLSearchParams({a:"1", b:"2"});
        assert(sp.get("a") === "1", "plain record");
    }
    {
        const sp = new URLSearchParams([ [1,2], [true, null], [{toString:()=>"k"},"v"] ]);
        assert(typeof sp.toString() === "string", "mixed ToString");
    }
});

__kitS.section("entries() is an iterator, not an array", () => {
    const sp = new URLSearchParams("a=1&b=2&a=3");

    assert(!Array.isArray(sp.entries()) && typeof sp.entries().next === "function",
           "entries() is an iterator, not an array");
    assert(!Array.isArray(sp.keys()) && !Array.isArray(sp.values()),
           "keys()/values() are iterators");
    assert(typeof sp[Symbol.iterator] === "function", "instance [Symbol.iterator] exists");
    {
        const it = sp.entries();
        assert(it[Symbol.iterator]() === it, "iterators return themselves from [Symbol.iterator]");
    }

    {
        const seen = [];
        for (const [k, v] of sp) seen.push(k + "=" + v);
        eq(seen.join(","), "a=1,b=2,a=3", "for..of over the instance (insertion order)");
        eq([...sp].length, 3, "spread over the instance");
        eq(Array.from(sp).length, 3, "Array.from over the instance");
        eq(JSON.stringify([...new Map(sp)]), '[["a","3"],["b","2"]]', "new Map(sp) (last a=3 wins in Map)");
    }

    {
        const it = sp.entries();
        const first = it.next();
        assert(!first.done && first.value[0] === "a" && first.value[1] === "1", "next() step 1");
        assert(it.next().value[0] === "b" && it.next().value[1] === "3", "next() steps 2-3");
        const last = it.next();
        assert(last.done === true, "next() exhausted has done === true");
        assert("value" in last && last.value === undefined,
               "exhausted result is the spec-literal {value: undefined, done: true}");
        assert(it.next().done === true, "next() past exhaustion stays done");
    }

    for (const [nm, get] of [
        ["entries", (p) => p.entries()],
        ["keys", (p) => p.keys()],
        ["values", (p) => p.values()],
    ]) {
        const p = new URLSearchParams("a=1");
        const it = get(p);
        it.next();
        assert(it.next().done === true, nm + " exhausts on a 1-pair list");
        p.append("b", "2");
        const after = it.next();
        assert(after.done === true && after.value === undefined,
               nm + " stays done after a post-exhaustion append");
    }

    {
        const it = sp.entries();
        const IteratorProto = Object.getPrototypeOf(Object.getPrototypeOf([][Symbol.iterator]()));
        assert(Object.getPrototypeOf(Object.getPrototypeOf(it)) === IteratorProto,
               "pair iterator chains to %IteratorPrototype%");
        assert(it[Symbol.toStringTag] === "URLSearchParams Iterator",
               "iterator Symbol.toStringTag");
        assert(it[Symbol.iterator]() === it, "chained iterator is still self-iterable");
    }

    eq(Array.from(sp.keys()).join(","), "a,b,a", "keys() iterator");
    eq(Array.from(sp.values()).join(","), "1,2,3", "values() iterator");

    assert(Array.isArray(sp.entriesArray()) && Array.isArray(sp.keysArray()) && Array.isArray(sp.valuesArray()),
           "*Array twins are arrays");
    eq(JSON.stringify(sp.entriesArray()), '[["a","1"],["b","2"],["a","3"]]', "entriesArray()");
    eq(JSON.stringify(sp.keysArray()), '["a","b","a"]', "keysArray()");
    eq(JSON.stringify(sp.valuesArray()), '["1","2","3"]', "valuesArray()");

    {
        const live = new URLSearchParams("x=1");
        const it = live.entries();
        it.next();
        live.append("y", "2");
        const second = it.next();
        assert(!second.done && second.value[0] === "y", "iteration is live over the pair list");
    }

    {
        const empty = new URLSearchParams("");
        const it = empty[Symbol.iterator]();
        assert(it.next().done === true, "empty instance iterates to done");
        eq([...empty].length, 0, "spread of empty");
    }

    {
        const u = new URL("https://x.test/?k=1");
        const seen = [];
        for (const [k, v] of u.searchParams) seen.push(k + "=" + v);
        eq(seen.join(","), "k=1", "bound instance iterates");
        u.searchParams.append("j", "2");
        eq([...u.searchParams.keysArray()].join(","), "k,j", "bound instance stays in sync");
    }

    {
        let it;
        {
            const u = new URL("https://drop.test/?m=9");
            it = u.searchParams.entries();
        }
        eq(it.next().value.join("="), "m=9", "iterator outlives the URL object");
    }
});
__kitS.end();

if (fails) {
    print("test_url: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_url failed");
}
print("test_url: " + n + " assertions, 0 failures");
