/* test_url_host.js -- WHATWG host normalization (E3) and URLSearchParams.
 *
 * Host rules pinned here, all verified against Node as the oracle:
 *   - special schemes ASCII-lowercase the host and run LENIENT IDNA
 *     (disallowed code points like '_' are kept -- the strict exported
 *     domainToASCII refuses them, so it must never be used for URL hosts)
 *   - non-special schemes keep the host verbatim, percent-encoding only the
 *     bytes above 0x7F
 *   - IPv6 literals are case-folded hex only
 *   - the WHATWG forbidden host code points (space < > \ ^ |) are refused
 * URLSearchParams: append/delete/get/getAll/has/set/sort/toString/size/
 * forEach/keys/values/entries/Symbol.iterator, both standalone and BOUND to
 * a URL (a mutation changes url.search and url.href).
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_url_host.js
 */
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
eq(new URL("https://EXAMPLE.COM/").hostname, "example.com", "special: ASCII lowercased");
eq(new URL("https://EXAMPLE.COM/").host, "example.com", "host too");
eq(new URL("foo://EXAMPLE.com/").hostname, "EXAMPLE.com", "non-special: case kept");
eq(new URL("foo://m%C3%BCnich.example/").hostname, "m%C3%BCnich.example", "non-special: pre-encoded input kept");
eq(new URL("https://münich.example/").hostname, "xn--mnich-kva.example", "special: unicode host punycoded");
eq(new URL("https://münich.example/").href, "https://xn--mnich-kva.example/", "href reflects the punycoded host");
eq(new URL("http://_foo.example/").hostname, "_foo.example", "underscore kept (lenient IDNA)");
eq(new URL("http://münich_fail.example/").hostname, "xn--mnich_fail-9db.example", "disallowed kept, still punycoded");
eq(new URL("http://[::A]/").hostname, "[::a]", "IPv6 literal case-folded");
eq(new URL("HTTP://Example.Com/").hostname, "example.com", "scheme-insensitive lowercase");

print("=== 2. forbidden host code points are refused ===");
throws(() => new URL("http://exa mple.com/"), "space in host");
throws(() => new URL("http://exa<mple.com/"), "'<' in host");
throws(() => new URL("http://exa>mple.com/"), "'>' in host");
throws(() => new URL("http://exa|mple.com/"), "'|' in host");
throws(() => new URL("http://exa^mple.com/"), "'^' in host");
throws(() => new URL("foo://exa\\mple.com/"), "backslash in a non-special host");

print("=== 2b. WHATWG IPv4 hosts (D4): a host ending in a number is an address ===");
eq(new URL("http://2130706433/").hostname, "127.0.0.1", "a bare 32-bit integer is an IPv4 address");
eq(new URL("http://0x7f.1/").hostname, "127.0.0.1", "hex parts are place-valued");
eq(new URL("http://0X7F.1/").hostname, "127.0.0.1", "0X prefix too");
eq(new URL("http://127.1/").hostname, "127.0.0.1", "fewer than four parts: the last carries the remainder");
eq(new URL("http://0x7f000001/").hostname, "127.0.0.1", "one hex part carries the whole address");
eq(new URL("http://0377.1.1.1/").hostname, "255.1.1.1", "a leading-0 part is octal");
eq(new URL("http://1.2.3.04/").hostname, "1.2.3.4", "octal 04 is 4");
eq(new URL("http://256/").hostname, "0.0.1.0", "the LAST part may exceed 255 (place value)");
eq(new URL("http://0/").hostname, "0.0.0.0", "the zero address");
eq(new URL("http://1.2.3./").hostname, "1.2.0.3", "one trailing dot is ignored");
eq(new URL("http://a.b.c/").hostname, "a.b.c", "a normal name is untouched");
eq(new URL("http://0xzz/").hostname, "0xzz", "hex-SHAPED garbage is not a number: stays a name");
eq(new URL("http://ex0x1.com/").hostname, "ex0x1.com", "a name containing 0x stays a name");
throws(() => new URL("http://999.1.1.1/"), "an ALMOST-address is a parse failure, not a name");
throws(() => new URL("http://09/"), "9 is not an octal digit");
throws(() => new URL("http://1..2.3/"), "an empty part is refused");
throws(() => new URL("http://1.2.3.4.5/"), "five parts is refused");
throws(() => new URL("http://0x100000000/"), "a part past 32 bits is refused");
throws(() => new URL("http://4294967296/"), "the last part cannot carry 2^32");
throws(() => new URL("http://42949672960000000000/"), "an arbitrarily large part overflows");

print("=== 2c. bracketed IPv6 literals are validated and canonicalized (D5) ===");
eq(new URL("http://[::1]/").hostname, "[::1]", "::1 parses");
eq(new URL("http://[2001:DB8::1]/").hostname, "[2001:db8::1]", "hex groups are lowercased");
eq(new URL("http://[0:0:0:0:0:0:0:1]/").hostname, "[::1]", "the first-longest zero run compresses");
eq(new URL("http://[::A]/").hostname, "[::a]", "single hex digit");
eq(new URL("http://[1:2:3:4:5:6:1.2.3.4]/").hostname, "[1:2:3:4:5:6:102:304]", "an embedded IPv4 tail serializes as hex groups");
throws(() => new URL("http://[zz]/"), "an invalid literal is refused");
throws(() => new URL("http://[]/"), "an empty literal is refused");
throws(() => new URL("http://[1:2:3:4:5:6:7:8:9]/"), "nine groups is refused");
throws(() => new URL("http://[1:2:3:]/"), "a lone trailing colon is refused");
throws(() => new URL("http://[::1.2.3.4.5]/"), "a five-part IPv4 tail is refused");
throws(() => new URL("http://[::01.2.3.4]/"), "a leading zero in the IPv4 tail is refused");
throws(() => new URL("http://[::1]j/"), "bytes after ']' are refused, not silently dropped");
throws(() => new URL("foo://[zz]/"), "bracket validation applies to non-special schemes too");

print("=== 2d. WPT urltestdata rows (hand-transcribed, node-arbitrated) ===");
/* userinfo: the LAST '@' splits, the FIRST ':' in it splits name/password,
   and the userinfo set encodes : ; = @ / (node pins pa%3Ass and a%40b). */
eq(new URL("http://user:pa:ss@example.com/").password, "pa%3Ass", "':' in a password is percent-encoded");
eq(new URL("http://a@b@c/").username, "a%40b", "an earlier '@' rides in the username");
eq(new URL("http://a@b@c/").host, "c", "the last '@' wins the host");
eq(new URL("http://u/p@example.com/").host, "u", "an '@' after the authority is a path character");
/* ports: 0 is a port (not a default); leading zeros are syntax; the OTHER
   special schemes drop their defaults too. */
eq(new URL("http://x:0/").port, "0", "port 0 is kept");
eq(new URL("http://foo:00000000000000000000080/").href, "http://foo/", "leading zeros are syntax, not error");
eq(new URL("ws://example.com:80/").href, "ws://example.com/", "ws drops its default 80");
eq(new URL("ftp://example.com:21/").href, "ftp://example.com/", "ftp drops its default 21");
/* opaque (non-special) hosts keep '%' escapes VERBATIM -- no decoding -- and
   a malformed escape is a validation error only, NOT a failure (node pins). */
eq(new URL("sc://a%2Fb/").hostname, "a%2Fb", "opaque host: %2F is kept encoded");
eq(new URL("sc://a%2z/").hostname, "a%2z", "opaque host: a malformed escape does not fail the parse");
/* IDNA hosts (the lenient URL-host pipeline; see test_idna.js for the
   strict exported domainToASCII). */
eq(new URL("http://münchen.de/").hostname, "xn--mnchen-3ya.de", "a U-label punycodes");
eq(new URL("http://xn--mnchen-3ya.de/").hostname, "xn--mnchen-3ya.de", "an A-label round-trips");
eq(new URL("http://xn--zzz.ru/").hostname, "xn--zzz.ru", "a decodable A-label with no U-label history is kept");
eq(new URL("http://a。b.com/").hostname, "a.b.com", "U+3002 maps to a label separator");
eq(new URL("http://。com/").hostname, ".com", "the resulting EMPTY label is not checked in URL hosts (node pins)");
eq(new URL("http://_foo.example/").hostname, "_foo.example", "disallowed_STD3 '_' is kept by the lenient host mapping");
throws(() => new URL("http://a\uFFFDb.com/"), "U+FFFD is plain-disallowed: refused");
eq(new URL("https://xn--/").hostname, "xn--", "the bare A-label is a valid URL host (VerifyDnsLength=false per the URL standard; node/ICU diverges)");
/* origin: tuple for the special schemes, "null" otherwise, blob unwraps. */
eq(new URL("blob:https://example.com:443/uuid").origin, "https://example.com", "blob origin unwraps the inner URL");
eq(new URL("blob:http://[::1]:8080/x").origin, "http://[::1]:8080", "blob origin with an IPv6 inner URL");
eq(new URL("blob:ftp://x/").origin, "null", "a blob over a non-http inner URL is null");
eq(new URL("ws://x:80/").origin, "ws://x", "ws origin drops the default port");
eq(new URL("gopher://x:70/").origin, "null", "gopher is NOT special: origin null");
/* file: drive spellings. */
eq(new URL("file://D|/x").href, "file:///D:/x", "a '|' drive in the authority becomes ':' in the path");
eq(new URL("file:///D|/x").href, "file:///D:/x", "the same for a path drive");
eq(new URL("file:host:12/x").href, "file:///host:12/x", "no authority: 'host:12' is an ordinary path segment");
/* ignore-slashes leaders and the not-a-base-URL family (node pins). */
eq(new URL("http:///p").href, "http://p/", "any slash run collapses into one authority");
eq(new URL("http:[::1]:2/").href, "http://[::1]:2/", "a special scheme needs no slashes at all");

print("=== 3. URLSearchParams basics ===");
{
    const sp = new URLSearchParams("a=1&b=2&a=3");
    eq(sp.get("a"), "1", "get returns the first value");
    eq(sp.getAll("a").join(","), "1,3", "getAll returns every value");
    eq(sp.get("b"), "2", "get b");
    eq(sp.get("missing"), null, "get missing returns null");
    eq(sp.has("a"), true, "has true");
    eq(sp.has("missing"), false, "has false");
    eq(sp.size, 3, "size");
    eq(sp.toString(), "a=1&b=2&a=3", "toString round-trips");
}
{
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
}
{
    const sp = new URLSearchParams("a+b=c/d&=v&empty=");
    eq(sp.get("a b"), "c/d", "'+' decodes to space, %2F to '/'");
    eq(sp.get(""), "v", "empty key preserved");
    eq(sp.get("empty"), "", "empty value preserved");
    /* Updated for the WHATWG urlencoded set (D8): '/' is no longer re-escaped
       on the way out, so the round trip normalizes c%2Fd to c/d. */
    eq(sp.toString(), "a+b=c/d&=v&empty=", "serialization uses the WHATWG form set");
}
{
    const sp = new URLSearchParams("c=3&a=1&b=2&a=0");
    sp.sort();
    eq(sp.toString(), "a=1&a=0&b=2&c=3", "sort orders by key, stable within a key");
}
{
    /* qsort was UNSTABLE: equal (name, value) pairs could be reordered. With
       ALL keys equal every comparison is a tie, so any instability is directly
       visible in the serialized order (and large enough N defeats the
       small-array insertion sort that makes tiny inputs look stable). */
    const N = 512;
    const sp = new URLSearchParams(Array.from({ length: N }, (_, i) => ["a", String(i)]));
    sp.sort();
    const vals = sp.getAll("a");
    let stable = true;
    for (let i = 0; i < N; i++)
        if (vals[i] !== String(i)) { stable = false; break; }
    ok(stable, "sort is stable: " + N + " equal (name, value) pairs keep insertion order");
}

print("=== 3b. coercion and two-argument delete ===");
{
    /* get/has used to THROW on a non-string; WHATWG says the key is a
       USVString, so it is ToString-coerced. */
    const sp = new URLSearchParams("12=3");
    eq(sp.get(12), "3", "get coerces the key (12 -> \"12\")");
    eq(sp.has(12), true, "has coerces the key too");
    sp.append(12, true);
    eq(sp.toString(), "12=3&12=true", "append coerces key and value");
    /* delete(name, value): the second argument used to be ignored. */
    const d = new URLSearchParams("a=1&a=2&a=1&b=3");
    d.delete("a", "1");
    eq(d.toString(), "a=2&b=3", "delete(name, value) removes only matching pairs");
    d.delete("a", "nope");
    eq(d.toString(), "a=2&b=3", "delete(name, value) with an absent value removes nothing");
    d.delete("b");
    eq(d.toString(), "a=2", "single-argument delete still removes every value");
}

print("=== 3c. URLSearchParams cache coherency (O(n^2) fix regression) ===");
{
    /* The parsed list is cached and revalidated against a version counter:
       these rows pin the shapes that used the cache wrong if it ever went
       stale or stayed fresh when it should not. */
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
}
{
    /* Two BOUND instances on one URL: writes through one must be visible to
       the other (the second cache must go stale, not serve the old list). */
    const u = new URL("http://x/?a=1");
    const p1 = u.searchParams, p2 = u.searchParams;
    p1.append("b", "2");
    eq(p2.get("b"), "2", "sibling sees the write (stale cache revalidated)");
    eq(p2.size, 2, "sibling size too");
    p2.set("a", "9");
    eq(p1.get("a"), "9", "and in the other direction");
    eq(u.search, "?a=9&b=2", "the URL slot agrees with both");
}
{
    /* forEach iterates the state AT CALL TIME: mutations from the callback
       do not disturb the ongoing walk (the engine's pinned snapshot
       semantics -- the pairs are parsed before the loop starts). */
    const sp = new URLSearchParams("a=1&b=2&c=3");
    let seen = "";
    sp.forEach(function (v, k) {
        seen += k + v + ",";
        sp.delete("b");          /* must not skip or revisit pairs */
    });
    eq(seen, "a1,b2,c3,", "forEach walks a snapshot: mid-loop delete changes nothing");
    eq(sp.toString(), "a=1&c=3", "the delete still landed");
}
{
    /* an empty pair in the array init contributes nothing (it used to
       stringify an array hole into the literal key "undefined") */
    eq(new URLSearchParams([[], ["a"]]).toString(), "a=", "an empty array pair is skipped");
    /* get/has/getAll read through the cache: a get loop must not see a
       half-applied set if it could not complete */
    const sp = new URLSearchParams("x=1");
    eq(sp.has("x", "1"), true, "has(name, value): match");
    eq(sp.has("x", "2"), false, "has(name, value): value mismatch");
    eq(sp.has("y", "1"), false, "has(name, value): name mismatch");
    eq(sp.has("x"), true, "has(name) still name-only");
    eq(sp.has("x", undefined), true, "has(name, undefined) is the name-only form");
}

print("=== 4. bound searchParams live-update the URL ===");
{
    const u = new URL("http://x.test/p?old=1");
    const sp = u.searchParams;
    eq(sp.get("old"), "1", "bound instance reads the URL's query");
    sp.set("new", "2");
    eq(u.search, "?old=1&new=2", "search reflects the mutation");
    eq(u.href, "http://x.test/p?old=1&new=2", "href reflects the mutation");
    sp.delete("old");
    eq(u.href, "http://x.test/p?new=2", "delete reflected in href");
    sp.append("old", "1");
    eq(u.href, "http://x.test/p?new=2&old=1", "append reflected in href");
    eq(u.searchParams.get("new"), "2", "a fresh access sees the same list");
    /* PIN, T-URL-6: searchParams is [SameObject] (WHATWG URL sec.6.1: the
       getter returns the URL's one "query object"), so the engine now
       memoizes one bound wrapper per URL. The suite used to pin the opposite
       (a fresh object per access), which also made `u.searchParams.append`
       loops O(n^2) -- each fresh wrapper's first mutation reparsed the whole
       query. */
    const sp2 = u.searchParams;
    ok(sp2 === sp, "each searchParams access returns the same object ([SameObject])");
    eq(sp2.toString(), "new=2&old=1", "...and it binds the same URL");
}
{
    const u = new URL("http://x.test/p");
    const sp = u.searchParams;
    sp.append("a", "1");
    eq(u.search, "?a=1", "appending to an empty query creates it");
}
{
    const u = new URL("http://x.test/p?x=1");
    const sp = u.searchParams;
    sp.delete("x");
    eq(u.search, "", "deleting the last key empties .search");
}

print("=== 5. constructors ===");
{
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
}

print("=== 6. iteration surface ===");
{
    const sp = new URLSearchParams("a=1&b=2");
    eq(sp.keys().join(","), "a,b", "keys()");
    eq(sp.values().join(","), "1,2", "values()");
    eq(sp.entries().map((p) => p.join("=")).join(","), "a=1,b=2", "entries()");
    eq([...sp].map((p) => p.join("=")).join(","), "a=1,b=2", "Symbol.iterator spreads");
    eq(Array.from(sp).length, 2, "Array.from works");
    const seen = [];
    sp.forEach((v, k, self) => { seen.push(v + "=" + k); ok(self === sp, "forEach 3rd arg is the params"); });
    eq(seen.join(","), "1=a,2=b", "forEach arg order is (value, key)");
}

print("=== 7. bound object lifetime ===");
{
    /* the bound params must outlive the URL object (raw-ref semantics) */
    let sp;
    {
        const u = new URL("http://x.test/p?a=1");
        sp = u.searchParams;
    }
    eq(sp.get("a"), "1", "bound params usable after the URL is unreachable");
    sp.append("b", "2");
    eq(sp.toString(), "a=1&b=2", "standalone mutations after unbinding");
}


print("=== 8. subclassing: the constructor honors new.target.prototype ===");
{
    /* The native constructors used to wrap with the CLASS proto directly, so
       a subclass's own methods were missing on its instances ("m.tag is not
       a function"). WHATWG URL is subclassable; JS requires the
       [[Construct]]ed object to carry new.target.prototype. */
    class MyURL extends URL {
        tagged() { return "T:" + this.protocol; }
    }
    const m = new MyURL("http://x/a?b#c");
    eq(m.href, "http://x/a?b#c", "subclass instance parses like the base");
    eq(m.tagged(), "T:http:", "subclass methods are present");
    ok(m instanceof MyURL && m instanceof URL, "instanceof chain intact");

    class MyParams extends URLSearchParams {
        add(k, v) { this.append(k, v); return this; }
    }
    const mp = new MyParams("q=1");
    eq(mp.add("z", "9").get("z"), "9", "URLSearchParams subclass methods chain");
    eq(mp.toString(), "q=1&z=9", "and the state is the real query");
    eq(mp.size, 2, "size through the subclass");
}

if (fails) {
    print("test_url_host: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_url_host failed");
}
print("test_url_host: " + n + " assertions, 0 failures");