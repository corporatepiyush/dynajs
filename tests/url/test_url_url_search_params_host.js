// covers: dyna:url.URLSearchParams
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_url_url_search_params_host");
import { URL, URLSearchParams } from "dyna:url";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("  FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function throws(fn, m) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    ok(t, m);
}

print("=== 1. hostname normalization ===");

print("=== 3. URLSearchParams basics ===");
__kitS.section("get returns the first value", () => {
    const sp = new URLSearchParams("a=1&b=2&a=3");
    eq(sp.get("a"), "1", "get returns the first value");
    eq(sp.getAll("a").join(","), "1,3", "getAll returns every value");
    eq(sp.get("b"), "2", "get b");
    eq(sp.get("missing"), null, "get missing returns null");
    eq(sp.has("a"), true, "has true");
    eq(sp.has("missing"), false, "has false");
    eq(sp.size, 3, "size");
    eq(sp.toString(), "a=1&b=2&a=3", "toString round-trips");
});

__kitS.section("append order", () => {
    const sp = new URLSearchParams();
    sp.append("k", "v1");
    sp.append("k", "v2");
    sp.append("x", "y");
    eq(sp.toString(), "k=v1&k=v2&x=y", "append order");
    sp.set("k", "V");
    eq(sp.toString(), "k=V&x=y", "set replaces the first, drops the rest");
    sp.set("new", "n");
    eq(sp.toString(), "k=V&x=y&new=n", "set appends a new key");
    sp.delete("x");
    eq(sp.toString(), "k=V&new=n", "delete removes the key");
    sp.delete("k");
    sp.delete("new");
    eq(sp.toString(), "", "deleting everything yields an empty query");
    eq(sp.size, 0, "size 0 after delete-all");
});

__kitS.section("'+' decodes to space, %2F to '/'", () => {
    const sp = new URLSearchParams("a+b=c/d&=v&empty=");
    eq(sp.get("a b"), "c/d", "'+' decodes to space, %2F to '/'");
    eq(sp.get(""), "v", "empty key preserved");
    eq(sp.get("empty"), "", "empty value preserved");
    // FIX2: node's WHATWG serializer re-encodes space as '+' and '/' as %2F.
    eq(sp.toString(), "a+b=c%2Fd&=v&empty=", "serialization uses the WHATWG form set");
});

__kitS.section("sort orders by key, stable within a key", () => {
    const sp = new URLSearchParams("c=3&a=1&b=2&a=0");
    sp.sort();
    eq(sp.toString(), "a=1&a=0&b=2&c=3", "sort orders by key, stable within a key");
});

__kitS.section("sort is stable:", () => {
    const N = 512;
    const sp = new URLSearchParams(Array.from({ length: N }, (_, i) => ["a", String(i)]));
    sp.sort();
    const vals = sp.getAll("a");
    let stable = true;
    for (let i = 0; i < N; i++)
        if (vals[i] !== String(i)) { stable = false; break; }
    ok(stable, "sort is stable: " + N + " equal (name, value) pairs keep insertion order");
});

print("=== 3b. coercion and two-argument delete ===");
__kitS.section("get coerces the key (12 -> \"12\")", () => {
    const sp = new URLSearchParams("12=3");
    eq(sp.get(12), "3", "get coerces the key (12 -> \"12\")");
    eq(sp.has(12), true, "has coerces the key too");
    sp.append(12, true);
    eq(sp.toString(), "12=3&12=true", "append coerces key and value");
    const d = new URLSearchParams("a=1&a=2&a=1&b=3");
    d.delete("a", "1");
    eq(d.toString(), "a=2&b=3", "delete(name, value) removes only matching pairs");
    d.delete("a", "nope");
    eq(d.toString(), "a=2&b=3", "delete(name, value) with an absent value removes nothing");
    d.delete("b");
    eq(d.toString(), "a=2", "single-argument delete still removes every value");
});

print("=== 3c. URLSearchParams cache coherency (O(n^2) fix regression) ===");
__kitS.section("get after set sees the new value", () => {
    const sp = new URLSearchParams("a=1");
    sp.set("a", "2");
    eq(sp.get("a"), "2", "get after set sees the new value");
    sp.append("b", "3");
    eq(sp.get("b"), "3", "get after append (cache went stale via the concat path)");
    sp.delete("a");
    eq(sp.get("a"), null, "get after delete");
    eq(sp.size, 1, "size after the mutation mix");
    sp.sort();
    eq(sp.toString(), "b=3", "toString after sort");
});

__kitS.section("forEach walks a snapshot: mid-loop delete changes nothing", () => {
    const sp = new URLSearchParams("a=1&b=2&c=3");
    let seen = "";
    sp.forEach(function (v, k) {
        seen += k + v + ",";
        sp.delete("b");
    });
    eq(seen, "a1,b2,c3,", "forEach walks a snapshot: mid-loop delete changes nothing");
    eq(sp.toString(), "a=1&c=3", "the delete still landed");
});

__kitS.section("an empty array pair is skipped", () => {
    eq(new URLSearchParams([[], ["a"]]).toString(), "a=", "an empty array pair is skipped");
    const sp = new URLSearchParams("x=1");
    eq(sp.has("x", "1"), true, "has(name, value): match");
    eq(sp.has("x", "2"), false, "has(name, value): value mismatch");
    eq(sp.has("y", "1"), false, "has(name, value): name mismatch");
    eq(sp.has("x"), true, "has(name) still name-only");
    eq(sp.has("x", undefined), true, "has(name, undefined) is the name-only form");
});

print("=== 5. constructors ===");
__kitS.section("from string", () => {
    eq(new URLSearchParams("a=1&b=2").toString(), "a=1&b=2", "from string");
    eq(new URLSearchParams("?a=1").toString(), "a=1", "leading '?' stripped");
    eq(new URLSearchParams([["a", "1"], ["b", "2"], ["a", "3"]]).toString(),
       "a=1&b=2&a=3", "from pair list");
    eq(new URLSearchParams([["a"]]).toString(), "a=", "pair without value");
    eq(new URLSearchParams({ a: "1", b: "2" }).toString(), "a=1&b=2", "from record");
    eq(new URLSearchParams({ "a b": "c d" }).toString(), "a+b=c+d", "record keys encoded");
    eq(new URLSearchParams().toString(), "", "empty ctor");
    eq(new URLSearchParams(null).toString(), "", "null init");
    eq(new URLSearchParams(undefined).toString(), "", "undefined init");
    const src = new URLSearchParams("a=1");
    eq(new URLSearchParams(src).toString(), "a=1", "copy of another URLSearchParams");
    const pair = new URLSearchParams([["x", "y"]]);
    eq(pair.get("x"), "y", "pairs accessible");
});

print("=== 6. iteration surface ===");
__kitS.section("keys() iterates", () => {
    const sp = new URLSearchParams("a=1&b=2");
    eq(Array.from(sp.keys()).join(","), "a,b", "keys() iterates");
    eq(Array.from(sp.values()).join(","), "1,2", "values() iterates");
    eq(Array.from(sp.entries()).map((p) => p.join("=")).join(","), "a=1,b=2", "entries() iterates");
    eq(sp.keysArray().join(","), "a,b", "keysArray()");
    eq(sp.valuesArray().join(","), "1,2", "valuesArray()");
    eq(sp.entriesArray().map((p) => p.join("=")).join(","), "a=1,b=2", "entriesArray()");
    eq([...sp].map((p) => p.join("=")).join(","), "a=1,b=2", "Symbol.iterator spreads");
    eq(Array.from(sp).length, 2, "Array.from works");
    const seen = [];
    sp.forEach((v, k, self) => { seen.push(v + "=" + k); ok(self === sp, "forEach 3rd arg is the params"); });
    eq(seen.join(","), "1=a,2=b", "forEach arg order is (value, key)");
});
__kitS.end();

if (fails) {
    print("test_url_host: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_url_host failed");
}
print("test_url_host: " + n + " assertions, 0 failures");
