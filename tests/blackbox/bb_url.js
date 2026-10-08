// bb_url.js -- black-box contract tests for dyna:url (WHATWG-style URL).
//
// Contract: dynajs.d.ts lines 5832-5954 (the "dyna:url" module section).
// Slice used: /tmp/dyna_contract/uring_url.d.ts.
//
// Expectation bases: the WHATWG URL Standard for attribute values of fixed
// hrefs, relative resolution, canonicalization and URLSearchParams semantics;
// UTS #46 / IDNA2008 for domainToASCII(-Unicode); RFC 3492 bootstring for
// punycode; application/x-www-form-urlencoded for form* and searchParams
// serialization.  Where the slice pins a DIFFERENT rule than WHATWG, the row
// says so with a DOC-TENSION comment and follows the slice.
//
// No network, no filesystem: this module's surface is pure.

import {
    URL as DURL,
    URLSearchParams,
    domainToASCII,
    domainToUnicode,
    punycodeEncode,
    punycodeDecode,
    formEncode,
    formDecode,
    encodeURIComponentStrict,
} from "dyna:url";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)); if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|"); }
function assertDeepEq(a, b, msg) { n++; if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error("assertion failed (deep): " + msg); }
function assertThrows(fn, msg, ErrType, errPattern) { n++; let threw = false, e = null; try { fn(); } catch (err) { threw = true; e = err; } if (!threw) throw new Error("expected throw: " + msg); if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type: " + msg); }

function runTable(tname, rows, fn) {
    for (const row of rows) {
        try { fn(row); }
        catch (e) { throw new Error("table " + tname + " row [" + row[0] + "]: " + (e && e.message ? e.message : String(e))); }
    }
}

// EXAMPLE_HREF is the classic WHATWG/URI example URL.
const EXAMPLE_HREF = "https://user:pass@example.com:8042/over/there?name=ferret#nose";

// --------------------------------------------------------------
// T1: exact attributes of a fixed href (WHATWG URL Standard)
// --------------------------------------------------------------
runTable("attributes", [
    ["href round trips verbatim", "href", EXAMPLE_HREF],
    ["protocol", "protocol", "https:"],
    ["username", "username", "user"],
    ["password", "password", "pass"],
    ["host includes explicit non-default port", "host", "example.com:8042"],
    ["hostname", "hostname", "example.com"],
    ["port", "port", "8042"],
    ["pathname", "pathname", "/over/there"],
    ["search includes the ?", "search", "?name=ferret"],
    ["hash includes the #", "hash", "#nose"],
    ["origin includes explicit non-default port", "origin", "https://example.com:8042"],
], row => {
    const u = new DURL(EXAMPLE_HREF);
    assertEq(u[row[1]], row[2], row[0]);
});

// --------------------------------------------------------------
// T2: canonicalization rows (WHATWG)
// --------------------------------------------------------------
runTable("canonicalization", [
    ["scheme+host lowercased, empty path becomes /", "HTTP://X.COM", "http://x.com/"],
    ["canonical default https port omitted from href", "https://example.com:443/x", "https://example.com/x"],
    ["canonical default http port omitted from href", "http://example.com:80/x", "http://example.com/x"],
    ["path case is preserved", "HTTPS://EXAMPLE.COM/PATH", "https://example.com/PATH"],
], row => {
    assertEq(new DURL(row[1]).href, row[2], row[0]);
});
runTable("canonicalization-origin", [
    ["http default port omitted from origin", "http://x.com", "http://x.com"],
    ["https default port omitted from origin", "https://x.com:443", "https://x.com"],
    ["non-default port kept in origin", "http://x.com:8080", "http://x.com:8080"],
    ["IPv6 host keeps brackets in origin", "http://[::1]:8080", "http://[::1]:8080"],
], row => {
    assertEq(new DURL(row[1]).origin, row[2], row[0]);
});
runTable("ipv6", [
    ["host is [addr]:port bracket form", "http://[::1]:8080/x", "host", "[::1]:8080"],
    ["hostname is bracketed [addr]", "http://[::1]:8080/x", "hostname", "[::1]"],
    ["bare IPv6 host bracket form", "http://[::1]/", "hostname", "[::1]"],
    ["IPv6 href keeps brackets", "http://[::1]/", "href", "http://[::1]/"],
], row => {
    assertEq(new DURL(row[1])[row[2]], row[3], row[0]);
});

// --------------------------------------------------------------
// T3: file: URLs (WHATWG; file origin is opaque -> "null")
// --------------------------------------------------------------
runTable("file-url", [
    ["file href", "file:///tmp/x", "href", "file:///tmp/x"],
    ["file protocol", "file:///tmp/x", "protocol", "file:"],
    ["file has no host", "file:///tmp/x", "host", ""],
    ["file pathname", "file:///tmp/x", "pathname", "/tmp/x"],
    // DOC-TENSION: WHATWG defines the origin of a file: URL as opaque, which
    // serializes to the string "null"; some engines instead return "file://".
    // This row pins the WHATWG reading per the slice's "WHATWG-style" claim.
    ["file origin is opaque 'null' (WHATWG)", "file:///tmp/x", "origin", "null"],
    ["relative resolution against a file base", "file:///a/x", null, (u) => assertEq(u.href, "file:///a/x", "resolved file href")],
], row => {
    const u = row[1] === null ? new DURL("x", "file:///a/b") : new DURL(row[1]);
    if (row[2] === null) { row[3](u); } else { assertEq(u[row[2]], row[3], row[0]); }
});

// --------------------------------------------------------------
// T4: relative resolution via the constructor (WHATWG; guidance case
// new URL("b", "http://a/c/d"))
// --------------------------------------------------------------
runTable("relative-resolution", [
    ["last segment replaced", "b", "http://a/c/d", "http://a/c/b"],
    ["../ pops a segment", "../z", "http://a/x/y", "http://a/z"],
    ["absolute reference replaces base", "http://z/", "http://a/b", "http://z/"],
    ["protocol-relative reference reuses scheme", "//x.com/y", "http://a/b", "http://x.com/y"],
    ["path-absolute reference roots at /", "/root", "http://a/b/c", "http://a/root"],
    ["query-only reference keeps path", "?q", "http://a/c/d", "http://a/c/d?q"],
    ["fragment-only reference keeps path+query", "#f", "http://a/c/d?x=1", "http://a/c/d?x=1#f"],
    ["empty reference drops the base fragment", "", "http://a/c/d#f", "http://a/c/d"],
], row => {
    assertEq(new DURL(row[1], row[2]).href, row[3], row[0]);
});
runTable("constructor-refusal", [
    ["no scheme and no base throws TypeError", () => assertThrows(() => new DURL("not a url at all"), "bare relative without base", TypeError)],
    ["forbidden host code point throws TypeError", () => assertThrows(() => new DURL("http://ex ample.com"), "space in host", TypeError)],
], row => row[1]());

// --------------------------------------------------------------
// T5: static parse / canParse (slice-pinned semantics: null for EVERY
// constructor failure; non-string still throws TypeError)
// --------------------------------------------------------------
runTable("parse", [
    ["parse of a valid URL returns it", () => {
        const r = DURL.parse("https://ok.example/p");
        assert(r !== null, "parse valid non-null");
        assertEq(r.href, "https://ok.example/p", "parse href");
    }],
    ["parse of garbage returns null", () => assertEq(DURL.parse("::not a url::"), null, "parse garbage null")],
    ["parse of a relative with a good base resolves", () => {
        const r = DURL.parse("b", "http://a/c/d");
        assert(r !== null, "parse relative non-null");
        assertEq(r.href, "http://a/c/b", "parse resolves against base");
    }],
    // TEST-FIX: "::bad::" against the good base "http://a/" is a RESOLVABLE relative reference
    // (WHATWG: the constructor does not throw, so parse() returns the URL — node agrees,
    // 'http://a/::bad::'). A bad BASE is one that itself fails to parse.
    ["parse with a bad base returns null", () => assertEq(DURL.parse("b", "::not a url::"), null, "parse bad base null")],
    ["a resolvable relative with a good base is not a parse failure", () => assertEq(DURL.parse("::bad::", "http://a/")?.href, "http://a/::bad::", "parse relative colons resolve")],
    ["parse of input over 65536 bytes returns null", () => assertEq(DURL.parse("http://a/" + "a".repeat(70000)), null, "parse oversized null")],
    ["parse of a non-string still throws TypeError", () => assertThrows(() => DURL.parse(42), "non-string input", TypeError)],
], row => row[1]());
runTable("canParse", [
    ["canParse true for a valid absolute URL", "https://ok.example/", undefined, true],
    ["canParse false for garbage", "::not a url::", undefined, false],
    ["canParse true for a resolvable relative+base", "b", "http://a/c/d", true],
    ["canParse false for an oversized input", "http://a/" + "a".repeat(70000), undefined, false],
], row => {
    assertEq(DURL.canParse(row[1], row[2]), row[3], row[0]);
});

// --------------------------------------------------------------
// T6: static join (slice documents join("https://a.com/x/y", "../z")
// === "https://a.com/z"; THROWS TypeError on unparseable base/ref)
// --------------------------------------------------------------
runTable("join", [
    ["documented example", "https://a.com/x/y", "../z", "https://a.com/z"],
    ["plain relative", "http://a/c/d", "b", "http://a/c/b"],
    ["absolute reference wins", "http://a/c/d", "http://z/", "http://z/"],
    ["query reference", "http://a/c/d", "?x", "http://a/c/d?x"],
    ["empty reference clears fragment", "http://a/c/d#f", "", "http://a/c/d"],
], row => {
    assertEq(DURL.join(row[1], row[2]), row[3], row[0]);
});
runTable("join-refusal", [
    ["unparseable base throws TypeError", () => assertThrows(() => DURL.join("::bad::", "x"), "join bad base", TypeError)],
    ["unparseable reference throws TypeError", () => assertThrows(() => DURL.join("http://a/", "http://[::"), "join bad reference", TypeError)],
], row => row[1]());

// --------------------------------------------------------------
// T7: setters -- host/hostname/port/pathname/search/hash per the
// slice's pinned rules (no-ops, digit-only port, query-state parse)
// --------------------------------------------------------------
runTable("setter-host", [
    ["host = 'b:2' splits into hostname+port", () => {
        const u = new DURL("https://a.com:1/p");
        u.host = "b.com:2";
        assertEq(u.host, "b.com:2", "host after set");
        assertEq(u.hostname, "b.com", "hostname after host set");
        assertEq(u.port, "2", "port after host set");
    }],
    ["host with '/' is a no-op (forbidden host code point)", () => {
        const u = new DURL("https://a.com/p");
        u.host = "b.com/x";
        assertEq(u.host, "a.com", "host unchanged after forbidden set");
    }],
    ["host = '' is a no-op", () => {
        const u = new DURL("https://a.com/p");
        u.host = "";
        assertEq(u.host, "a.com", "host unchanged after empty set");
    }],
    ["host-less URL has no host to set (mailto)", () => {
        const u = new DURL("mailto:user@example.com");
        u.host = "other.com";
        assertEq(u.href, "mailto:user@example.com", "mailto href unchanged");
    }],
], row => row[1]());
runTable("setter-hostname-port", [
    ["hostname lowercases and leaves port untouched", () => {
        const u = new DURL("https://a.com:8042/p");
        u.hostname = "B.TEST";
        assertEq(u.hostname, "b.test", "hostname lowercased");
        assertEq(u.port, "8042", "port untouched");
    }],
    ["hostname with '/' is a no-op", () => {
        const u = new DURL("https://a.com:8042/p");
        u.hostname = "bad/host";
        assertEq(u.hostname, "a.com", "hostname unchanged");
        assertEq(u.port, "8042", "port unchanged");
    }],
    ["port = digits sets it", () => {
        const u = new DURL("https://a.com/p");
        u.port = "8080";
        assertEq(u.port, "8080", "port set");
    }],
    ["port with non-digits is a no-op", () => {
        const u = new DURL("https://a.com:8080/p");
        u.port = "8x";
        assertEq(u.port, "8080", "port unchanged after '8x'");
    }],
    ["port = '' clears the port", () => {
        const u = new DURL("https://a.com:8080/p");
        u.port = "";
        assertEq(u.port, "", "port cleared");
        assertEq(u.href, "https://a.com/p", "href without a port");
    }],
], row => row[1]());
runTable("setter-pathname", [
    ["relative value roots at / and encodes spaces", () => {
        const u = new DURL("https://a.com/old");
        u.pathname = "x y";
        assertEq(u.pathname, "/x%20y", "pathname rooted+encoded");
    }],
    ["'?' starts the query and '#' the fragment", () => {
        const u = new DURL("https://a.com/old");
        u.pathname = "a?b#c";
        assertEq(u.pathname, "/a", "pathname part");
        assertEq(u.search, "?b", "query part");
        assertEq(u.hash, "#c", "fragment part");
    }],
    ["'.'/'..' resolve in path state", () => {
        const u = new DURL("https://a.com/old");
        u.pathname = "q/../w";
        assertEq(u.pathname, "/w", "dot segments resolved");
    }],
    ["'../x' roots at /", () => {
        const u = new DURL("https://a.com/old");
        u.pathname = "../x";
        assertEq(u.pathname, "/x", "leading .. rooted");
    }],
], row => row[1]());
runTable("setter-search-hash", [
    ["search keeps a single leading ?", () => {
        const u = new DURL("https://a.com/p");
        u.search = "?q=1";
        assertEq(u.search, "?q=1", "search with delimiter");
    }],
    ["search supplies the leading ? itself", () => {
        const u = new DURL("https://a.com/p");
        u.search = "q=2";
        assertEq(u.search, "?q=2", "search without delimiter");
    }],
    ["search = '' clears the query", () => {
        const u = new DURL("https://a.com/p?x=1");
        u.search = "";
        assertEq(u.search, "", "search cleared");
        assertEq(u.href, "https://a.com/p", "href without ?");
    }],
    ["'#' inside a search value starts the fragment", () => {
        const u = new DURL("https://a.com/p");
        u.search = "v#w";
        assertEq(u.search, "?v", "query part");
        assertEq(u.hash, "#w", "fragment part");
    }],
    ["hash supplies the leading # itself", () => {
        const u = new DURL("https://a.com/p");
        u.hash = "f2";
        assertEq(u.hash, "#f2", "hash without delimiter");
    }],
    ["hash = '' clears the fragment", () => {
        const u = new DURL("https://a.com/p#f");
        u.hash = "";
        assertEq(u.hash, "", "hash cleared");
        assertEq(u.href, "https://a.com/p", "href without #");
    }],
    ["a later # is fragment content, unencoded", () => {
        const u = new DURL("https://a.com/p");
        u.hash = "#a#b";
        assertEq(u.hash, "#a#b", "second # kept verbatim");
    }],
], row => row[1]());

// --------------------------------------------------------------
// T8: searchParams -- bound instance writes through (slice: settable,
// replaces the whole query; "" clears)
// --------------------------------------------------------------
runTable("searchParams-bound", [
    ["bound instance reads the URL query", () => {
        const u = new DURL("https://x.com/p?a=1");
        assertEq(u.searchParams.get("a"), "1", "read through");
    }],
    ["append writes through to search+href", () => {
        const u = new DURL("https://x.com/p?a=1");
        u.searchParams.append("b", "2");
        assertEq(u.search, "?a=1&b=2", "search after append");
        assertEq(u.href, "https://x.com/p?a=1&b=2", "href after append");
    }],
    ["delete writes through", () => {
        const u = new DURL("https://x.com/p?a=1&b=2");
        u.searchParams.delete("a");
        assertEq(u.search, "?b=2", "search after delete");
    }],
    ["set writes through", () => {
        const u = new DURL("https://x.com/p?a=1");
        u.searchParams.set("a", "9");
        assertEq(u.search, "?a=9", "search after set");
    }],
    ["searchParams = string replaces the query", () => {
        const u = new DURL("https://x.com/p?a=1");
        u.searchParams = "x=1&y=2";
        assertEq(u.search, "?x=1&y=2", "search after string assignment");
    }],
    ["searchParams = pair array replaces the query", () => {
        const u = new DURL("https://x.com/p?a=1");
        u.searchParams = [["m", "3"]];
        assertEq(u.search, "?m=3", "search after array assignment");
    }],
    ["searchParams = record replaces the query", () => {
        const u = new DURL("https://x.com/p?a=1");
        u.searchParams = { z: "4" };
        assertEq(u.search, "?z=4", "search after record assignment");
    }],
    ["searchParams = '' clears the query", () => {
        const u = new DURL("https://x.com/p?a=1");
        u.searchParams = "";
        assertEq(u.search, "", "search cleared");
        assertEq(u.href, "https://x.com/p", "href without query");
    }],
], row => row[1]());

// --------------------------------------------------------------
// T9: standalone URLSearchParams (ordering = insertion; "+"/%20
// encode as space; two-arg has; sort; iteration)
// --------------------------------------------------------------
runTable("URLSearchParams", [
    ["ctor from string keeps duplicates", () => {
        const sp = new URLSearchParams("a=1&b=2&a=3");
        assertEq(sp.size, 3, "size");
        assertEq(sp.get("a"), "1", "get returns the first");
        assertDeepEq(sp.getAll("a"), ["1", "3"], "getAll in order");
    }],
    ["get(missing) === null, getAll(missing) === []", () => {
        const sp = new URLSearchParams("a=1");
        assertEq(sp.get("zz"), null, "get missing");
        assertDeepEq(sp.getAll("zz"), [], "getAll missing");
    }],
    ["has(name) and two-arg has(name, value)", () => {
        const sp = new URLSearchParams("a=1&b=2&a=3");
        assertEq(sp.has("b"), true, "has b");
        assertEq(sp.has("zz"), false, "has missing");
        assertEq(sp.has("a", "3"), true, "has(a,3)");
        assertEq(sp.has("a", "2"), false, "has(a,2)");
    }],
    ["append preserves insertion order", () => {
        const sp = new URLSearchParams();
        sp.append("a", "1");
        sp.append("b", "2");
        sp.append("a", "3");
        assertEq(sp.toString(), "a=1&b=2&a=3", "insertion order kept");
    }],
    ["delete removes every pair with the name", () => {
        const sp = new URLSearchParams("a=1&b=2&a=3");
        sp.delete("a");
        assertEq(sp.toString(), "b=2", "all a pairs gone");
        assertEq(sp.size, 1, "size after delete");
    }],
    ["set replaces the first and drops the rest", () => {
        const sp = new URLSearchParams("a=1&a=2&b=3");
        sp.set("a", "9");
        assertEq(sp.toString(), "a=9&b=3", "set semantics");
        assertDeepEq(sp.getAll("a"), ["9"], "getAll after set");
    }],
    ["sort orders by name, stable for equal names", () => {
        const sp = new URLSearchParams("b=2&a=1&a=0");
        sp.sort();
        assertEq(sp.toString(), "a=1&a=0&b=2", "stable sort by name");
    }],
    ["'+' on input decodes to space; %20 too", () => {
        const sp = new URLSearchParams("k=b+c%20d");
        assertEq(sp.get("k"), "b c d", "plus and %20 decode");
    }],
    ["toString encodes space as + and reserved bytes, keeps ~ verbatim", () => {
        const sp = new URLSearchParams();
        sp.append("q", "x y");
        sp.append("m", "a&b=c");
        sp.append("t", "~");
        // WHATWG urlencoded serializer: ~ is not in the percent-encode set.
        assertEq(sp.toString(), "q=x+y&m=a%26b%3Dc&t=~", "urlencoded serialization");
    }],
    ["toString: = inside a name is %3D, ~ stays verbatim", () => {
        assertEq(new URLSearchParams([["k=1~2", ""]]).toString(), "k%3D1~2=",
            "WHATWG #urlencoded-serializing");
    }],
    ["toString: space in a value becomes +", () => {
        assertEq(new URLSearchParams([["k", "a b"]]).toString(), "k=a+b",
            "WHATWG #urlencoded-serializing");
    }],
    ["toString: & in a value is %26", () => {
        assertEq(new URLSearchParams([["k", "a&b"]]).toString(), "k=a%26b",
            "WHATWG #urlencoded-serializing");
    }],
    ["toString: + in a value is %2B", () => {
        assertEq(new URLSearchParams([["k", "a+b"]]).toString(), "k=a%2Bb",
            "WHATWG #urlencoded-serializing");
    }],
    ["ctor from pair array and from record", () => {
        assertEq(new URLSearchParams([["a", "1"], ["b", "2"]]).toString(), "a=1&b=2", "array init");
        assertEq(new URLSearchParams({ a: "1", b: "2" }).toString(), "a=1&b=2", "record init");
        assertEq(new URLSearchParams(new URLSearchParams("a=1&b=2")).toString(), "a=1&b=2", "copy init");
    }],
    ["round trip toString -> ctor is byte-stable", () => {
        const sp = new URLSearchParams("a=1&b=x+y&m=a%26b");
        assertDeepEq(new URLSearchParams(sp.toString()).entriesArray(), sp.entriesArray(), "parse(serialize) === original pairs");
    }],
    ["keysArray/valuesArray/entriesArray in insertion order", () => {
        const sp = new URLSearchParams("a=1&b=2&a=3");
        assertDeepEq(sp.keysArray(), ["a", "b", "a"], "keysArray");
        assertDeepEq(sp.valuesArray(), ["1", "2", "3"], "valuesArray");
        assertDeepEq(sp.entriesArray(), [["a", "1"], ["b", "2"], ["a", "3"]], "entriesArray");
    }],
    ["entriesArray is a snapshot, not live", () => {
        const sp = new URLSearchParams("a=1");
        const arr = sp.entriesArray();
        sp.append("zz", "9");
        assertEq(arr.length, 1, "snapshot length unchanged");
        assertEq(sp.size, 2, "live size grew");
    }],
    ["[Symbol.iterator] is the entries iterator (for..of/Map)", () => {
        const sp = new URLSearchParams("a=1&b=2");
        const out = [];
        for (const [k, v] of sp) out.push(k + "=" + v);
        assertDeepEq(out, ["a=1", "b=2"], "for..of pairs");
        const m = new Map(sp);
        assertEq(m.size, 2, "Map size");
        assertEq(m.get("b"), "2", "Map value");
    }],
    ["forEach walks value,key in order", () => {
        const sp = new URLSearchParams("a=1&b=2");
        const out = [];
        sp.forEach((v, k) => out.push(k + "=" + v));
        assertDeepEq(out, ["a=1", "b=2"], "forEach order");
    }],
    ["keys()/values()/entries() are live iterators, latched done", () => {
        const sp = new URLSearchParams("a=1&b=2");
        assertDeepEq(Array.from(sp.keys()), ["a", "b"], "keys iterator");
        assertDeepEq(Array.from(sp.values()), ["1", "2"], "values iterator");
        const it = sp.entries();
        it.next(); it.next();
        assertEq(it.next().done, true, "entries exhausted");
        assertEq(it.next().done, true, "entries latched done");
    }],
], row => row[1]());

// --------------------------------------------------------------
// T10: idempotence -- reparsing a serialized href is the identity
// --------------------------------------------------------------
runTable("idempotence", [
    ["example href reparses to itself", () => {
        const u = new DURL(EXAMPLE_HREF);
        assertEq(new DURL(u.href).href, EXAMPLE_HREF, "reparse identity");
    }],
    ["canonicalized href reparses to itself", () => {
        const u = new DURL("HTTP://X.COM");
        assertEq(new DURL(u.href).href, u.href, "reparse identity after canonicalization");
    }],
    ["toString/toJSON === href", () => {
        const u = new DURL(EXAMPLE_HREF);
        assertEq(u.toString(), EXAMPLE_HREF, "toString");
        assertEq(u.toJSON(), EXAMPLE_HREF, "toJSON");
    }],
    ["mutated href reparses to itself", () => {
        const u = new DURL("https://a.com/p");
        u.search = "q=1";
        u.hash = "f";
        u.pathname = "x y";
        assertEq(new DURL(u.href).href, u.href, "reparse identity after mutation");
    }],
], row => row[1]());

// --------------------------------------------------------------
// T11: IDNA / punycode / form codecs
// --------------------------------------------------------------
runTable("idna", [
    ["ASCII passthrough", "example.com", undefined, "example.com"],
    ["IDN label maps + lowercases (UTS #46)", "BÜCHER.de", undefined, "xn--bcher-kva.de"],
    ["lowercase IDN label", "bücher.de", undefined, "xn--bcher-kva.de"],
    // UTS #46: nontransitional keeps ß (-> xn--fa-hia), transitional maps ß -> ss.
    ["ß nontransitional stays sharp s", "faß.de", undefined, "xn--fa-hia.de"],
    ["ß transitional maps to ss", "faß.de", { transitional: true }, "fass.de"],
    ["empty domain maps to empty", "", undefined, ""],
    ["domainToUnicode reverses the label", "xn--bcher-kva.de", undefined, "bücher.de", true],
], row => {
    const got = row[4] ? domainToUnicode(row[1]) : (row[2] === undefined ? domainToASCII(row[1]) : domainToASCII(row[1], row[2]));
    assertEq(got, row[3], row[0]);
});
runTable("punycode", [
    // RFC 3492 bootstring; the raw encoding carries no xn-- prefix (that is
    // the IDNA layer): bücher -> bcher-kva (the canonical xn--bcher-kva.de label).
    ["encode bücher", "bücher", "bcher-kva"],
    // TEST-FIX: RFC 3492's reference encoder emits the delimiter after the basic code points
    // whenever b > 0 (Python: 'abc'.encode('punycode') === b'abc-'), so pure-basic input
    // encodes to "abc-"; the decoder accepts both spellings.
    ["all-basic input passes through", "abc", "abc-"],
    ["decode reverses encode", "bcher-kva", "bücher"],
    ["round trip decode(encode(x)) === x", "München", "roundtrip"],
    ["round trip with CJK", "ドメイン", "roundtrip"],
], row => {
    if (row[2] === "roundtrip") {
        assertEq(punycodeDecode(punycodeEncode(row[1])), row[1], row[0]);
    } else if (row[0].indexOf("decode") === 0) {
        assertEq(punycodeDecode(row[1]), row[2], row[0]);
    } else {
        assertEq(punycodeEncode(row[1]), row[2], row[0]);
    }
});
// refusal row: slice pins "input over 1024 code points is refused"
assertThrows(() => punycodeEncode("a".repeat(1100)), "punycodeEncode over 1024 code points");
runTable("form-codecs", [
    ["formEncode simple record in key order", () => assertEq(formEncode({ a: "1", b: "2" }), "a=1&b=2", "formEncode order")],
    ["formEncode space becomes +", () => assertEq(formEncode({ k: "x y" }), "k=x+y", "formEncode space")],
    ["formEncode encodes reserved bytes", () => assertEq(formEncode({ k: "a&b=c" }), "k=a%26b%3Dc", "formEncode reserved")],
    ["formEncode of {} is empty", () => assertEq(formEncode({}), "", "formEncode empty")],
    ["formDecode splits pairs", () => assertDeepEq(formDecode("a=1&b=2"), { a: "1", b: "2" }, "formDecode basic")],
    ["formDecode treats + as space", () => assertDeepEq(formDecode("k=a+b"), { k: "a b" }, "formDecode plus")],
    ["formDecode decodes %20", () => assertDeepEq(formDecode("k=x%20y"), { k: "x y" }, "formDecode percent")],
    // slice pin: "keeps the LAST value per key"
    ["formDecode keeps the LAST value per key", () => assertDeepEq(formDecode("k=1&k=2"), { k: "2" }, "formDecode last wins")],
    ["formDecode of an empty value", () => assertDeepEq(formDecode("k="), { k: "" }, "formDecode empty value")],
    ["formDecode reverses formEncode", () => assertDeepEq(formDecode(formEncode({ a: "1", b: "x y" })), { a: "1", b: "x y" }, "form round trip")],
], row => row[1]());
runTable("encodeURIComponentStrict", [
    // slice pin: "encodeURIComponent plus !'()~" -- those five join the
    // escaped set; standard unescaped A-Za-z0-9 - _ . ! ~ * ' ( ) lose
    // exactly ! ' ( ) ~.
    ["plain alnum and -._* stay", "a-b.c_d*e9", "a-b.c_d*e9"],
    ["! is escaped", "!", "%21"],
    ["' is escaped", "'", "%27"],
    ["( is escaped", "(", "%28"],
    [") is escaped", ")", "%29"],
    ["~ is escaped", "~", "%7E"],
    ["space is escaped", "a b", "a%20b"],
    ["/ is escaped", "/", "%2F"],
    ["= is escaped", "=", "%3D"],
    ["& is escaped", "&", "%26"],
], row => {
    assertEq(encodeURIComponentStrict(row[1]), row[2], row[0]);
});

// --------------------------------------------------------------
// T12: entriesArray twins (audit sweep) -- d.ts pins the former ARRAY
// returns as entriesArray()/keysArray()/valuesArray(), "for code that
// indexed or measured the results": real Arrays, equal to what the
// WHATWG live iterators walk, measuring sp.size.
// --------------------------------------------------------------
runTable("entriesArray twins", [
    ["twins are real Arrays (d.ts: Array<[string,string]> / string[])", () => {
        const sp = new URLSearchParams("a=1&b=2");
        assertEq(Array.isArray(sp.entriesArray()), true, "entriesArray isArray");
        assertEq(Array.isArray(sp.keysArray()), true, "keysArray isArray");
        assertEq(Array.isArray(sp.valuesArray()), true, "valuesArray isArray");
    }],
    ["the twins describe the same pairs the live iterators yield", () => {
        const sp = new URLSearchParams("a=1&b=2&a=3");
        assertDeepEq(Array.from(sp.entries()), sp.entriesArray(), "entries() === entriesArray()");
        assertDeepEq(Array.from(sp.keys()), sp.keysArray(), "keys() === keysArray()");
        assertDeepEq(Array.from(sp.values()), sp.valuesArray(), "values() === valuesArray()");
    }],
    ["the twins measure the pair list (sp.size)", () => {
        const sp = new URLSearchParams("a=1&b=2&a=3");
        assertEq(sp.entriesArray().length, sp.size, "entriesArray measures size");
        assertEq(sp.keysArray().length, sp.size, "keysArray measures size");
        assertEq(sp.valuesArray().length, sp.size, "valuesArray measures size");
        assertEq(sp.valuesArray()[2], sp.entriesArray()[2][1], "valuesArray is entriesArray's second column");
    }],
], row => row[1]());

print("bb_url: all tests passed (" + n + " assertions)");
