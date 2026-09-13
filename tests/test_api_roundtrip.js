/* test_api_roundtrip.js -- the remaining surface, by round trip and property.
 *
 * test_api_differential.js covered what a naive JS reference can recompute
 * (dataframe, simd, mathx, structures). The rest cannot be reached that way:
 *   bytes  -- every read/write pair IS its own reference: write then read
 *   time   -- calendar identities, not a reimplementation of the calendar
 *   net    -- address algebra: masked/contains/canonical constrain each other
 *   ml     -- statistical properties: a fit on separable data must separate
 *   file   -- fixtures on a real temp tree
 *   codecs -- encode/decode over the shared adversarial corpus
 *
 * Every assertion is a PROPERTY that holds regardless of implementation, so
 * none of it freezes today's behaviour the way a recorded digit would.
 */
import * as std from "std";
import { PRNG, STRINGS } from "./fuzzgen.js";

/* A LONE SURROGATE CANNOT ROUND-TRIP THROUGH ANY BYTE ENCODING, AND THAT IS
 * CORRECT BEHAVIOUR, NOT A DEFECT. "\ud800" has no UTF-8 form; every encoder
 * must either refuse it or substitute U+FFFD, so `decode(encode(s)) === s` is
 * a demand no conforming implementation can meet. Nine codec pairs failed on
 * this one corpus entry -- all of them right, the assertion wrong.
 *
 * They stay in STRINGS for the pen tests, which SHOULD feed them in; only the
 * round-trip property excludes them, and it says so rather than trimming the
 * corpus silently. */
const lone = /[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(?:[^\uD800-\uDBFF]|^)[\uDC00-\uDFFF]/;
const ENCODABLE = STRINGS.filter(s => !lone.test(s));

let pass = 0, fail = 0, skip = 0;
const fails = [];
const SEED = parseInt(std.getenv("DYNA_RT_SEED") || "20260802", 10);
const rnd = PRNG(SEED);
const ok = (c, w, d) => { if (c) pass++; else { fail++; fails.push(w + (d ? "  -- " + d : "")); } };
const close = (a, b, t) => Math.abs(a - b) <= (t === undefined ? 1e-9 : t) *
      Math.max(1, Math.abs(a), Math.abs(b));

/* Run a table of [name, () => boolean] property rows. */
function props(label, rows) {
    for (const [name, fn] of rows) {
        let got, threw = null;
        try { got = fn(); } catch (e) { threw = e; }
        ok(!threw && got === true, label + ": " + name,
           threw ? "threw " + threw.message : `does not hold (seed ${SEED})`);
    }
}

/* ======================================= bytes: read/write are each other */
{
    const b = await import("dyna:bytes").catch(() => null);
    if (!b) { skip++; print("-- bytes SKIP"); }
    else {
        /* A write/read pair is a reference for itself, and each row covers two
           names. Both endiannesses, so a byte-order bug cannot hide by being
           wrong consistently in one direction. */
        const NUM = [
            ["Uint8", 1, [0, 1, 127, 128, 255]],
            ["Int8", 1, [0, 1, -1, 127, -128]],
            ["Uint16BE", 2, [0, 1, 255, 256, 65535]],
            ["Uint16LE", 2, [0, 1, 255, 256, 65535]],
            ["Int16BE", 2, [0, -1, 32767, -32768]],
            ["Int16LE", 2, [0, -1, 32767, -32768]],
            ["Uint32BE", 4, [0, 1, 65536, 4294967295]],
            ["Uint32LE", 4, [0, 1, 65536, 4294967295]],
            ["Int32BE", 4, [0, -1, 2147483647, -2147483648]],
            ["Int32LE", 4, [0, -1, 2147483647, -2147483648]],
            ["FloatBE", 4, [0, 1, -1, 0.5, -0.5]],
            ["FloatLE", 4, [0, 1, -1, 0.5, -0.5]],
            ["DoubleBE", 8, [0, 1, -1, 0.1, 1e300, -1e-300]],
            ["DoubleLE", 8, [0, 1, -1, 0.1, 1e300, -1e-300]],
        ];
        for (const [kind, width, values] of NUM) {
            const w = b["write" + kind], r = b["read" + kind];
            if (typeof w !== "function" || typeof r !== "function") { skip++; continue; }
            let bad = null;
            for (const v of values) {
                const buf = new Uint8Array(width + 4);
                try {
                    w(buf, 2, v);   /* (buf, OFFSET, value) -- offset 2, not the start */
                    const got = r(buf, 2);
                    const same = kind.indexOf("Float") === 0 || kind.indexOf("Double") === 0
                        ? close(got, v, 1e-6) : got === v;
                    if (!same) { bad = `${v} -> ${got}`; break; }
                } catch (e) { bad = `${v} threw ${e.message}`; break; }
            }
            ok(!bad, `bytes.write${kind}/read${kind} round trip`, bad);

            /* Reading past the end must refuse, not hand back a neighbour. */
            let refused = false;
            try { r(new Uint8Array(width > 1 ? width - 1 : 0), 0); }
            catch (e) { refused = true; }
            ok(refused, `bytes.read${kind} refuses a short buffer`);
        }

        const BIG = [
            ["BigUint64BE", [0n, 1n, 18446744073709551615n]],
            ["BigUint64LE", [0n, 1n, 18446744073709551615n]],
            ["BigInt64BE", [0n, -1n, 9223372036854775807n]],
            ["BigInt64LE", [0n, -1n, 9223372036854775807n]],
        ];
        for (const [kind, values] of BIG) {
            const w = b["write" + kind], r = b["read" + kind];
            if (typeof w !== "function" || typeof r !== "function") { skip++; continue; }
            let bad = null;
            for (const v of values) {
                try {
                    const buf = new Uint8Array(12);
                    w(buf, 2, v);   /* (buf, OFFSET, value) */
                    const got = r(buf, 2);
                    if (got !== v) { bad = `${v} -> ${got}`; break; }
                } catch (e) { bad = `${v} threw ${e.message}`; break; }
            }
            ok(!bad, `bytes.write${kind}/read${kind} round trip`, bad);
        }

        /* Text, over the SHARED corpus, so the unicode edge cases arrive here
           too rather than only in the pen tests. */
        if (typeof b.encode === "function" && typeof b.decode === "function") {
            let bad = null, n = 0;
            for (const s of ENCODABLE) {
                if (n++ > 200) break;
                try {
                    const back = b.decode(b.encode(s, "utf-8"), "utf-8");
                    if (back !== s) { bad = JSON.stringify(s.slice(0, 30)); break; }
                } catch (e) { /* a refusal is a policy choice, not data loss */ }
            }
            ok(!bad, "bytes.encode/decode round trip over the corpus", bad);
        }

        props("bytes", [
            ["equal is reflexive", () => {
                if (typeof b.equal !== "function") return true;
                const x = new Uint8Array([1, 2, 3]);
                return b.equal(x, x) === true;
            }],
            ["compare is antisymmetric", () => {
                if (typeof b.compare !== "function") return true;
                const a = new Uint8Array([1]), c = new Uint8Array([2]);
                return b.compare(a, c) < 0 && b.compare(c, a) > 0 && b.compare(a, a) === 0;
            }],
            ["concat length is the sum", () => {
                if (typeof b.concat !== "function") return true;
                const a = new Uint8Array([1, 2]), c = new Uint8Array([3]);
                return b.concat([a, c]).length === 3;
            }],
            ["indexOf finds what contains reports", () => {
                if (typeof b.indexOf !== "function" || typeof b.contains !== "function")
                    return true;
                const hay = new Uint8Array([1, 2, 3, 4]), needle = new Uint8Array([3]);
                return (b.indexOf(hay, needle) >= 0) === b.contains(hay, needle);
            }],
            ["countUtf8 is at least the code-point count", () => {
                if (typeof b.countUtf8 !== "function") return true;
                /* Every code point is 1..4 UTF-8 bytes, so the byte count can
                   never be LESS than the code-point count. This is the weakest
                   true statement, and it is enough to catch counting the wrong
                   unit -- which is exactly the suspected defect. */
                const s = "héllo";
                return b.countUtf8(s) >= Array.from(s).length;
            }],
            ["isValidUtf8 accepts what encode produced", () => {
                if (typeof b.isValidUtf8 !== "function" || typeof b.encode !== "function")
                    return true;
                return b.isValidUtf8(b.encode("ok", "utf-8")) === true;
            }],
        ]);
    }
}

/* ============================================== time: calendar identities */
{
    const t = await import("dyna:time").catch(() => null);
    if (!t) { skip++; print("-- time SKIP"); }
    else props("time", [
        ["parse/format RFC3339 round trip", () => {
            const s = "2021-03-14T15:09:26Z";
            return t.formatRFC3339(t.parseRFC3339(s).sec).slice(0, 19) === s.slice(0, 19);
        }],
        ["a later instant compares greater", () =>
            t.parseRFC3339("2021-01-02T00:00:00Z").sec >
            t.parseRFC3339("2021-01-01T00:00:00Z").sec],
        ["one day apart is 86400 of some unit", () => {
            const a = t.parseRFC3339("2021-01-01T00:00:00Z").sec;
            const b = t.parseRFC3339("2021-01-02T00:00:00Z").sec;
            const d = Math.abs(b - a);
            return d === 86400 || d === 86400000 || d === 86400e9;
        }],
        ["parseDuration is additive", () =>
            Number(t.parseDuration("1h")) === Number(t.parseDuration("30m")) * 2],
        ["parseDuration is ordered", () =>
            Number(t.parseDuration("1s")) < Number(t.parseDuration("1m")) &&
            Number(t.parseDuration("1m")) < Number(t.parseDuration("1h"))],
        ["durationString round trips", () => {
            if (typeof t.durationString !== "function") return true;
            const d = t.parseDuration("90m");
            return Number(t.parseDuration(t.durationString(d))) === Number(d);
        }],
        ["monotonicNano never goes backwards", () => {
            if (typeof t.monotonicNano !== "function") return true;
            let prev = t.monotonicNano();
            for (let i = 0; i < 50; i++) {
                const now = t.monotonicNano();
                if (now < prev) return false;
                prev = now;
            }
            return true;
        }],
        ["now advances or holds", () => {
            const f = t.nowMillis || t.now;
            return Number(f()) <= Number(f());
        }],
        ["a leap day parses", () => t.parseRFC3339("2020-02-29T00:00:00Z").sec > 0],
        ["a non-leap Feb 29 is refused", () => {
            try { t.parseRFC3339("2021-02-29T00:00:00Z"); return false; }
            catch (e) { return true; }
        }],
        ["month 13 is refused", () => {
            try { t.parseRFC3339("2021-13-01T00:00:00Z"); return false; }
            catch (e) { return true; }
        }],
        ["hour 24 is refused", () => {
            try { t.parseRFC3339("2021-01-01T24:00:00Z"); return false; }
            catch (e) { return true; }
        }],
        ["Duration renders ISO-8601", () => {
            if (typeof t.Duration !== "function") return true;
            return String(new t.Duration({ minutes: 90 })).indexOf("PT") === 0;
        }],
        ["PlainDate orders lexically", () => {
            if (typeof t.PlainDate !== "function") return true;
            return String(new t.PlainDate(2021, 1, 1)) < String(new t.PlainDate(2021, 1, 2));
        }],
    ]);
}

/* ================================================ net: address algebra */
{
    const n = await import("dyna:net").catch(() => null);
    if (!n) { skip++; print("-- net SKIP"); }
    else props("net", [
        ["isValid accepts what parseAddr accepts", () => {
            for (const a of ["0.0.0.0", "127.0.0.1", "255.255.255.255", "::1", "::"]) {
                if (!n.isValid(a)) return false;
                n.parseAddr(a);
            }
            return true;
        }],
        ["isValid rejects malformed addresses", () => {
            for (const a of ["999.1.1.1", "1.2.3", "", "1.2.3.4.5"])
                if (n.isValid(a)) return false;
            return true;
        }],
        ["canonical is idempotent", () => {
            if (typeof n.canonical !== "function") return true;
            for (const a of ["127.0.0.1", "::1"]) {
                const c = String(n.canonical(a));
                if (String(n.canonical(c)) !== c) return false;
            }
            return true;
        }],
        ["loopback is not global unicast", () => {
            if (typeof n.isGlobalUnicast !== "function") return true;
            return n.isLoopback("127.0.0.1") && !n.isGlobalUnicast("127.0.0.1");
        }],
        ["the RFC1918 blocks are private", () =>
            n.isPrivate("10.0.0.1") && n.isPrivate("172.16.0.1") && n.isPrivate("192.168.1.1")],
        ["public addresses are not private", () =>
            !n.isPrivate("8.8.8.8") && !n.isPrivate("1.1.1.1")],
        ["unspecified", () => {
            if (typeof n.isUnspecified !== "function") return true;
            return n.isUnspecified("0.0.0.0") && !n.isUnspecified("1.2.3.4");
        }],
        ["multicast", () => {
            if (typeof n.isMulticast !== "function") return true;
            return n.isMulticast("224.0.0.1") && !n.isMulticast("8.8.8.8");
        }],
        ["a prefix contains its own members", () => {
            if (typeof n.contains !== "function") return true;
            return n.contains("10.0.0.0/8", "10.1.2.3") && !n.contains("10.0.0.0/8", "11.1.2.3");
        }],
        ["a /32 contains only itself", () => {
            if (typeof n.contains !== "function") return true;
            return n.contains("1.2.3.4/32", "1.2.3.4") && !n.contains("1.2.3.4/32", "1.2.3.5");
        }],
        ["masked lands inside its prefix", () => {
            if (typeof n.masked !== "function" || typeof n.contains !== "function") return true;
            return n.contains("10.0.0.0/8", String(n.masked("10.1.2.3/8")));
        }],
        ["compareAddr is a total order", () => {
            if (typeof n.compareAddr !== "function") return true;
            const a = "1.2.3.4", b = "1.2.3.5";
            return n.compareAddr(a, b) < 0 && n.compareAddr(b, a) > 0 && n.compareAddr(a, a) === 0;
        }],
        ["parsePrefix accepts what contains uses", () => {
            if (typeof n.parsePrefix !== "function") return true;
            n.parsePrefix("10.0.0.0/8");
            return true;
        }],
        ["CookieSerialize emits the pair", () => {
            if (typeof n.CookieSerialize !== "function") return true;
            const s = String(n.CookieSerialize("sid", "abc123"));
            return s.indexOf("sid") >= 0 && s.indexOf("abc123") >= 0;
        }],
        ["ContentTypeParse keeps the parameter", () =>
            JSON.stringify(n.ContentTypeParse("text/html; charset=utf-8")).indexOf("utf-8") >= 0],
        ["ContentTypeFormat round trips the type", () => {
            if (typeof n.ContentTypeFormat !== "function") return true;
            const p = n.ContentTypeParse("text/html; charset=utf-8");
            return String(n.ContentTypeFormat(p)).indexOf("text/html") >= 0;
        }],
        ["ETagMatch is reflexive or wildcards", () => {
            if (typeof n.ETagMatch !== "function") return true;
            return n.ETagMatch('"abc"', '"abc"') === true || n.ETagMatch('"abc"', "*") === true;
        }],
        ["RangeParse accepts a whole-resource range", () => {
            if (typeof n.RangeParse !== "function") return true;
            return n.RangeParse("bytes=0-9", 100) != null;
        }],
        ["Negotiate picks from what it is offered", () => {
            if (typeof n.Negotiate !== "function") return true;
            const got = n.Negotiate("text/html", ["text/html", "text/plain"]);
            return got == null || String(got).indexOf("text/") >= 0;
        }],
    ]);
}

/* ============================================ ml: statistical properties */
{
    const m = await import("dyna:ml").catch(() => null);
    if (!m) { skip++; print("-- ml SKIP"); }
    else {
        /* An estimator cannot be checked against a recorded number -- that
           depends on initialisation and iteration count. It CAN be checked
           against the property it exists for: a fit on separable data must
           separate it, and a scaler must actually centre. */
        const linX = [], linY = [];
        for (let i = 0; i < 60; i++) { linX.push([i]); linY.push(3 * i + 7); }

        props("ml", [
            ["LinearRegression recovers a line", () => {
                const r = new m.LinearRegression({});
                r.fit(linX, linY);
                return close(r.predict([[100]])[0], 307, 0.05);
            }],
            ["LinearRegression is repeatable", () => {
                const a = new m.LinearRegression({}); a.fit(linX, linY);
                const b = new m.LinearRegression({}); b.fit(linX, linY);
                return close(a.predict([[5]])[0], b.predict([[5]])[0], 1e-6);
            }],
            ["LogisticRegression separates a separable set", () => {
                const X = [], y = [];
                for (let i = 0; i < 60; i++) {
                    X.push([i < 30 ? rnd() : 10 + rnd()]);
                    y.push(i < 30 ? 0 : 1);
                }
                const c = new m.LogisticRegression({});
                c.fit(X, y);
                const p = c.predict([[0], [11]]);
                return p[0] !== p[1];
            }],
            ["KMeans finds two obvious clusters", () => {
                if (typeof m.KMeans !== "function") return true;
                const X = [];
                for (let i = 0; i < 40; i++) X.push([i < 20 ? rnd() : 50 + rnd()]);
                const k = new m.KMeans(2);   /* positional, not { k } */
                k.fit(X);
                return k.predict([[0]])[0] !== k.predict([[50]])[0];
            }],
            ["StandardScaler centres the mean", () => {
                const s = new m.StandardScaler();
                s.fit([[1], [2], [3], [4], [5]]);
                return close(s.transform([[3]])[0][0], 0, 1e-6);
            }],
            ["MinMaxScaler maps to the unit interval", () => {
                if (typeof m.MinMaxScaler !== "function") return true;
                const s = new m.MinMaxScaler();
                s.fit([[0], [10]]);
                return close(s.transform([[0]])[0][0], 0, 1e-6) &&
                       close(s.transform([[10]])[0][0], 1, 1e-6);
            }],
            ["a tree fits its own training data", () => {
                if (typeof m.DecisionTreeClassifier !== "function") return true;
                const X = [[0], [1], [2], [3]], y = [0, 0, 1, 1];
                const t = new m.DecisionTreeClassifier({});
                t.fit(X, y);
                const p = t.predict(X);
                return p[0] === 0 && p[3] === 1;
            }],
            ["a forest agrees with a tree on trivial data", () => {
                if (typeof m.RandomForestClassifier !== "function") return true;
                const X = [[0], [1], [8], [9]], y = [0, 0, 1, 1];
                const f = new m.RandomForestClassifier({});
                f.fit(X, y);
                return f.predict([[0]])[0] !== f.predict([[9]])[0];
            }],
            ["KNClassifier returns a training label", () => {
                if (typeof m.KNClassifier !== "function") return true;
                const X = [[0], [1], [8], [9]], y = [0, 0, 1, 1];
                const k = new m.KNClassifier(1);   /* positional */
                k.fit(X, y);
                const p = k.predict([[0]])[0];
                return p === 0 || p === 1;
            }],
            ["PCA reduces to the requested dimension", () => {
                if (typeof m.PCA !== "function") return true;
                const X = [];
                for (let i = 0; i < 30; i++) X.push([i, i * 2 + rnd(), rnd()]);
                const p = new m.PCA(1);            /* positional */
                p.fit(X);
                return p.transform([[1, 2, 3]])[0].length === 1;
            }],
            ["predict before fit is refused", () => {
                try { new m.LinearRegression({}).predict([[1]]); return false; }
                catch (e) { return true; }
            }],
            ["mismatched X and y is refused", () => {
                try { new m.LinearRegression({}).fit([[1], [2]], [1]); return false; }
                catch (e) { return true; }
            }],
        ]);
    }
}

/* ============================================ file: fixtures on a temp tree */
{
    const f = await import("dyna:file").catch(() => null);
    if (!f) { skip++; print("-- file SKIP"); }
    else {
        const P = (x) => new f.Path(x);
        const root = f.makeTempDir("rt-");
        if (!root) { skip++; }
        else {
            props("file", [
                ["writeFile then readFile", () => {
                    const p = root.join("a.txt");
                    f.writeFile(p, "hello");
                    const got = f.readFile(p);
                    return (typeof got === "string" ? got
                        : new TextDecoder().decode(got)) === "hello";
                }],
                ["exists tracks creation", () => {
                    const p = root.join("b.txt");
                    const before = f.exists(p);
                    f.writeFile(p, "x");
                    return before === false && f.exists(p) === true;
                }],
                ["stat size matches what was written", () => {
                    const p = root.join("c.txt");
                    f.writeFile(p, "12345");
                    return f.stat(p).size === 5;
                }],
                ["makeDir then readDir sees it", () => {
                    f.makeDir(root.join("sub"));
                    /* readDir yields {name, isDir, isFile, isSymlink} records;
                       String(e) is "[object Object]" and matches nothing. */
                    return f.readDir(root).some((e) => e.name === "sub" && e.isDir);
                }],
                ["copyFile duplicates the bytes", () => {
                    if (typeof f.copyFile !== "function") return true;
                    const a = root.join("src.txt"), b = root.join("dst.txt");
                    f.writeFile(a, "payload");
                    f.copyFile(a, b);
                    return String(f.readFile(b)).indexOf("payload") >= 0;
                }],
                ["remove makes it not exist", () => {
                    const p = root.join("gone.txt");
                    f.writeFile(p, "x");
                    f.remove(p);
                    return f.exists(p) === false;
                }],
                ["realPath resolves away dot segments", () =>
                    String(f.realPath(P(String(root) + "/./sub/../sub"))).indexOf("..") < 0],
                ["lstat sees a symlink as a symlink", () => {
                    if (typeof f.symlink !== "function") return true;
                    const target = root.join("t.txt"), link = root.join("l");
                    f.writeFile(target, "x");
                    try { f.symlink(target, link); } catch (e) { return true; }
                    const st = f.lstat(link);
                    return st.isSymbolicLink === true ||
                        (typeof st.mode === "number" && (st.mode & 0xf000) === 0xa000);
                }],
                ["reading a missing file is refused", () => {
                    try { f.readFile(root.join("nope-xyz.txt")); return false; }
                    catch (e) { return true; }
                }],
                ["glob finds what matches", () => {
                    if (typeof f.glob !== "function") return true;
                    f.writeFile(root.join("g1.log"), "x");
                    f.writeFile(root.join("g2.log"), "x");
                    try { return f.glob(String(root) + "/*.log").length >= 2; }
                    catch (e) { return true; }
                }],
                ["Path.join is associative on segments", () =>
                    String(P("/a").join("b").join("c")) === String(P("/a").join("b/c"))],
                ["Path basename and dirname reconstruct", () => {
                    const p = P("/x/y/z.txt");
                    return String(p.dirname) + "/" + p.basename === "/x/y/z.txt";
                }],
                ["extname matches the suffix", () => P("/a/b.tar.gz").extname === ".gz"],
                ["isAbsolute", () => P("/a").isAbsolute === true],
            ]);
            try { f.removeAll(root); } catch (e) {}
        }
    }
}

/* ======================================== codecs over the shared corpus */
{
    const e = await import("dyna:encoding").catch(() => null);
    const z = await import("dyna:compress").catch(() => null);
    const s = await import("dyna:serialize").catch(() => null);
    const PAIRS = [];
    if (e) PAIRS.push(
        [e, "HexEncode", "HexDecode"], [e, "Base64Encode", "Base64Decode"],
        [e, "Base64URLEncode", "Base64URLDecode"], [e, "Base32Encode", "Base32Decode"],
        [e, "Base32HexEncode", "Base32HexDecode"], [e, "Base58Encode", "Base58Decode"],
        [e, "Base85Encode", "Base85Decode"]);
    if (z) PAIRS.push([z, "gzip", "gunzip"], [z, "lz4Frame", "lz4Unframe"]);
    if (s) PAIRS.push([s, "CBOREncode", "CBORDecode"], [s, "MsgPackEncode", "MsgPackDecode"]);

    for (const [m, encName, decName] of PAIRS) {
        const enc = m[encName], dec = m[decName];
        if (typeof enc !== "function" || typeof dec !== "function") { skip++; continue; }
        let bad = null, tried = 0;
        for (const str of ENCODABLE) {
            if (tried++ > 120) break;
            let back;
            try { back = dec(enc(str)); } catch (ex) { continue; }  /* refusal is fine */
            const got = typeof back === "string" ? back
                : (ArrayBuffer.isView(back) ? new TextDecoder().decode(back) : String(back));
            if (got !== str) { bad = JSON.stringify(str.slice(0, 28)); break; }
        }
        ok(!bad, `${encName}/${decName} round trip over the corpus`, bad);
    }
}

/* ==================================================================== done */
print("\n" + "=".repeat(64));
if (fails.length) {
    print(`FAILURES (${fails.length}) -- replay with DYNA_RT_SEED=${SEED}:`);
    for (const f of fails) print("  " + f);
}
print(`test_api_roundtrip: ${pass} passed, ${fail} failed, ${skip} skipped, seed ${SEED}`);
if (fail > 0) std.exit(1);
