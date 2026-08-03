/* test_api_params.js -- PARAMETRIC value-level cases across every dyna module.
 *
 * One table per module, one runner. Each row is [name, thunk, expected]: the
 * thunk is called and its result compared to `expected` structurally, so a row
 * pins a VALUE rather than "it did not throw". A row whose expected value is
 * simply "no exception" belongs in the pen-test suites, not here.
 *
 * Use `THROWS` as the expected value to require a throw, and `ANY` where the
 * value is legitimately environment-dependent but must exist.
 */
import * as std from "std";

const THROWS = Symbol("throws");
const ANY = Symbol("any");

let pass = 0, fail = 0, skip = 0;
const fails = [];

function eqv(a, b) {
    if (b === ANY) return a !== undefined && a !== null;
    if (Object.is(a, b)) return true;
    if (typeof a === "number" && typeof b === "number")
        return Math.abs(a - b) < 1e-9;
    if (a && b && typeof a === "object" && typeof b === "object") {
        const A = Array.isArray(a) ? a : Object.keys(a).sort().map((k) => [k, a[k]]);
        const B = Array.isArray(b) ? b : Object.keys(b).sort().map((k) => [k, b[k]]);
        if (A.length !== B.length) return false;
        for (let i = 0; i < A.length; i++) if (!eqv(A[i], B[i])) return false;
        return true;
    }
    return false;
}

const show = (v) => {
    try { return typeof v === "string" ? JSON.stringify(v) : String(v); }
    catch (e) { return "<unprintable>"; }
};

function run(mod, rows) {
    print("\n-- " + mod + " (" + rows.length + " cases) --");
    for (const [name, thunk, want] of rows) {
        let got, threw = null;
        try { got = thunk(); } catch (e) { threw = e; }
        if (want === THROWS) {
            if (threw) { pass++; }
            else { fail++; fails.push(`${mod}: ${name} -- expected a throw, got ${show(got)}`); }
            continue;
        }
        if (threw) {
            fail++; fails.push(`${mod}: ${name} -- threw ${threw.name}: ${threw.message}`);
            continue;
        }
        if (eqv(got, want)) pass++;
        else { fail++; fails.push(`${mod}: ${name} -- got ${show(got)} want ${show(want)}`); }
    }
}

async function mod(n) { try { return await import("dyna:" + n); } catch (e) { return null; } }
function section(name, m, rowsFn) {
    if (!m) { skip++; print("\n-- " + name + " -- SKIP (not in this build)"); return; }
    run(name, rowsFn(m));
}

/* ==================================================================== hash */
section("hash", await mod("hash"), (h) => [
    ["SHA256Hex('abc')", () => h.SHA256Hex("abc"),
     "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"],
    ["SHA256Hex('')", () => h.SHA256Hex(""),
     "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"],
    ["SHA1Hex('abc')", () => h.SHA1Hex("abc"), "a9993e364706816aba3e25717850c26c9cd0d89d"],
    ["MD5Hex('abc')", () => h.MD5Hex("abc"), "900150983cd24fb0d6963f7d28e17f72"],
    ["SHA512Hex('') prefix", () => h.SHA512Hex("").slice(0, 16), "cf83e1357eefb8bd"],
    ["SHA3_256Hex('abc') prefix", () => h.SHA3_256Hex("abc").slice(0, 16), "3a985da74fe225b2"],
    ["CRC32('abc')", () => h.CRC32("abc") >>> 0, 891568578],
    ["CRC32('')", () => h.CRC32("") >>> 0, 0],
    ["BLAKE3Hex('abc') is 64 hex", () => h.BLAKE3Hex("abc").length, 64],
    ["Hasher matches one-shot", () => {
        const x = new h.Hasher("sha256"); x.update("ab"); x.update("c");
        return x.digestHex() === h.SHA256Hex("abc");
    }, true],
    ["unknown algorithm", () => new h.Hasher("nope-xyz"), THROWS],
]);

/* ================================================================ encoding */
section("encoding", await mod("encoding"), (e) => [
    ["HexEncode('abc')", () => e.HexEncode("abc"), "616263"],
    ["Base64Encode('hello')", () => e.Base64Encode("hello"), "aGVsbG8="],
    ["Base64URLEncode('hello') unpadded", () => e.Base64URLEncode("hello"), "aGVsbG8"],
    ["Base64 round trip", () => {
        const d = e.Base64Decode(e.Base64Encode("hello world"));
        return new TextDecoder().decode(d);
    }, "hello world"],
    ["Base32 round trip", () => {
        const d = e.Base32Decode(e.Base32Encode("hi there"));
        return new TextDecoder().decode(d);
    }, "hi there"],
    ["Base58 round trip", () => {
        const d = e.Base58Decode(e.Base58Encode("satoshi"));
        return new TextDecoder().decode(d);
    }, "satoshi"],
    ["HexDecode odd length", () => e.HexDecode("abc"), THROWS],
    ["Base58Decode rejects '0'", () => e.Base58Decode("0OIl"), THROWS],
    ["JSON5Parse trailing comma", () => e.JSON5Parse('{a:1,}').a, 1],
    ["JSON5Parse comment", () => e.JSON5Parse('{/*c*/ "a":2}').a, 2],
    ["StableStringify key order", () => e.StableStringify({ b: 1, a: 2 }), '{"a":2,"b":1}'],
    ["StableStringify is stable", () =>
        e.StableStringify({ a: 1, b: 2 }) === e.StableStringify({ b: 2, a: 1 }), true],
]);

/* ================================================================== semver */
section("semver", await mod("semver"), (s) => [
    ["compare 1.0.0 < 2.0.0", () => s.compare("1.0.0", "2.0.0"), -1],
    ["compare equal", () => s.compare("1.2.3", "1.2.3"), 0],
    ["compare 1.10.0 > 1.9.0", () => s.compare("1.10.0", "1.9.0"), 1],
    ["prerelease sorts before release", () => s.compare("1.0.0-alpha", "1.0.0"), -1],
    ["major/minor/patch", () => [s.major("4.5.6"), s.minor("4.5.6"), s.patch("4.5.6")], [4, 5, 6]],
    ["isValid good", () => s.isValid("1.2.3"), true],
    ["isValid bad", () => s.isValid("not-a-version"), false],
    ["satisfies caret", () => s.satisfies("1.2.5", "^1.2.0"), true],
    ["satisfies caret excludes major bump", () => s.satisfies("2.0.0", "^1.2.0"), false],
    ["satisfies tilde", () => s.satisfies("1.2.9", "~1.2.0"), true],
    ["inc patch", () => s.inc("1.2.3", "patch"), "1.2.4"],
    ["inc minor resets patch", () => s.inc("1.2.3", "minor"), "1.3.0"],
    ["inc major resets both", () => s.inc("1.2.3", "major"), "2.0.0"],
    ["sort", () => s.sort(["1.10.0", "1.2.0", "1.9.0"]).join(","), "1.2.0,1.9.0,1.10.0"],
    ["gt/lt/eq", () => [s.gt("2.0.0", "1.0.0"), s.lt("1.0.0", "2.0.0"), s.eq("1.0.0", "1.0.0")],
     [true, true, true]],
]);

/* ===================================================================== url */
section("url", await mod("url"), (u) => [
    ["hostname", () => new u.URL("https://example.com/a/b?x=1#f").hostname, "example.com"],
    ["pathname", () => new u.URL("https://example.com/a/b?x=1#f").pathname, "/a/b"],
    ["protocol", () => new u.URL("https://example.com/").protocol, "https:"],
    ["backslash does not change the host", () =>
        new u.URL("https://example.com\\@evil.com/").hostname, "example.com"],
    ["userinfo does not become the host", () =>
        new u.URL("https://user@real.example/").hostname, "real.example"],
    ["dot segments collapse", () => new u.URL("https://h/a/../b").pathname, "/b"],
    ["formDecode", () => u.formDecode("a=1&b=2").b, "2"],
    ["formEncode round trip", () => u.formDecode(u.formEncode({ k: "a b" })).k, "a b"],
    ["percent decoding in a query", () => u.formDecode("k=a%20b").k, "a b"],
]);

/* ================================================================= decimal */
section("decimal", await mod("decimal"), (d) => [
    ["exact addition", () => new d.Decimal("0.1").add(new d.Decimal("0.2")).toString(), "0.3"],
    ["float would not be exact", () => 0.1 + 0.2 === 0.3, false],
    ["multiplication", () => new d.Decimal("1.5").mul(new d.Decimal("4")).toString(), "6"],
    ["subtraction", () => new d.Decimal("1").sub(new d.Decimal("0.9")).toString(), "0.1"],
    ["comparison", () => new d.Decimal("2.50").equals(new d.Decimal("2.5")), true],
    ["absurd exponent is refused", () => new d.Decimal("1e999999999").toString(), THROWS],
    ["two dots", () => new d.Decimal("1.2.3"), THROWS],
]);

/* =================================================================== mathx */
section("mathx", await mod("mathx"), (m) => [
    ["gcd", () => Number(m.gcd(48, 36)), 12],
    ["erf(0)", () => m.erf(0), 0],
    ["erf(large) -> 1", () => Math.round(m.erf(5)), 1],
    ["bits.onesCount32(255)", () => m.bits.onesCount32(255), 8],
    ["bits.leadingZeros32(1)", () => m.bits.leadingZeros32(1), 31],
    ["bits.reverse32 is an involution", () => m.bits.reverse32(m.bits.reverse32(12345)), 12345],
    ["Pi", () => Math.abs(m.Pi - Math.PI) < 1e-12, true],
]);

/* ==================================================================== time */
section("time", await mod("time"), (t) => [
    /* parseRFC3339/now return {sec, nsec}, not a scalar. */
    ["parseRFC3339 epoch", () => t.parseRFC3339("1970-01-01T00:00:00Z").sec, 0],
    ["formatRFC3339 round trip", () =>
        t.formatRFC3339(t.parseRFC3339("2020-06-15T12:30:45Z").sec).slice(0, 19),
     "2020-06-15T12:30:45"],
    ["parseDuration 1s in ns", () => t.parseDuration("1s"), 1000000000],
    ["parseDuration 1m", () => t.parseDuration("1m"), 60000000000],
    ["parseDuration garbage", () => t.parseDuration("nonsense"), THROWS],
    ["Duration toString", () => String(new t.Duration({ minutes: 90 })), "PT1H30M"],
    ["now is positive", () => t.now().sec > 0, true],
]);

/* ================================================================ compress */
section("compress", await mod("compress"), (z) => [
    ["gzip round trip", () => {
        const x = "abc".repeat(1000);
        return new TextDecoder().decode(z.gunzip(z.gzip(x))) === x;
    }, true],
    ["gzip compresses repetitive input", () => z.gzip("a".repeat(10000)).length < 200, true],
    ["gunzip of garbage", () => z.gunzip(new Uint8Array([1, 2, 3])), THROWS],
    ["lz4 round trip", () => {
        const x = "xyz".repeat(2000);
        return new TextDecoder().decode(z.lz4Unframe(z.lz4Frame(x))) === x;
    }, true],
]);

/* ================================================================== crypto */
section("crypto", await mod("crypto"), (c) => [
    ["HMACHex known vector", () => c.HMACHex("sha256", "key", "The quick brown fox jumps over the lazy dog"),
     "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8"],
    ["TimingSafeEqual same", () => c.TimingSafeEqual("abc", "abc"), true],
    ["TimingSafeEqual differ", () => c.TimingSafeEqual("abc", "abd"), false],
    ["RandomBytes length", () => c.RandomBytes(16).length, 16],
    ["RandomBytes differs", () => c.HexEncode === undefined ?
        String(c.RandomBytes(8)) !== String(c.RandomBytes(8)) : true, true],
    ["JWT round trip", () => {
        const tok = c.JWTSign({ sub: "a" }, "k", { alg: "HS256" });
        return c.JWTVerify(tok, "k", { algorithms: ["HS256"] }).sub;
    }, "a"],
    ["JWT wrong key", () => {
        const tok = c.JWTSign({ sub: "a" }, "k", { alg: "HS256" });
        return c.JWTVerify(tok, "wrong", { algorithms: ["HS256"] });
    }, THROWS],
    ["JWT requires an algorithms allowlist", () => {
        const tok = c.JWTSign({ sub: "a" }, "k", { alg: "HS256" });
        return c.JWTVerify(tok, "k");
    }, THROWS],
    ["PBKDF2 is deterministic", () =>
        String(c.PBKDF2({ password: "p", salt: "s", iterations: 1000, keyLength: 16 })) ===
        String(c.PBKDF2({ password: "p", salt: "s", iterations: 1000, keyLength: 16 })), true],
]);

/* ==================================================================== uuid */
section("uuid", await mod("uuid"), (u) => [
    ["v4 validates", () => u.validate(u.v4()), true],
    ["v4 version", () => u.version(u.v4()), 4],
    ["v7 validates", () => u.validate(u.v7()), true],
    /* RFC 9562 s5.7: rand_a MAY carry a sub-millisecond counter, so two v7s in
       the SAME millisecond are legitimately unordered. The guarantee is the
       48-bit big-endian timestamp prefix, which is what this pins. */
    ["v7 timestamp prefix is non-decreasing", () => {
        const pre = (x) => x.slice(0, 8) + x.slice(9, 13);
        const a = pre(u.v7()); let n = 0;
        for (let i = 0; i < 500; i++) if (pre(u.v7()) < a) n++;
        return n === 0;
    }, true],
    ["NIL", () => u.NIL, "00000000-0000-0000-0000-000000000000"],
    ["validate rejects garbage", () => u.validate("not-a-uuid"), false],
    ["parse garbage", () => u.parse("zz"), THROWS],
    ["v5 is deterministic", () => u.v5(u.NAMESPACE_DNS, "x") === u.v5(u.NAMESPACE_DNS, "x"), true],
]);

/* ================================================================ matcher */
section("matcher", await mod("matcher"), (m) => [
    ["Levenshtein kitten/sitting", () => m.Levenshtein("kitten", "sitting"), 3],
    ["Levenshtein identical", () => m.Levenshtein("abc", "abc"), 0],
    ["Levenshtein empty", () => m.Levenshtein("", "abc"), 3],
    ["Levenshtein refuses 1e10 cells", () =>
        m.Levenshtein("a".repeat(100000), "b".repeat(100000)), THROWS],
    ["Levenshtein with max answers", () =>
        m.Levenshtein("a".repeat(100000), "b".repeat(100000), { max: 5 }) > 0, true],
    ["Matcher finds", () => new m.Matcher("ss").test("mississippi"), true],
    ["Matcher misses", () => new m.Matcher("zz").test("mississippi"), false],
    ["MultiMatcher counts", () => new m.MultiMatcher(["a", "b"]).countIn("aabb"), 4],
    ["DiceCoefficient identical", () => m.DiceCoefficient("night", "night"), 1],
]);

/* ============================================================== structures */
section("structures", await mod("structures"), (s) => [
    ["Trie insert/has", () => { const t = new s.Trie(); t.insert("ab", 1); return t.has("ab"); }, true],
    ["Trie prefix", () => {
        const t = new s.Trie(); t.insert("cat", 1); t.insert("car", 2);
        return t.keysWithPrefix("ca").sort().join(",");
    }, "car,cat"],
    ["Trie miss", () => { const t = new s.Trie(); return t.has("nope"); }, false],
    ["BloomFilter no false negative", () => {
        const b = new s.BloomFilter(1000, 0.01); b.add("x"); return b.mayContain("x");
    }, true],
    ["LRU evicts", () => {
        const l = new s.LRU(2); l.set("a", 1); l.set("b", 2); l.set("c", 3);
        return l.has("a");
    }, false],
    ["LRU keeps recent", () => {
        const l = new s.LRU(2); l.set("a", 1); l.set("b", 2); return l.get("b");
    }, 2],
    ["BitSet set/get", () => { const b = new s.BitSet(64); b.set(7); return b.get(7); }, true],
    ["Deque order", () => {
        const d = new s.Deque(); d.pushBack(1); d.pushBack(2); return d.popFront();
    }, 1],
]);

/* ================================================================ validate */
section("validate", await mod("validate"), (v) => [
    ["IsEmail good", () => v.IsEmail("a@b.co"), true],
    ["IsEmail bad", () => v.IsEmail("not-an-email"), false],
    ["IsAlpha", () => v.IsAlpha("abc"), true],
    ["IsAlpha with digit", () => v.IsAlpha("ab1"), false],
    ["IsAlphanumeric", () => v.IsAlphanumeric("ab1"), true],
    ["IsAscii", () => v.IsAscii("abc"), true],
    ["IsAscii unicode", () => v.IsAscii("abç"), false],
    ["IsCreditCard valid luhn", () => v.IsCreditCard("4242424242424242"), true],
    ["IsCreditCard invalid", () => v.IsCreditCard("1234567812345678"), false],
]);

/* =================================================================== bytes */
section("bytes", await mod("bytes"), (b) => [
    ["encode/decode round trip", () => {
        const u8 = b.encode("héllo", "utf-8"); return b.decode(u8, "utf-8");
    }, "héllo"],
    ["equal", () => b.equal(b.encode("a", "utf-8"), b.encode("a", "utf-8")), true],
    ["compare orders", () => b.compare(b.encode("a", "utf-8"), b.encode("b", "utf-8")) < 0, true],
    ["indexOf", () => b.indexOf(b.encode("hello", "utf-8"), b.encode("ll", "utf-8")), 2],
    /* countUtf8 counts CODE POINTS, not bytes: "héllo" is 6 bytes, 5 points. */
    ["countUtf8", () => b.countUtf8("héllo"), 5],
    ["isValidUtf8", () => b.isValidUtf8(b.encode("ok", "utf-8")), true],
]);

/* ==================================================================== yaml */
section("yaml", await mod("yaml"), (y) => [
    ["scalar map", () => y.Parse("a: 1\nb: two\n").b, "two"],
    ["nested", () => y.Parse("a:\n  b: 2\n").a.b, 2],
    ["list", () => y.Parse("- 1\n- 2\n").length, 2],
    ["round trip", () => y.Parse(y.Stringify({ k: "v" })).k, "v"],
]);

/* ===================================================================== xml */
section("xml", await mod("xml"), (x) => [
    /* XMLToObject takes a parsed node, not source: XMLParse first. */
    ["element text", () => x.XMLToObject(x.XMLParse("<r><a>1</a></r>")).r.a, "1"],
    ["attributes", () => {
        const o = x.XMLToObject(x.XMLParse('<r a="1"/>'));
        return JSON.stringify(o).indexOf("1") >= 0;
    }, true],
    ["malformed", () => x.XMLParse("<unclosed>"), THROWS],
]);

/* =============================================================== serialize */
section("serialize", await mod("serialize"), (s) => [
    ["CBOR round trip", () => s.CBORDecode(s.CBOREncode({ a: 1, b: "x" })).b, "x"],
    ["MsgPack round trip", () => s.MsgPackDecode(s.MsgPackEncode([1, 2, 3])).length, 3],
    ["CBOR declared 4GB is refused", () =>
        s.CBORDecode(new Uint8Array([0x5a, 0xff, 0xff, 0xff, 0xff])), THROWS],
    ["structuredClone deep", () => s.structuredClone({ a: { b: [1, 2] } }).a.b[1], 2],
    ["structuredClone is a copy", () => {
        const src = { a: [1] }; const c = s.structuredClone(src); c.a.push(2);
        return src.a.length;
    }, 1],
]);

/* ================================================================== random */
section("random", await mod("random"), (r) => [
    ["seeded is reproducible", () =>
        new r.Random(7).nextU53() === new r.Random(7).nextU53(), true],
    ["different seeds differ", () =>
        new r.Random(1).nextU53() !== new r.Random(2).nextU53(), true],
    ["nextBounded in range", () => {
        const g = new r.Random(3);
        for (let i = 0; i < 200; i++) { const v = g.nextBounded(10); if (v < 0 || v >= 10) return false; }
        return true;
    }, true],
    ["nextFloat in [0,1)", () => {
        const g = new r.Random(4);
        for (let i = 0; i < 200; i++) { const v = g.nextFloat(); if (v < 0 || v >= 1) return false; }
        return true;
    }, true],
]);

/* =================================================================== simd */
section("simd", await mod("simd"), (s) => [
    ["sum", () => s.sum(new Float32Array([1, 2, 3, 4])), 10],
    ["max", () => s.max(new Float32Array([1, 9, 3])), 9],
    ["min", () => s.min(new Float32Array([4, 2, 8])), 2],
    ["argmax attains the max", () => {
        const a = new Float32Array([1, 5, 5, 2]); return a[s.argmax(a)];
    }, 5],
    ["dot", () => s.dot(new Float32Array([1, 2]), new Float32Array([3, 4])), 11],
    ["max of a single element", () => s.max(new Float32Array([42])), 42],
    ["sum of empty", () => s.sum(new Float32Array(0)), 0],
]);

/* =============================================================== dataframe */
section("dataframe", await mod("dataframe"), (d) => [
    ["ROWS", () => new d.DataFrame({ a: new Float64Array([1, 2, 3]) }).ROWS, 3],
    ["SUM", () => new d.DataFrame({ a: new Float64Array([1, 2, 3]) }).SUM("a"), 6],
    ["MEAN", () => new d.DataFrame({ a: new Float64Array([2, 4]) }).MEAN("a"), 3],
    ["MIN/MAX", () => {
        const f = new d.DataFrame({ a: new Float64Array([5, 1, 9]) });
        return [f.MIN("a"), f.MAX("a")];
    }, [1, 9]],
    ["GROUP_BY_SUM", () => {
        const f = new d.DataFrame({ k: ["x", "y", "x"], v: new Float64Array([1, 2, 3]) });
        const g = f.GROUP_BY_SUM("k", "v");
        return [g.keys.join(","), Array.from(g.values).join(",")];
    }, ["x,y", "4,2"]],
    ["MEDIAN", () => new d.DataFrame({ a: new Float64Array([1, 2, 3]) }).MEDIAN("a"), 2],
]);

/* ==================================================================== html */
section("html", await mod("html"), (h) => [
    ["sanitizer drops script", () => {
        const s = new h.Sanitizer({ allow: { p: [] } });
        return s.clean("<p>ok</p><script>bad()</script>");
    }, "<p>ok</p>"],
    ["sanitizer drops javascript: url", () => {
        const s = new h.Sanitizer({ allow: { a: ["href"] }, protocols: { "a.href": ["https"] } });
        return s.clean('<a href="javascript:x()">t</a>');
    }, "<a>t</a>"],
    ["sanitizer keeps allowed", () => {
        const s = new h.Sanitizer({ allow: { b: [] } });
        return s.clean("<b>hi</b>");
    }, "<b>hi</b>"],
]);

/* ===================================================================== cli */
section("cli", await mod("cli"), (c) => [
    ["StyleText returns a string", () => typeof c.StyleText("red", "x"), "string"],
    ["StyleText contains the text", () => c.StyleText("red", "hello").indexOf("hello") >= 0, true],
    ["Columns is positive", () => c.Columns() > 0, true],
]);

/* ===================================================================== log */
section("log", await mod("log"), (l) => [
    ["Logger constructs", () => typeof new l.Logger({ level: "error" }), "object"],
    ["Logger has levels", () => {
        const g = new l.Logger({ level: "error" });
        return ["info", "warn", "error", "debug"].every((k) => typeof g[k] === "function");
    }, true],
]);

/* ==================================================================== sys */
section("sys", await mod("sys"), (s) => [
    ["platform is a string", () => typeof s.platform(), "string"],
    ["pid is positive", () => s.pid() > 0, true],
    ["cwd is absolute", () => String(s.cwd()).startsWith("/"), true],
    ["env round trip", () => { s.setEnv("DYNA_T_X", "v"); return s.getEnv("DYNA_T_X"); }, "v"],
    ["env NUL name is refused", () => s.setEnv("A\u0000B", "v"), THROWS],
]);

/* ==================================================================== file */
section("file", await mod("file"), (f) => [
    ["Path basename", () => new f.Path("/a/b/c.txt").basename, "c.txt"],
    ["Path dirname", () => String(new f.Path("/a/b/c.txt").dirname), "/a/b"],
    ["Path extname", () => new f.Path("/a/b/c.txt").extname, ".txt"],
    ["Path normalises", () => String(new f.Path("/srv//a/../b")), "/srv/b"],
    ["Path join", () => String(new f.Path("/a").join("b")), "/a/b"],
    ["Path requires a Path, not a string", () => f.readFile("/etc/hosts"), THROWS],
    ["NUL in a path is refused", () => new f.Path("/tmp/a\u0000b"), THROWS],
    ["write/read round trip", () => {
        const d = f.makeTempDir("apiparam-");
        const p = d.join("x.txt");
        f.writeFile(p, "body");
        const got = f.readFile(p);
        f.removeAll(d);
        return typeof got === "string" ? got : new TextDecoder().decode(got);
    }, "body"],
]);

/* ==================================================================== csv */
section("csv", await mod("csv"), (c) => [
    ["CSVFile requires a Path", () => new c.CSVFile("/tmp/x.csv"), THROWS],
]);

/* ================================================================= config */
section("config", await mod("config"), (c) => [
    ["INI parses a section", () => c.INI.parse("[s]\nk = v\n").s.k, "v"],
    ["INI empty", () => typeof c.INI.parse(""), "object"],
]);

/* =================================================================== ml */
section("ml", await mod("ml"), (m) => [
    ["LinearRegression learns y = 2x", () => {
        const r = new m.LinearRegression({});
        r.fit([[1], [2], [3], [4]], [2, 4, 6, 8]);
        return Math.abs(r.predict([[5]])[0] - 10) < 0.5;
    }, true],
    ["predict before fit", () => new m.LinearRegression({}).predict([[1]]), THROWS],
    ["StandardScaler centres", () => {
        const s = new m.StandardScaler();
        s.fit([[1], [2], [3]]);
        return Math.abs(s.transform([[2]])[0][0]) < 1e-6;
    }, true],
]);

/* ================================================================= scrape */
section("scrape", await mod("scrape"), (s) => [
    ["Robots allows by default", () => new s.Robots("").allows("/x", "bot"), true],
    ["Robots disallows", () =>
        new s.Robots("User-agent: *\nDisallow: /p\n").allows("/p", "bot"), false],
]);

/* ==================================================================== net */
section("net", await mod("net"), (n) => [
    ["parseAddr v4", () => n.parseAddr("127.0.0.1").string, "127.0.0.1"],
    ["isLoopback", () => n.isLoopback("127.0.0.1"), true],
    ["isPrivate 10/8", () => n.isPrivate("10.0.0.1"), true],
    ["isPrivate public", () => n.isPrivate("8.8.8.8"), false],
    ["isValid good", () => n.isValid("1.2.3.4"), true],
    ["isValid bad", () => n.isValid("999.1.1.1"), false],
    ["NUL-truncated address is not valid", () => n.isValid("127.0.0.1\u0000evil"), false],
    ["CookieParse", () => n.CookieParse("a=1; b=2").b, "2"],
    ["ContentTypeParse", () => { const r = n.ContentTypeParse("text/html; charset=utf-8");
        return r.type + "/" + r.subtype; }, "text/html"],
]);

/* =================================================================== done */
print("\n" + "=".repeat(64));
if (fails.length) {
    print("FAILURES (" + fails.length + "):");
    for (const f of fails) print("  " + f);
}
print(`test_api_params: ${pass} passed, ${fail} failed, ${skip} modules skipped`);
if (fail > 0) std.exit(1);
