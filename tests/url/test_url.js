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

{
    const u = new URL("https://user:pw@example.com:8443/a/b?x=1&y=2#frag");
    eq(u.protocol, "https:", "protocol keeps its colon");
    eq(u.username, "user", "username");
    eq(u.password, "pw", "password");
    eq(u.hostname, "example.com", "hostname excludes the port");
    eq(u.port, "8443", "port");
    eq(u.host, "example.com:8443", "host includes the port");
    eq(u.pathname, "/a/b", "pathname");
    eq(u.search, "?x=1&y=2", "search keeps its question mark");
    eq(u.hash, "#frag", "hash keeps its octothorpe");
    eq(u.origin, "https://example.com:8443", "origin");
    eq(u.href, "https://user:pw@example.com:8443/a/b?x=1&y=2#frag", "href round-trips");
    eq(String(u), u.href, "toString is href");
    eq(JSON.stringify({ u }), '{"u":"' + u.href + '"}', "toJSON is href");
}

eq(new URL("http://a.com:80/").port, "", "http port 80 is dropped");
eq(new URL("https://a.com:443/").port, "", "https port 443 is dropped");
eq(new URL("http://a.com:8080/").port, "8080", "a non-default port is kept");
eq(new URL("http://a.com:80/").href, "http://a.com/", "the dropped port is gone from href");
eq(new URL("http://a.com/").origin, new URL("http://a.com:80/").origin,
   "the two spellings of one origin agree");

eq(new URL("HTTP://a.com/").protocol, "http:", "scheme is lowercased");
eq(new URL("hTtPs://a.com/").protocol, "https:", "mixed-case scheme");

{
    const u = new URL("http://a.com/p");
    eq(u.search, "", "no query -> empty search");
    eq(u.hash, "", "no fragment -> empty hash");
    eq(u.username, "", "no userinfo -> empty username");
    eq(u.port, "", "no port -> empty port");
}
eq(new URL("http://a.com/?").search, "", "an empty query serialises as empty");
eq(new URL("http://a.com/#").hash, "", "an empty fragment serialises as empty");

eq(new URL("http://[::1]/x").hostname, "[::1]", "IPv6 hostname keeps brackets");
eq(new URL("http://[::1]:8080/x").port, "8080", "IPv6 with a port");
eq(new URL("http://[::1]:8080/x").host, "[::1]:8080", "IPv6 host");
eq(new URL("http://[2001:db8::1]/").hostname, "[2001:db8::1]", "full IPv6 literal");

eq(new URL("http:/example.com/").hostname, "example.com", "http:/ one slash still names its authority");
eq(new URL("http:/example.com/").pathname, "/", "http:/ path");
eq(new URL("http:example.com").hostname, "example.com", "http: with no slash still names its authority");
eq(new URL("http:example.com").href, new URL("http://example.com").href, "the spellings are one URL (an empty path stays empty)");
eq(new URL("https:\\\\evil.test/x").hostname, "evil.test", "backslashes join the slash run");
eq(new URL("ws:example.com/").href, "ws://example.com/", "ws: too");
eq(new URL("ftp:example.com/").href, "ftp://example.com/", "ftp: too");
eq(new URL("file:example.com").pathname, "/example.com", "file: no-slash is a fresh path over the empty host (the file state never goes opaque)");
throws(() => new URL("http:/"), "http:/ has an empty host: refused");
throws(() => new URL("http:"), "http: alone has an empty host: refused");
throws(() => new URL("http:?q"), "http:? has an empty host: refused");
{
    const b = new URL("http://a/b/c/d;p?q");
    eq(new URL("g", b).href, "http://a/b/c/g", "relative refs keep relative semantics");
    eq(new URL("//g", b).href, "http://g/", "a network-path ref parses its authority and, like every special URL with an empty path, carries one empty segment (WHATWG, not RFC 3986)");
}

{
    const u = new URL("mailto:ada@example.com");
    eq(u.protocol, "mailto:", "mailto protocol");
    eq(u.pathname, "ada@example.com", "mailto path is opaque");
    eq(u.origin, "null", "a scheme with no authority has origin null");
}
eq(new URL("data:text/plain,hi").pathname, "text/plain,hi", "data: opaque path");

eq(new URL("file:///x").href, "file:///x", "file:///x parses (file: may have an empty host)");
eq(new URL("file:///x").origin, "null", "a file: URL has an opaque origin");
eq(new URL("file://host/x").origin, "null", "file: origin is null even with a host");
eq(new URL("http://x.test/").origin, "http://x.test", "http origin is untouched");

eq(new URL("https://a.com/p{x}").href, "https://a.com/p%7Bx%7D", "path braces are percent-encoded");
eq(new URL("https://a.com/p`x").href, "https://a.com/p%60x", "a path backtick is percent-encoded");
eq(new URL("https://a.com/p|q^r").href, "https://a.com/p|q%5Er", "path ^ is percent-encoded (U+005E is in the standard's path set), | stays literal");
eq(new URL("https://a.com/p%2fq%2fr").href, "https://a.com/p%2fq%2fr", "existing escapes keep their hex case verbatim ('%' is in no percent-encode set)");
eq(new URL("https://a.com/p?q{x}`y").href, "https://a.com/p?q{x}`y", "the query set keeps { } ` literal");
eq(new URL("https://a.com/p#f`x{").href, "https://a.com/p#f%60x{", "the fragment set encodes `, not {");

{
    const u = new URL("http://a.com/p?q=a?b/c#f#g?h");
    eq(u.search, "?q=a?b/c", "a query may contain ? and /");
    eq(u.hash, "#f#g?h", "a fragment may contain # and ?");
    eq(u.pathname, "/p", "the path stops at the first ?");
}

const BASE = "http://a/b/c/d;p?q";
const NORMAL = [
    ["g:h", "g:h"], ["g", "http://a/b/c/g"], ["./g", "http://a/b/c/g"],
    ["g/", "http://a/b/c/g/"], ["/g", "http://a/g"], ["//g", "http://g/"],
    ["?y", "http://a/b/c/d;p?y"], ["g?y", "http://a/b/c/g?y"],
    ["#s", "http://a/b/c/d;p?q#s"], ["g#s", "http://a/b/c/g#s"],
    ["g?y#s", "http://a/b/c/g?y#s"], [";x", "http://a/b/c/;x"],
    ["g;x", "http://a/b/c/g;x"], ["g;x?y#s", "http://a/b/c/g;x?y#s"],
    ["", "http://a/b/c/d;p?q"], [".", "http://a/b/c/"], ["./", "http://a/b/c/"],
    ["..", "http://a/b/"], ["../", "http://a/b/"], ["../g", "http://a/b/g"],
    ["../..", "http://a/"], ["../../", "http://a/"], ["../../g", "http://a/g"],
];
let normalOk = 0;
for (const [ref, want] of NORMAL) {
    let got;
    try { got = new URL(ref, BASE).href; } catch (e) { got = "THREW: " + e.message; }
    if (got === want) normalOk++;
    else assert(false, "RFC 3986 5.4.1 <" + ref + "> -> " + JSON.stringify(got)
                       + " want " + JSON.stringify(want));
}
eq(normalOk, NORMAL.length, "all " + NORMAL.length + " RFC 3986 normal examples");

const ABNORMAL = [
    ["../../../g", "http://a/g"], ["../../../../g", "http://a/g"],
    ["/./g", "http://a/g"], ["/../g", "http://a/g"],
    ["g.", "http://a/b/c/g."], [".g", "http://a/b/c/.g"],
    ["g..", "http://a/b/c/g.."], ["..g", "http://a/b/c/..g"],
    ["./../g", "http://a/b/g"], ["./g/.", "http://a/b/c/g/"],
    ["g/./h", "http://a/b/c/g/h"], ["g/../h", "http://a/b/c/h"],
    ["g;x=1/./y", "http://a/b/c/g;x=1/y"], ["g;x=1/../y", "http://a/b/c/y"],
];
let abnormalOk = 0;
for (const [ref, want] of ABNORMAL) {
    let got;
    try { got = new URL(ref, BASE).href; } catch (e) { got = "THREW: " + e.message; }
    if (got === want) abnormalOk++;
    else assert(false, "RFC 3986 5.4.2 <" + ref + "> -> " + JSON.stringify(got)
                       + " want " + JSON.stringify(want));
}
eq(abnormalOk, ABNORMAL.length, "all " + ABNORMAL.length + " RFC 3986 abnormal examples");

assert("http://a/b/c/" + "../../../g" !== "http://a/g",
       "fault injection: concatenation really does differ from resolution");

throws(() => new URL("not a url"), "a bare string with no scheme and no base");
throws(() => new URL("/relative/only"), "a relative reference with no base");
throws(() => new URL(42), "a non-string input");
throws(() => new URL("http://a\x01b/"), "a non-strippable C0 byte in the authority is refused");
eq(new URL("http://a\r\nb/").hostname, "ab", "interior CR/LF is stripped input-wide");
eq(new URL("http://a\tb/").hostname, "ab", "interior TAB is stripped input-wide");
eq(new URL("http://a.com/x\ty\nz").pathname, "/xyz", "interior tab/LF never reach .pathname");
eq(new URL("http://a.com/p", "http://a\tb.com/").href, "http://a.com/p", "the BASE is stripped too");
throws(() => new URL("http://a.com", "also not a url"), "an invalid base");
throws(() => new URL("http://a.com:99999/"), "a port above 65535");
throws(() => new URL("http://a.com:abc/"), "a non-numeric port");
throws(() => new URL("http://[::1/"), "an unterminated IPv6 literal");
throws(() => new URL("http://a.com/" + "x".repeat(70000)), "an over-long URL");

eq(new URL("  http://a.com/  ").href, "http://a.com/", "surrounding space is stripped");
eq(new URL("\thttp://a.com/\n").href, "http://a.com/", "tabs and newlines are stripped");

eq(formDecode("a=1&b=2").a, "1", "formDecode simple");
eq(formDecode("a=1&b=2").b, "2", "formDecode second key");
eq(formDecode("?a=1").a, "1", "formDecode tolerates a leading ?");
eq(formDecode("a=hello+world").a, "hello world", "+ decodes to a space");
eq(formDecode("a=%20%2B%3D").a, " +=", "percent escapes decode");
eq(formDecode("a").a, "", "a key with no = has an empty value");
eq(formDecode("a=").a, "", "a key with an empty value");
eq(formDecode("").a, undefined, "an empty string decodes to nothing");
eq(formDecode("a=1&a=2").a, "2", "a repeated key takes the last value");
eq(formDecode("a=1&&b=2").b, "2", "an empty pair is skipped");
eq(formDecode("a=%zz").a, "%zz", "a malformed escape stays literal");
eq(formDecode("a=%4").a, "%4", "a truncated escape stays literal");
eq(formDecode("a=%E4%BD%A0").a, "\u4F60", "UTF-8 percent escapes decode");

{
    const o = formDecode("__proto__=polluted&x=1");
    assert(Object.prototype.hasOwnProperty.call(o, "__proto__"),
           "formDecode writes __proto__ as an OWN property");
    eq(({}).polluted, undefined, "formDecode polluted nothing");
    eq(o.x, "1", "the rest of the query still decoded");
}
{
    const o = formDecode("constructor=x&prototype=y");
    eq(({}).prototype, undefined, "constructor/prototype keys pollute nothing");
    eq(o.prototype, "y", "and they still land as own properties");
}

eq(formEncode({ a: "1", b: "2" }), "a=1&b=2", "formEncode simple");
eq(formEncode({ a: "hello world" }), "a=hello+world", "space encodes to +");
eq(formEncode({ a: "x&y=z" }), "a=x%26y%3Dz", "delimiters are escaped");
eq(formEncode({ a: "\u4F60" }), "a=%E4%BD%A0", "non-ASCII encodes as UTF-8");
eq(formEncode({}), "", "an empty object encodes to an empty string");
eq(formEncode({ a: undefined, b: "1" }), "b=1", "undefined values are omitted");
eq(formEncode({ a: 1, b: true }), "a=1&b=true", "non-string values are coerced");
throws(() => formEncode("not an object"), "formEncode refuses a non-object");

for (const v of ["", " ", "a b", "a%b", "\u4F60\u597D",
                 "\u{1f600}", "!'()~*", "\n\t"]) {
    eq(formDecode(formEncode({ k: v })).k, v,
       "form round trip: " + JSON.stringify(v));
}
// FIX2: the WHATWG urlencoded serializer encodes '+' as %2B (node parity).
eq(new URLSearchParams({ k: "a+b" }).toString(), "k=a%2Bb", "a literal + encodes to %2B (WHATWG form set)");
eq(new URLSearchParams("a=a+b").get("a"), "a b", "...and therefore re-decodes as a space (spec-lossy)");
eq(new URLSearchParams({ k: "a&b=c" }).toString(), "k=a%26b%3Dc", "delimiters & and = encode (WHATWG form set)");

{
    const u = new URL("http://a.com/p?q#f");
    for (const k of ["href", "protocol", "username", "password", "origin"])
        throws(() => { u[k] = "zz"; }, k + " still has no setter");
    u.host = "b.com";
    eq(u.href, "http://b.com/p?q#f", "host is settable (UL-1)");
    u.hostname = "c.com";
    eq(u.href, "http://c.com/p?q#f", "hostname is settable (UL-1)");
    u.port = "8080";
    eq(u.href, "http://c.com:8080/p?q#f", "port is settable (UL-1)");
    u.pathname = "/z";
    eq(u.href, "http://c.com:8080/z?q#f", "pathname is settable (UL-1)");
    u.search = "s=1";
    eq(u.href, "http://c.com:8080/z?s=1#f", "search is settable (UL-1)");
    u.searchParams = "u=3";
    eq(u.href, "http://c.com:8080/z?u=3#f", "searchParams is replaceable (UL-1)");
    u.hash = "#g";
    eq(u.href, "http://c.com:8080/z?u=3#g", "hash is settable (UL-1)");
}

eq(formEncode({ a: "x&y=z" }), "a=x%26y%3Dz", "formEncode keeps its strict delimiters");
// WHATWG urlencoded serializer keeps * and ~ verbatim; only ' joins the
// escaped set here (%27). Node parity:
// new URLSearchParams({a:"1~2*x'y"}).toString() === "a=1~2*x%27y".
eq(new URLSearchParams({ a: "1~2*x'y" }).toString(), "a=1~2*x%27y",
   "SP: ~ and * verbatim, ' encodes (WHATWG form set)");
eq(new URLSearchParams({ a: "b c" }).toString(), "a=b+c", "SP: space is +");
eq(new URLSearchParams({ a: "b#c" }).toString(), "a=b%23c", "SP: # is %23");
eq(new URLSearchParams({ a: "x&y=z" }).toString(), "a=x%26y%3Dz",
   "SP: = and & encode (WHATWG form set)");

eq(encodeURIComponentStrict("!'()~"), "%21%27%28%29%7E",
   "strict encoding escapes !'()~");
assert(encodeURIComponent("!'()~") === "!'()~",
       "fault injection: the builtin really does leave them alone");
// FIX2: the doc pins "like encodeURIComponent but also escapes !'()~" --
// encodeURIComponent escapes space as %20, never '+'.
eq(encodeURIComponentStrict("a b"), "a%20b", "strict encoding uses %20 for space");
throws(() => encodeURIComponentStrict(42), "strict encoding refuses a non-string");

{
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
}

{
    const OVER = "https://a.test/" + "x".repeat(70000);
    const CASES = [
        ["https://a.test/p", undefined, true],
        ["https://a.test/p", "https://b.test/", true],
        ["/p?q", "https://b.test/", true],
        ["../z", "https://b.test/x/y", true],
        ["", "https://b.test/", true],
        ["", undefined, false],
        ["http://", undefined, false],
        [":", undefined, false],
        ["https://ex ample.com/", undefined, false],
        ["http://a.test/%zz", undefined, true],
        ["http://[::1", undefined, false],
        ["::::", undefined, false],
        ["/p", "not a url", false],
        ["https://a.test/p", "::::", false],
        [OVER, undefined, false],
        ["ht\ttp://x.test/", undefined, true],
        ["http://x.test/\n.", undefined, true],
    ];
    for (const [input, base, good] of CASES) {
        let ctorThrew = false, ctorHref = null;
        try {
            const u = base === undefined ? new URL(input) : new URL(input, base);
            ctorHref = u.href;
        } catch { ctorThrew = true; }
        assert(ctorThrew === !good, "ctor parity for " + JSON.stringify(input.slice(0, 30)));
        const p = base === undefined ? URL.parse(input) : URL.parse(input, base);
        const c = base === undefined ? URL.canParse(input) : URL.canParse(input, base);
        assert(good ? (p !== null && c === true) : (p === null && c === false),
           "parse/canParse parity for " + JSON.stringify(input.slice(0, 30)) + " base=" + base);
        if (good) assert(p.href === ctorHref, "parse href matches ctor for " + input.slice(0, 40));
        assert(URL.canParse(input, null) === (base === undefined ? c : URL.canParse(input, base)) || base !== undefined,
               "null base tolerated");
    }
    {
        class MyURL extends URL {}
        const viaSub = URL.parse("https://plain.test/");
        assert(!(viaSub instanceof MyURL), "URL.parse returns the plain class");
        assert(viaSub instanceof URL, "URL.parse result is a URL");
    }
    {
        let t1 = false, t2 = false;
        try { URL.parse(42); } catch (e) { t1 = e instanceof TypeError; }
        try { URL.canParse({}); } catch (e) { t2 = e instanceof TypeError; }
        assert(t1 && t2, "statics keep the ctor's non-string TypeError");
    }

    eq(URL.join("https://a.com/x/y", "../z"), "https://a.com/z", "join ../ past one segment");
    eq(URL.join("https://a.com/x/y", "/root"), "https://a.com/root", "join absolute path rel");
    eq(URL.join("https://a.com/x/y", "https://other.test/q"), "https://other.test/q", "join absolute rel wins");
    eq(URL.join("https://a.com/x/y?q=1#f", ""), "https://a.com/x/y?q=1", "join empty rel keeps path+query, clears fragment (RFC 3986 5.2.2)");
    eq(URL.join("https://a.com/x/y", "./p"), "https://a.com/x/p", "join ./p");
    eq(URL.join("https://a.com/x/y", "p?q=2"), "https://a.com/x/p?q=2", "join keeps rel query");
    eq(URL.join("https://a.com/x/y", "//other.test/p"), "https://other.test/p", "join scheme-relative rel");
    eq(URL.join("https://a.com/x/y", "../../../../../z"), "https://a.com/z", "join .. past root is dropped");
    throws(() => URL.join("::::", "/p"), "join throws on a bad base");
    throws(() => URL.join("https://a.com/", OVER), "join throws on an over-long rel");
    throws(() => URL.join("https://a.com/"), "join requires both arguments");
}

{
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
}

if (fails) {
    print("test_url: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_url failed");
}
print("test_url: " + n + " assertions, 0 failures");

{
    const u = new URL("https://example.com/?a=1");
    if (u.searchParams !== u.searchParams) throw new Error("searchParams must be [SameObject]");
    const sp = u.searchParams;
    for (let i = 0; i < 5000; i++) sp.append("k" + i, "v");
    const t0 = Date.now();
    for (let i = 0; i < 500; i++) u.searchParams.append("m" + i, "v");
    const dt = Date.now() - t0;
    if (dt > 2000) throw new Error("memoized append idiom regressed: " + dt + "ms for 500 appends");
    print("test_url: SameObject + memoized-idiom ok (" + dt + "ms/500)");
}
