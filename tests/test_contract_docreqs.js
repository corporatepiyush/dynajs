// flags: --std
// timeout: 180
// Contract-clause tests for the WS-CONTRACT documentation pass.
// Two halves: (1) the new dynajs.d.ts clause sentences must be present in the
// spec text; (2) the behavior each clause promises must hold at runtime.
// Every assertion names the DOC-REQ / finding it locks.
import "./httpc.js";
import * as crypto from "dyna:crypto";
import { JWTSign } from "dyna:crypto";
import * as oauth2 from "dyna:oauth2";
import { nchoosek, besselk, besselkScaled, besselh, stats, cumsum } from "dyna:mathx";
import { DiffChars } from "dyna:matcher";
import { Random } from "dyna:random";
import { gzip, inflate, unzstd } from "dyna:compress";
import { domainToASCII, punycodeDecode } from "dyna:url";
import { Template } from "dyna:html";
import { ASN1 } from "dyna:serialize";
import { DataFrame } from "dyna:dataframe";
import { IsAscii } from "dyna:validate";
import { parseTime, PlainTime, Duration, Format, date } from "dyna:time";
import { Path, writeFile, symlink, readFile, remove } from "dyna:file";
import { Command } from "dyna:cli";
import * as sys from "dyna:sys";
import { TCPServer, Metrics } from "dyna:net";
import { HTTPClient } from "dyna:http";
import { Parse as yamlParse } from "dyna:yaml";
import { TOML, Env } from "dyna:config";
import { XMLParse } from "dyna:xml";
import { PCA } from "dyna:ml";
import { Graph, Fenwick } from "dyna:structures";
import { Sitemap, Fetcher } from "dyna:scrape";
import { CRC32C } from "dyna:hash";
import * as simd from "dyna:simd";
import { sprintf } from "std";
import { Bytes } from "dyna:bytes";

let n = 0, failed = 0;
function ok(c, msg) { n++; if (!c) { failed++; console.log("  FAIL " + msg); } }
function eq(a, b, msg) { n++; if (!Object.is(a, b)) { failed++; console.log("  FAIL " + msg + " — got |" + a + "| want |" + b + "|"); } }
function throws(fn, msg, ctor, pat) {
    n++;
    let e = null;
    try { fn(); } catch (err) { e = err; }
    if (!e) { failed++; console.log("  FAIL " + msg + " — did not throw"); return; }
    if (ctor && !(e instanceof ctor)) { failed++; console.log("  FAIL " + msg + " — wrong type " + e.constructor.name + ": " + e); return; }
    if (pat && !pat.test(String(e))) { failed++; console.log("  FAIL " + msg + " — wrong message |" + e + "|"); return; }
}

// ---- (1) spec-text clauses -------------------------------------------------
let dtsPath = Path.cwd().join("dynajs.d.ts");
try { readFile(dtsPath); } catch (e) { dtsPath = Path.cwd().join("../dynajs.d.ts"); }
const dts = readFile(dtsPath);
const CLAUSES = [
    ["SEC-007", "salt` is REQUIRED"],
    ["SEC-006", "1048576 (1 GiB)"],
    ["SEC-011", "at most 20"],
    ["SEC-092", "CONTEXTO"],
    ["SEC-093", "4096 mapped code points"],
    ["P6/SEC-005", "1 GiB across"],
    ["P5", "16777216 elements"],
    ["P1", "Infinity promptly"],
    ["COMPAT-235", 'IsAscii("") is false'],
    ["COMPAT-067", "dense-window encoded"],
    ["D11/SEC-151", "TRUSTED input"],
    ["COMPAT-101", "UTF-8 labels are accepted"],
    ["COMPAT-100", "Known omissions"],
    ["COMPAT-093", "FRACTION in (0,1)"],
    ["SEC-250", "NOT synchronized with worker"],
    ["COMPAT-182", "atomic whole-file replacements"],
    // ---- wave-3 clauses -----------------------------------------------------
    ["COMPAT-176", "Ties resolve to the"],
    ["COMPAT-179", "tolerance of 1e-3"],
    ["COMPAT-040", "all-NaN"],
    ["SEC-013", "1000 child"],
    ["SEC-013", "50000 URLs"],
    ["SEC-063", "256 series"],
    ["SEC-066", "PostgreSQL's `ca`"],
    ["SEC-073", "<pem-redacted>"],
    ["OPT-046", "must fit int32"],
    ["SEC-047", "64x the record's payload"],
    ["COMPAT-107", "no null literal"],
    ["COMPAT-106", "run of six or more"],
    ["COMPAT-108", "previous `[[x]]` header"],
    ["COMPAT-109/110", "expand in file order"],
    ["COMPAT-230/231/232", "At most one DOCTYPE"],
    ["COMPAT-246", "regardless of the host locale"],
    ["COMPAT-241", "MSVC is not a verified target"],
    ["SEC-246/247", "|value| <= 1e12"],
    ["SEC-247", "|day| <= 1e9"],
    ["OPT-053", "60 * nFeatures^3"],
    ["OPT-055", "push budget of 64*n+1024"],
    ["OPT-056", "2^28 day/position steps"],
    ["OPT-057/058", "accounted like every other native class"],
    ["SEC-195", "resolves to the same file"],
    ["COMPAT-112", "already free"],
    ["SEC-107", "inherited the pipe"],
    // ---- iter1 folded clauses (DOC-REQS-A2 / DOC-REQS-A4) --------------------
    ["SEC-047", "512 MiB + 64x the record's payload"],
    ["SEC-061", "maxOutputBytes"],
    ["SEC-052", "keys are 1..4096 bytes without `=` or NUL"],
    ["SEC-062", "non-matching declared Content-Type is 415"],
    ["SEC-079", "no DTD is processed"],
    ["SEC-080", "capped at 256 MiB (RangeError), the XMLParse limit"],
    ["COMPAT-068", "unsigned value 0..4294967295"],
    ["COMPAT-038", "deduplicated: N identical hits at one position emit one, at the first index"],
    ["COMPAT-009", "required for TLS endpoints"],
    ["SEC-068", "TypeError on shape/length mismatch, RangeError otherwise"],
    ["SEC-059", "re-parsing the output introduces no new structure"],
    // ---- A3 corrections ------------------------------------------------------
    ["COMPAT-001", "negative or Infinity n is TypeError"],
    ["SEC-137", "key exactly 16/24/32 bytes (else TypeError)"],
    ["SEC-137", "key exactly 32 bytes, nonce exactly 12 bytes (else TypeError)"],
    ["COMPAT-139", "above 65536 is clamped"],
];
for (const [id, phrase] of CLAUSES)
    ok(dts.indexOf(phrase) >= 0, "[" + id + "] dynajs.d.ts states: " + phrase);

// README.md carries the clauses with no JS surface (SEC-208 / SEC-193 C API).
let apiPath = Path.cwd().join("README.md");
try { readFile(apiPath); } catch (e) { apiPath = Path.cwd().join("../README.md"); }
const api = readFile(apiPath);
const API_CLAUSES = [
    ["SEC-208", "duplicate prefix"],
    ["SEC-193", "ENOTSUP"],
    ["SEC-193", "DYN_SLURP_MMAP"],
    ["COMPAT-042", "scalar-only"],
];
for (const [id, phrase] of API_CLAUSES)
    ok(api.indexOf(phrase) >= 0, "[" + id + "] README.md states: " + phrase);

// ---- (2) behavior ----------------------------------------------------------
// crypto: PBKDF2 salt requirement + defaults (SEC-007)
throws(() => crypto.PBKDF2({ password: "x", iterations: 1000 }), "PBKDF2 without salt is a TypeError (SEC-007)", TypeError);
eq(crypto.PBKDF2({ password: "x", salt: "s" }).length, 32, "PBKDF2 default length 32 (SEC-007)");
{
    const def = crypto.PBKDF2({ password: "x", salt: "s" });
    const explicit = crypto.PBKDF2({ password: "x", salt: "s", hash: "sha256", iterations: 100000, length: 32 });
    eq(def.length, explicit.length, "PBKDF2 documented defaults match explicit sha256/100000/32 (SEC-007)");
    let same = def.length === explicit.length;
    for (let i = 0; same && i < def.length; i++) same = def[i] === explicit[i];
    ok(same, "PBKDF2 defaults are byte-identical to explicit sha256/100000/32 (SEC-007)");
}
eq(crypto.HKDF({ key: "k" }).length, 32, "HKDF salt stays optional, default length 32 (SEC-007)");
throws(() => crypto.PBKDF2({ password: "x", salt: "s", iterations: 1 << 24, length: 64 }),
       "PBKDF2 iterations x blocks cap is enforced (SEC-007)", RangeError, /16777216/);

// crypto: Argon2id verify-side PHC cap (SEC-006 / N1)
throws(() => crypto.Argon2id.verify("$argon2id$v=19$m=1048577,t=1,p=1$c2FsdHNhbHQ$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", "pw"),
       "Argon2id.verify refuses PHC memory above 1 GiB (SEC-006)", RangeError, /1 GiB/);
throws(() => crypto.Argon2id.verify("$argon2id$v=19$m=4194304,t=16,p=16$c2FsdHNhbHQ$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", "pw"),
       "the old 4 GiB verify workload is refused too (SEC-006)", RangeError, /1 GiB/);

// crypto: TOTP window cap (SEC-233)
throws(() => crypto.TOTPVerify("AAAA", "000000", { window: 4097 }), "TOTPVerify window > 4096 is RangeError (SEC-233)", RangeError, /4096/);
eq(crypto.TOTPVerify("AAAA", "000000", { window: 4096 }), false, "TOTPVerify window 4096 is accepted (SEC-233)");

// crypto: bcrypt refusal + rounds (SEC-053)
throws(() => crypto.Bcrypt.hash("a".repeat(100), 4), "Bcrypt.hash refuses >72-byte password (SEC-053)", RangeError, /72/);
throws(() => crypto.Bcrypt.hash("x", 3), "Bcrypt.hash rounds below 4 is RangeError (SEC-053)", RangeError, /4\.\.31/);

// crypto: X25519 small-order refusal (SEC-054, TLS builds)
if (typeof crypto.X25519Derive === "function") {
    throws(() => crypto.X25519Derive(new Uint8Array(32), new Uint8Array(32)),
           "X25519Derive refuses a small-order peer (SEC-054)", TypeError, /small-order/);
} else {
    console.log("  SKIP(no CONFIG_TLS): X25519Derive small-order");
}

// crypto JWT alg enforcement (SEC-055) — TLS builds only
if (typeof crypto.JWTVerify === "function") {
    const tok = crypto.JWTSign({ sub: "u" }, "k", { alg: "HS256" });
    eq(crypto.JWTVerify(tok, "k", { algorithms: ["HS256"] }).sub, "u", "JWTVerify allowlist happy path (SEC-055)");
    throws(() => crypto.JWTVerify(tok, "k", { algorithms: ["none"] }), "alg not in allowlist is refused (SEC-055)");
} else {
    console.log("  SKIP(no CONFIG_TLS): JWTVerify allowlist");
}

// mathx guards (SEC-024/025, P1-P5)
{
    const t0 = Date.now();
    eq(nchoosek(1e18, 5e17), Infinity, "nchoosek huge order returns Infinity (SEC-024)");
    ok(Date.now() - t0 < 2000, "nchoosek huge order is prompt (SEC-024)");
    eq(besselk(1e9, 0.1), Infinity, "besselk huge order answers +Infinity (SEC-025)");
    eq(besselkScaled(3e9, 0.1), Infinity, "besselkScaled huge order answers +Infinity (SEC-025)");
    const h = besselh(2 ** 25, 1, 1);
    ok(Array.isArray(h) && h.length === 2, "besselh huge order answers an underflow pair promptly (P4)");
    throws(() => stats.sum({ length: 1 << 25 }), "stats array-like length cap (SEC-096)", RangeError, /16777216/);
    throws(() => cumsum({ length: 1 << 25 }), "cumsum array-like length cap (SEC-096)", RangeError, /16777216/);
    eq(stats.sum(new Float64Array([1, 2, 3])), 6, "Float64Array stats fast path intact (SEC-096)");
}

// random nextBounded BigInt semantics (P8)
throws(() => new Random(1).nextBounded(0n), "nextBounded(0n) is RangeError (P8)", RangeError);
throws(() => new Random(1).nextBounded(2.5), "fractional Number bound is RangeError (P8)", RangeError);
{
    const a = new Random(7).nextBounded(-5n);
    const b = new Random(7).nextBounded(2n ** 64n - 5n);
    eq(a, b, "a negative BigInt bound aliases modulo 2^64 (P8)");
}

// compress: gzip level paths + unzstd/inflate refusals (P7, SEC-050)
{
    const data = "abcabcabc ".repeat(2000);
    ok(gzip(data, 6).length < gzip(data, 1).length, "gzip level >= 6 selects the dynamic-Huffman path (P7)");
    throws(() => unzstd(new Uint8Array([1, 2, 3])), "unzstd refuses malformed input (SEC-050)", TypeError);
    throws(() => inflate(new Uint8Array([1, 2, 3])), "inflate refuses malformed input (SEC-050)");
}

// cli/platform guards (SEC-095, COMPAT-056)
{
    throws(() => sprintf("%2000000000d", 1), "sprintf width cap is RangeError (SEC-095)", RangeError);
    eq(sprintf("%5c", 65), "    A", "%c honours width (SEC-095)");
    ok(typeof (await import("dyna:cli")).Columns() === "number", "Columns() answers a number (COMPAT-056)");
}

// diff budget (COMPAT-020): wide dissimilar inputs now diff instead of refusing
{
    const a = "a".repeat(30000), b = "b".repeat(30000);
    const t0 = Date.now();
    const d = DiffChars(a, b);
    ok(Array.isArray(d) && d.length > 0, "DiffChars handles 30k/30k dissimilar inputs (COMPAT-020)");
    ok(Date.now() - t0 < 30000, "DiffChars result is prompt (COMPAT-020)");
}

// file/platform refusals (P15)
{
    const p = Path.temp().join("wsc-docreq-" + Date.now());
    throws(() => writeFile(p, undefined), "writeFile(undefined) is TypeError (COMPAT-184)", TypeError);
    throws(() => symlink("a\u0000b", p), "symlink NUL target is TypeError (COMPAT-196)", TypeError);
    throws(() => new Command("x").parse(["a\u0000b"]), "Command.parse NUL argv is TypeError (COMPAT-193)", TypeError);
    throws(() => new Path("a\u0000b"), "Path segment NUL is TypeError (COMPAT-182 family)", TypeError);
}

// Bytes.alloc coercion, documented as-is (COMPAT-001 correction)
{
    eq(Bytes.alloc(3.7).length, 3, "Bytes.alloc truncates fractions toward zero (COMPAT-001)");
    eq(Bytes.alloc("x").length, 0, "Bytes.alloc of a NaN-coercing value is 0 (COMPAT-001)");
    eq(Bytes.alloc(true).length, 1, "Bytes.alloc(true) is 1 (COMPAT-001)");
    throws(() => Bytes.alloc(-1), "Bytes.alloc(-1) is TypeError (COMPAT-001)", TypeError, /requires a length/);
    throws(() => Bytes.alloc(Infinity), "Bytes.alloc(Infinity) is TypeError (COMPAT-001)", TypeError, /requires a length/);
    throws(() => Bytes.alloc(2 ** 31), "Bytes.alloc(2^31) is RangeError (COMPAT-001)", RangeError, /length too large/);
}

// AEAD key/nonce length refusals are TypeErrors, as documented (SEC-137 correction)
{
    throws(() => new crypto.AESGCM(new Uint8Array(15)),
           "AESGCM bad key length is TypeError (SEC-137)", TypeError, /16, 24 or 32/);
    const gcm = new crypto.AESGCM(new Uint8Array(32));
    throws(() => gcm.seal(new Uint8Array(11), "x"),
           "AESGCM bad nonce length is TypeError (SEC-137)", TypeError, /nonce must be 12/);
    throws(() => new crypto.ChaCha20Poly1305(new Uint8Array(31)),
           "ChaCha20Poly1305 bad key length is TypeError (SEC-137)", TypeError, /exactly 32/);
}

// Number.pad/hex place clamp, documented as-is (COMPAT-139 correction)
{
    eq((7).pad(70000).length, 65536, "pad clamps place to 65536 (COMPAT-139)");
    eq((1234567).pad(70000).length, 65536, "pad(70000) output is at most 65536 chars (COMPAT-139)");
    eq((1234567).pad(1), "1234567", "pad never truncates the number's own digits (COMPAT-139)");
    eq((255).hex(70000).length, 65536, "hex clamps place to 65536 (COMPAT-139)");
}

// array/string engine DOC-REQs (COMPAT-029/131/132/134/135/136/137)
{
    const r = [1, 2, 3, 4, 5].removeRange(1, 2);
    eq(JSON.stringify(r), "[1,4,5]", "removeRange second argument is a COUNT (COMPAT-029)");
    eq([1, 2].aperture(0).length, 3, "aperture(0) yields len+1 empty windows (COMPAT-131)");
    throws(() => [1, 2].aperture(-1), "aperture(-1) is RangeError (COMPAT-131)", RangeError);
    eq([10, 2, 1].sortBy((v) => v).join(","), "1,2,10", "sortBy numeric keys sort numerically (COMPAT-132)");
    eq(JSON.stringify(["b", "a"].sortBy((v) => v)), '["a","b"]', "sortBy string keys sort by byte order (COMPAT-132)");
    eq("Ä".equalsIgnoreCase("ä"), false, "equalsIgnoreCase is ASCII-only (COMPAT-136)");
    eq("A".equalsIgnoreCase("a"), true, "equalsIgnoreCase folds ASCII (COMPAT-136)");
    eq("0x10".toNumber(), 16, "String.toNumber uses C syntax (COMPAT-135)");
    eq(" 12x".toNumber(), 12, "String.toNumber strtod semantics (COMPAT-135)");
    eq("é".localeCompare("e\u0301"), 0, "localeCompare compares NFC-normalized code points (COMPAT-134)");
    const ta = new Uint8Array([1, 2, 3]);
    const sep = { toString() { ta.buffer.transfer(0); return ","; } };
    eq(ta.join(sep).split(",").length, 3, "typed-array join captures length before ToString (COMPAT-137)");
}

// IDNA (SEC-092/093, COMPAT-036/037, COMPAT-159)
throws(() => domainToASCII("a\u00B7b"), "CONTEXTO middle dot outside l·l is refused (SEC-092)", TypeError);
throws(() => domainToASCII("\u0375a.gr"), "CONTEXTO Greek keraia refused (SEC-092)", TypeError);
throws(() => domainToASCII("a\u30FBb"), "CONTEXTO katakana middle dot without kana/Han refused (SEC-092)", TypeError);
eq(domainToASCII("l\u00B7l"), "xn--ll-0ea", "CONTEXTO l·l is allowed (SEC-092)");
throws(() => domainToASCII("a".repeat(4097) + ".com"), "label over 4096 mapped code points refused (SEC-093)", TypeError);
eq(domainToASCII("example.com."), "example.com.", "trailing-dot FQDN accepted (COMPAT-036)");
eq(domainToASCII("\u1E9E.com"), "xn--zca.com", "U+1E9E maps non-transitionally to U+00DF (COMPAT-037)");
throws(() => punycodeDecode("a".repeat(1100)), "punycodeDecode 1024-octet cap (COMPAT-159)", RangeError, /1024/);

// oauth2 strict runtime types + requireExp (SEC-126/127, SEC-016, SEC-226)
{
    const tok = JWTSign({ sub: "u" }, "secret", { alg: "HS256" });
    throws(() => oauth2.verifyJWT(tok, "secret", { algorithms: ["HS256"], iss: 5 }),
           "verifyJWT iss non-string is TypeError (SEC-126)", TypeError, /iss/);
    throws(() => oauth2.verifyJWT(tok, "secret", { algorithms: ["HS256"], requireExp: true }),
           "requireExp rejects a token without exp (SEC-226)", TypeError, /requireExp|exp/);
    const tokExp = JWTSign({ sub: "u", exp: Math.floor(Date.now() / 1000) + 100 }, "secret", { alg: "HS256" });
    eq(oauth2.verifyJWT(tokExp, "secret", { algorithms: ["HS256"], requireExp: true }).sub, "u",
       "requireExp accepts a numeric exp (SEC-226)");
    throws(() => oauth2.generateCodeChallenge("a".repeat(43), "S512"),
           "unknown PKCE method is TypeError (SEC-078)", TypeError, /S256 or plain/);
}

// html template budget (SEC-120)
{
    eq(new Template("{{x}}").render({ x: "<b>" }), "&lt;b&gt;", "Template escapes by default (SEC-120 family)");
    const arr = [];
    arr.length = 1000000000;
    throws(() => new Template("{{#a}}x{{/a}}").render({ a: arr }),
           "Template render refuses a hostile sparse section (SEC-120)", RangeError, /budget/i);
}

// ASN.1 untrusted decode (SEC-057)
throws(() => ASN1.decode(new Uint8Array([0x30, 0xff, 0xff, 0xff, 0x7f])),
       "ASN1.decode refuses an oversized length-of-length (SEC-057)", SyntaxError);

// sys spawn backpressure cap (P9)
{
    const p = new sys.Spawn("sleep", ["3"], { stdin: "pipe", maxPipe: 4096 });
    let refused = null;
    try { await p.stdin.write(new Uint8Array(8192)); } catch (e) { refused = e; }
    ok(refused instanceof RangeError, "SpawnStdin write past maxPipe rejects RangeError (SEC-105/P9)");
    p.close();
}

// dataframe group-by key window + sample head (COMPAT-067/069)
{
    const d = new DataFrame({ k: Int32Array.from([0, 1048577]), v: Float64Array.from([1, 2]) });
    throws(() => d.GROUP_BY_SUM("k", "v"), "integer group key >= 2^20 is RangeError (COMPAT-067)", RangeError, /too many groups/);
    const d2 = new DataFrame({ k: Int32Array.from([0, 0, 0]), v: Float64Array.from([3, 2, 1]) });
    const g = d2.GROUP_ARRAY_SAMPLE("k", "v", 2);
    eq(JSON.stringify(Array.from(g.values[0])), "[3,2]", "GROUP_ARRAY_SAMPLE is the first-k head (COMPAT-069)");
}

// validate / time / csv / protobuf / yaml (data DOC-REQs)
eq(IsAscii(""), false, "IsAscii(\"\") is false (COMPAT-235)");
throws(() => parseTime("12:00:60"), "parseTime rejects leap second 60 (COMPAT-261)", RangeError);
eq(new PlainTime(1, 2, 3).add(new Duration({ days: 1 })).toString(), "01:02:03",
   "PlainTime.add ignores days (COMPAT-259)");
throws(() => new PlainTime(1, 2).add(new Duration({ months: 1 })),
       "PlainTime.add refuses months (COMPAT-259)", RangeError);
{
    const df = new DataFrame({ a: ["=1+1"] });
    ok(df.TO_CSV().indexOf("=1+1") >= 0, "TO_CSV emits formula-leading cells verbatim by default (SEC-234)");
    ok(df.TO_CSV({ escapeFormulas: true }).indexOf("'=1+1") >= 0, "TO_CSV escapeFormulas is the opt-in (SEC-234)");
    const msg = new Uint8Array([0x08, 0x01, 0x10, 0x02]); // field 1 = 1, field 2 = 2
    const schema = { fields: [
        { name: "a", number: 1, type: "int32", oneof: "x" },
        { name: "b", number: 2, type: "int32", oneof: "x" },
    ] };
    const dec = await import("dyna:serialize");
    const got = dec.Proto.decode(msg, schema);
    ok(got.a === 1 && got.b === 2, "protobuf oneof is an annotation; both fields decode (COMPAT-237)");
}
{
    const nested = yamlParse("{a:[1,2]}");
    ok(nested.a && nested.a.length === 2,
        "a nested flow collection after ':' parses (COMPAT-228 repaired)");
    const keyed = yamlParse("{a:b}");
    ok(keyed["a:b"] === null,
        "a flow colon without a boundary stays in a null-valued key (PyYAML parity)");
    throws(() => yamlParse("{a}"), "a flow key with no colon at all is refused", SyntaxError);
}

// HTTP: onConnect scope + fetch redirect policy (SEC-035/COMPAT-047/SEC-011)
async function httpTests() {
    let requests = 0;
    const loop = new TCPServer({ port: 0 });
    loop.start({
        data(conn) {
            requests++;
            conn.write("HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        },
    });
    const loopUrl = "http://127.0.0.1:" + loop.port + "/loop";
    let redirectErr = null;
    try { await fetch(loopUrl); } catch (e) { redirectErr = e; }
    ok(redirectErr !== null && /too many redirects/i.test(String(redirectErr)),
       "fetch refuses after the 20-hop redirect cap (SEC-011)");
    ok(requests >= 21, "the cap counted >= 21 requests (got " + requests + ")");
    loop.close();

    const scheme = new TCPServer({ port: 0 });
    scheme.start({
        data(conn) {
            conn.write("HTTP/1.1 302 Found\r\nLocation: file:///etc/passwd\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        },
    });
    let schemeErr = null;
    try { await fetch("http://127.0.0.1:" + scheme.port + "/x"); } catch (e) { schemeErr = e; }
    ok(schemeErr !== null, "fetch refuses a non-http(s) redirect target (SEC-011)");
    scheme.close();

    const ok200 = new TCPServer({ port: 0 });
    ok200.start({
        data(conn) {
            conn.write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nhi");
        },
    });
    const url200 = "http://127.0.0.1:" + ok200.port + "/ok";
    {
        const c = new HTTPClient();
        let saw = "";
        c.onConnect = (ip) => { saw = ip; return false; };
        let refused = false;
        try { await c.getAsync(url200); } catch (e) { refused = /connect/i.test(String(e)); }
        ok(refused, "HTTPClient.getAsync honors onConnect=false (SEC-035)");
        eq(saw, "127.0.0.1", "getAsync hook receives the resolved IP (SEC-035)");
        c.close();
    }
    {
        const c = new HTTPClient();
        let saw = "";
        c.onConnect = (ip) => { saw = ip; return true; };
        const r = await c.getAsync(url200);
        ok(r.status === 200 && saw === "127.0.0.1", "getAsync proceeds when the hook allows (SEC-035)");
        c.close();
    }
    {
        let saw = "";
        let refused = false;
        try { await fetch(url200, { onConnect: (ip) => { saw = ip; return false; } }); }
        catch (e) { refused = /connect/i.test(String(e)); }
        ok(refused && saw === "127.0.0.1", "fetch honors RequestInit.onConnect (SEC-035/COMPAT-047)");
    }
    ok200.close();
}

// ---- wave-3 behavior clauses ------------------------------------------------
function wave3Clauses() {
    // COMPAT-176/041/040: SIMD tie-break, domain and NaN semantics.
    {
        eq(simd.argmax(Float32Array.from([1, 1, 9, 1, 9, 1, 1, 1])), 2,
            "COMPAT-176: argmax ties answer the first occurrence");
        eq(simd.argmin(Float32Array.from([3, 1, 1, 3, 1])), 1,
            "COMPAT-176: argmin ties answer the first occurrence");
        eq(simd.argmax(new Float32Array(1000).fill(4)), 0,
            "COMPAT-176: all-equal input answers index 0");
        const sm = Float32Array.from([NaN, 1, 2]);
        simd.softmax(sm);
        ok(sm.every(Number.isNaN), "COMPAT-040: softmax of a NaN input is all-NaN");
        const vi = Float32Array.from([-1, 0, 1, -2, 0.5]);
        simd.vinv(vi);
        eq("" + vi, "-1,0,1,-0.5,2", "COMPAT-041: vinv answers 0 on +-0");
        const vl = Float32Array.from([NaN, 0, 1]);
        simd.vlog(vl);
        ok(Number.isNaN(vl[0]) && vl[1] === -3.4028234663852886e+38 && vl[2] === 0,
            "COMPAT-040: vlog propagates NaN while 0 stays -FLT_MAX");
    }
    // SEC-063: the 257th metrics series is refused.
    {
        Metrics.reset();
        let refused = null;
        for (let i = 0; i < 300; i++) {
            try { Metrics.counter("w3doc_" + i, 1); }
            catch (e) { refused = e; break; }
        }
        ok(refused instanceof RangeError && /full/.test(String(refused)),
            "SEC-063: registering the 257th series is a RangeError");
        Metrics.reset();
    }
    // SEC-047/048/131: the per-record decode budget refuses a crafted bomb.
    {
        const u16 = (v) => [v & 0xff, (v >>> 8) & 0xff];
        const u32 = (v) => [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff];
        const u64 = (v) => [...u32(v >>> 0), ...u32(Math.floor(v / 4294967296))];
        const f64 = (v) => { const b = new Uint8Array(8); new DataView(b.buffer).setFloat64(0, v, true); return [...b]; };
        const rec = (tid, payload) => {
            const head = [0x44, 0x59, 0x4e, 0x53, ...u16(1), ...u16(tid), ...u32(0), ...u64(payload.length)];
            const body = [...head, ...payload];
            return new Uint8Array([...body, ...u32(CRC32C(new Uint8Array(body)) >>> 0)]);
        };
        // Fenwick TID 4: constant-range header declaring 2^24 elements in <64 bytes.
        const bomb = rec(4, [...u32(0xFFFFFFFF), ...u32(1 << 24), 1, ...f64(1)]);
        ok(bomb.length < 64, "SEC-047: the Fenwick bomb record is tiny (" + bomb.length + " B)");
        throws(() => Fenwick.deserialize(bomb), "SEC-047: the decode budget refuses the bomb", Error);
    }
    // COMPAT-106/107/108: TOML.
    {
        throws(() => TOML.stringify({ a: null }), "COMPAT-107: TOML.stringify refuses null", TypeError);
        eq(TOML.parse('s = """a""""').s, 'a"', "COMPAT-106: one quote before the closing delimiter");
        throws(() => TOML.parse('s = """a""""""'), "COMPAT-106: a six-quote run is a SyntaxError");
        throws(() => TOML.parse("x = [1, 2]\n[[x]]"), "COMPAT-108: [[x]] over a static array is refused");
        eq(TOML.parse("[[x]]\na = 1\n[[x]]\na = 2").x.length, 2, "COMPAT-108: [[x]] still appends to header arrays");
    }
    // COMPAT-109/110: Env.load order and quoted escapes.
    {
        const p = Path.temp().join("w3-docreq-env-" + Date.now() + ".env");
        writeFile(p, 'W3DOCA=one\nW3DOCB=${W3DOCA}-two\nW3DOCQ="a\\nb"\n');
        const r = Env.load(p.toString(), { expand: true, override: true });
        eq(r.W3DOCB, "one-two", "COMPAT-109: expand:true sees earlier keys of the same file");
        eq(r.W3DOCQ, "a\nb", "COMPAT-110: quoted escapes unescape before expansion");
        remove(p);
    }
    // COMPAT-230/231: XML document-level hardening.
    {
        throws(() => XMLParse("<a/><!DOCTYPE a>"), "COMPAT-230: a DOCTYPE after the root is refused");
        throws(() => XMLParse("<!DOCTYPE a><!DOCTYPE a><a/>"), "COMPAT-230: a second DOCTYPE is refused");
        throws(() => XMLParse("<!-- a -- b --><a/>"), "COMPAT-231: -- inside a comment is refused");
        eq(XMLParse("<!DOCTYPE a [<!ELEMENT a ANY>]><a/>").name, "a", "COMPAT-230: a prolog DOCTYPE still parses");
    }
    // SEC-246/247: extreme temporal fields.
    {
        throws(() => new Duration({ years: 1000000000001 }), "SEC-246: a Duration component above 1e12 is refused", RangeError);
        ok(new Duration({ years: 1000000000000 }) instanceof Duration, "SEC-246: the exact 1e12 bound still constructs");
        throws(() => date(2024, 1, 1000000001), "SEC-247: date() refuses |day| > 1e9", RangeError);
        throws(() => date(2024, 1, 1, 1000001), "SEC-247: date() refuses |hour| > 1e6", RangeError);
        eq(new Format("2006-01-02").parse("1970-01-02"), 86400, "Format.parse stays the month/day inverse");
    }
    // OPT-053/OPT-055: work budgets.
    {
        const X = [
            Array.from({ length: 1100 }, (_, i) => i * 0.001),
            Array.from({ length: 1100 }, (_, i) => 1 - i * 0.001),
        ];
        throws(() => new PCA().fit(X), "OPT-053: PCA refuses the 1100-column Jacobi workload", RangeError, /budget/);
        const g = new Graph();
        for (let i = 0; i < 9; i++) g.addNode();
        for (const [a, b, w] of [[0, 1, 9], [1, 2, 6], [2, 3, 7], [1, 4, 5], [3, 5, 8], [3, 6, 7], [0, 7, 4], [5, 8, 5], [0, 6, 6]])
            g.addEdge(a, b, w);
        const hv = [0.5, 0.75, 0.5, 0.25, 0, 0.75, 1, 1, 0];
        const r = g.aStar(0, 8, (x) => hv[x] * g.dijkstra(x, 8));
        eq(r.dist, g.dijkstra(0, 8), "OPT-055: aStar reopens closed nodes (admissible but inconsistent heuristic)");
    }
    // SEC-013: the sitemapindex child cap refuses before any child fetch.
    {
        const kids = [];
        for (let i = 0; i < 1001; i++) kids.push("http://x.test/c" + i);
        const idx = "http://x.test/idx.xml";
        const indexXml = "<sitemapindex>" + kids.map((k) => "<sitemap><loc>" + k + "</loc></sitemap>").join("") + "</sitemapindex>";
        let fetches = 0;
        throws(() => Sitemap.list(idx, { get() { fetches++; return { status: 200, body: indexXml }; } }),
            "SEC-013: an index over the 1000-child cap is a RangeError", RangeError, /more than 1000 child/);
        eq(fetches, 1, "SEC-013: the child cap refuses before fetching any child");
    }
    // OPT-046: numeric Fetcher options are int32-ranged.
    throws(() => new Fetcher({ agent: "w3doc/1.0", retries: 4294967296 }),
        "OPT-046: retries beyond int32 is a RangeError", RangeError, /out of range/);
}

async function main() {
    wave3Clauses();
    await httpTests();
    console.log("test_contract_docreqs: " + (n - failed) + " passed, " + failed + " failed");
    if (failed) throw new Error("test_contract_docreqs: " + failed + " failures");
}
main().catch((e) => { throw new Error("test_contract_docreqs: harness error " + e); });
