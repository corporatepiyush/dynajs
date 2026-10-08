// timeout: 180
import { TOML, Env } from "dyna:config";
import { XMLParse } from "dyna:xml";
import { Proto } from "dyna:serialize";
import { PCA } from "dyna:ml";
import { RRule, Duration, date, parseDurationMs, parseDurationSecs } from "dyna:time";
import { Bytes, lastIndexOf } from "dyna:bytes";
import { Parse as YAMLParse } from "dyna:yaml";
import { Graph } from "dyna:structures";
import { PgPool, RedisPool } from "../src/pool.js";
import { Path, writeFile, remove } from "dyna:file";
import { setEnv, setNativeMemoryLimit } from "dyna:sys";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { check(a === b, m + " -- got " + JSON.stringify(a) + ", want " + JSON.stringify(b)); }
function throws(fn, m) { n++; try { fn(); fails++; print("FAIL: " + m + " (did not throw)"); } catch (e) { } }
function peq(text, expected, m) {
    n++;
    let got;
    try { got = TOML.parse(text).s; }
    catch (e) { fails++; print("FAIL: " + m + " (threw " + e + ")"); return; }
    check(got === expected, m + " -- got " + JSON.stringify(got) + ", want " + JSON.stringify(expected));
}

peq("s = \"\"\"a\"\"\"\"", "a\"", "TOML multiline basic: one quote before the closing delimiter");
peq("s = \"\"\"a\"\"\"\"\"", "a\"\"", "TOML multiline basic: two quotes before the closing delimiter");
peq("s = \"\"\"\"This,\" she said, \"is just a pointless statement.\"\"\"\"",
    "\"This,\" she said, \"is just a pointless statement.\"", "TOML 1.0 spec str7 (quote-run)");
throws(() => TOML.parse("s = \"\"\"a\"\"\"\"\"\""), "TOML multiline basic: a six-quote content run is refused");
peq("s = '''a''''", "a'", "TOML multiline literal: one apostrophe before the closing delimiter");
peq("s = '''a'''''", "a''", "TOML multiline literal: two apostrophes before the closing delimiter");
throws(() => TOML.parse("s = '''a''''''"), "TOML multiline literal: a six-apostrophe content run is refused");
peq("s = \"\"\"plain\"\"\"", "plain", "a plain multiline basic string still parses");
peq("s = \"\"\"a\"b\"\"\"", "a\"b", "a single interior quote still parses");

throws(() => TOML.parse("x = [1, 2]\n[[x]]"), "TOML [[x]] over a static array is refused");
throws(() => TOML.parse("x = [{ a = 1 }]\n[[x]]"), "TOML [[x]] over a static inline array is refused");
throws(() => TOML.parse("x = []\n[[x]]"), "TOML [[x]] over an empty static array is refused");
eq(TOML.parse("[[x]]\na = 1\n[[x]]\na = 2").x.length, 2, "TOML [[x]] still appends to a header array");
eq(TOML.parse("x = [1, 2]").x.length, 2, "a static array still parses");
eq(TOML.parse("[[x]]\n[[x.y]]\na = 1\n[[x.y]]\na = 2").x[0].y.length, 2,
   "a nested array-of-tables still parses");

eq(XMLParse("<!DOCTYPE a [<!ELEMENT a ANY>]><a/>").name, "a", "a prolog DOCTYPE is still skipped");
throws(() => XMLParse("<a/><!DOCTYPE a>"), "COMPAT-230: a DOCTYPE after the root is refused");
throws(() => XMLParse("<!DOCTYPE a><!DOCTYPE a><a/>"), "COMPAT-230: a second DOCTYPE is refused");
eq(XMLParse("<?xml version=\"1.0\"?><!DOCTYPE a [<!ELEMENT a ANY>]><!-- c --><a/>").name, "a",
   "decl, DOCTYPE and comment prolog order still parses");
eq(XMLParse("<!-- ok --><a/>").name, "a", "a plain comment still parses");
eq(XMLParse("<!----><a/>").name, "a", "an empty comment still parses");
throws(() => XMLParse("<!-- a -- b --><a/>"), "COMPAT-231: -- inside a comment is refused");
throws(() => XMLParse("<!--a---><a/>"), "COMPAT-231: a comment ending in - is refused");
eq(XMLParse("<a><!-- a - b --><b/></a>").children.length, 1, "a single - inside a comment still parses");
throws(() => XMLParse("<" + "a".repeat(17 * 1024 * 1024) + "/>"),
       "COMPAT-232: an element name over the token cap is refused");
throws(() => XMLParse("<a " + "b".repeat(17 * 1024 * 1024) + "='1'/>"),
       "COMPAT-232: an attribute name over the token cap is refused");

{
    const U64 = { fields: [{ name: "a", number: 1, type: "uint64" }] };
    const F64 = { fields: [{ name: "a", number: 1, type: "fixed64" }] };
    const big = 18446744073709549568;
    eq(Proto.decode(Proto.encode({ a: big }, U64), U64).a, big,
       "COMPAT-236: a uint64 above 2^63 round-trips");
    eq(Proto.decode(Proto.encode({ a: big }, F64), F64).a, big,
       "COMPAT-236: a fixed64 above 2^63 round-trips");
    eq(Proto.decode(Proto.encode({ a: 9223372036854775808 }, U64), U64).a, 9223372036854775808,
       "COMPAT-236: exactly 2^63 round-trips");
    throws(() => Proto.encode({ a: 18446744073709551616 }, U64),
           "COMPAT-236: 2^64 is still refused");
}
{
    const fields = [];
    for (let i = 0; i < 40000; i++)
        fields.push({ name: "f" + i, number: i + 1, type: "int32" });
    const t0 = Date.now();
    const out = Proto.encode({}, { fields });
    const ms = Date.now() - t0;
    check(out instanceof Uint8Array && ms < 150,
          "OPT-048: a 40000-field unique schema compiles in " + ms + " ms (< 150)");
    throws(() => Proto.encode({}, { fields: [
        { name: "a", number: 1, type: "int32" },
        { name: "b", number: 1, type: "int32" }] }),
        "OPT-048: a duplicate field number is still refused");
    throws(() => Proto.encode({}, { fields: [
        { name: "a", number: 1, type: "int32" },
        { name: "a", number: 2, type: "int32" }] }),
        "OPT-048: a duplicate field name is still refused");
}

class StubPgPool extends PgPool {
    _spawn() {
        return {
            transactionStatus: "I",
            closed: false,
            close() { this.closed = true; },
            query: async () => ({ ok: 1 }),
            pipeline: async () => [],
        };
    }
}

class FlakyPgPool extends StubPgPool {
    _spawn() {
        const c = super._spawn();
        c.pipeline = async () => { throw new RangeError("socket dead"); };
        return c;
    }
}

class StubRedisPool extends RedisPool {
    _spawn() {
        return {
            closed: false,
            close() { this.closed = true; },
            command: async () => 1,
            pipeline: async () => [],
        };
    }
}

{
    const p = new StubPgPool({ size: 1 });
    const c = await p.acquire();
    p.release(c);
    p.release(c);
    eq(p.stats.free, 1, "COMPAT-112: a double release leaves exactly one free entry");
    eq(p.stats.used, 0, "COMPAT-112: and nothing in use");
    await p.close();
}
{
    const p = new StubPgPool({ size: 1 });
    p.release({ close() {} });
    eq(p.stats.total, 0, "COMPAT-112: releasing an unknown connection never pools it");
    await p.close();
}
{
    const p = new FlakyPgPool({ size: 1 });
    let threw = false;
    try { await p.pipeline([{ sql: "SELECT 1" }]); } catch (e) { threw = true; }
    check(threw, "COMPAT-112: a failing pipeline propagates its error");
    eq(p.stats.total, 0, "COMPAT-112: and retires the broken connection");
    await p.close();
}
{
    const p = new StubRedisPool({ size: 1 });
    const c = await p.acquire();
    p.release(c);
    p.release(c);
    eq(p.stats.free, 1, "COMPAT-112: RedisPool double release leaves exactly one free entry");
    eq(p.stats.used, 0, "COMPAT-112: and nothing in use");
    await p.close();
}

const envPath = Path.cwd().join("w3_env_order.env");
writeFile(envPath, "981234568=b\n981234567=$981234568\n");
{
    const rec = Env.load(envPath.toString(), { expand: true, override: true });
    eq(rec["981234567"], "b",
       "COMPAT-109: expansion follows file order for integer-like keys");
}
setEnv("981234567", "");
setEnv("981234568", "");

writeFile(envPath, "W3QUOTED=\"a\\nb\"\n");
{
    const rec = Env.load(envPath.toString(), { expand: true, assign: false });
    eq(rec.W3QUOTED, "a\nb", "COMPAT-110: quoted escapes survive expand:true");
}
writeFile(envPath, "W3Q2=\" x $W3NOPE y \"\n");
{
    const rec = Env.load(envPath.toString(), { expand: true, assign: false });
    eq(rec.W3Q2, " x  y ", "COMPAT-110: expansion still runs inside a quoted value");
}
remove(envPath);

{
    const g = new Graph();
    for (let i = 0; i < 9; i++) g.addNode();
    for (const [a, b, w] of [[0,1,9],[1,2,6],[2,3,7],[1,4,5],[3,5,8],[3,6,7],[0,7,4],[5,8,5],[0,6,6]])
        g.addEdge(a, b, w);
    const hv = [0.5,0.75,0.5,0.25,0,0.75,1,1,0];
    const hd = [];
    for (let x = 0; x < 9; x++) hd.push(g.dijkstra(x, 8));
    const r = g.aStar(0, 8, (x) => hv[x] * hd[x]);
    eq(r.dist, g.dijkstra(0, 8),
       "OPT-055: aStar reopens closed nodes (admissible but inconsistent heuristic)");
}
{
    const X = [
        Array.from({ length: 1100 }, (_, i) => i * 0.001),
        Array.from({ length: 1100 }, (_, i) => 1 - i * 0.001),
    ];
    throws(() => new PCA().fit(X), "OPT-053: PCA refuses a 1100-column Jacobi workload");
    const small = new PCA().fit([[1, 2], [3, 4], [5, 6]]);
    check(small instanceof PCA, "OPT-053: a small PCA fit still works");
}
throws(() => new Duration({ years: 2 ** 62 }), "SEC-246: a huge Duration year is refused");
throws(() => new Duration({ milliseconds: 2 ** 62 }), "SEC-246: huge Duration milliseconds are refused");
check(new Duration({ hours: 25, minutes: 1 }) instanceof Duration,
      "SEC-246: ordinary Duration components still construct");
throws(() => date(2024, 1, 2 ** 62), "SEC-247: date() refuses a huge day field");
throws(() => date(2024, 1, 1, 2 ** 40), "SEC-247: date() refuses a huge hour field");
eq(date(1970, 1, 1), 0, "SEC-247: the epoch still converts");
throws(() => YAMLParse("{:a}"), "final-verify: {:a} is refused like PyYAML");
throws(() => YAMLParse("{: a}"), "final-verify: {: a} is refused like PyYAML");
throws(() => YAMLParse("{ : a }"), "final-verify: { : a } is refused like PyYAML");
eq(JSON.stringify(YAMLParse("{a:b}")), JSON.stringify({ "a:b": null }),
   "final-verify: the PyYAML-exact {a:b} rule still holds");
{
    const keep = [];
    let threwRR = false;
    setNativeMemoryLimit(1 << 20);
    try { for (let i = 0; i < 200000; i++) keep.push(new RRule({ freq: "DAILY" })); }
    catch (e) { threwRR = true; }
    keep.length = 0;
    const keep2 = [];
    let threwDur = false;
    setNativeMemoryLimit(1 << 20);
    try { for (let i = 0; i < 200000; i++) keep2.push(new Duration({ days: 1 })); }
    catch (e) { threwDur = true; }
    keep2.length = 0;
    setNativeMemoryLimit(0);
    check(threwRR, "OPT-057: RRule allocations count against the native limit");
    check(threwDur, "OPT-058: Duration allocations count against the native limit");
}

eq(parseDurationMs("2784249659845191438ns"), 2784249659845.1914,
   "COMPAT-262: parseDurationMs is correctly rounded (was 1 ulp high)");
eq(parseDurationMs("-8531699129511414324ns"), -8531699129511.414,
   "COMPAT-262: negative ns is correctly rounded");
eq(parseDurationMs("8785356068037452444ns"), 8785356068037.452,
   "COMPAT-262: a third counterexample");
eq(parseDurationSecs("1577960769301841103ns"), 1577960769.301841,
   "COMPAT-262: parseDurationSecs is correctly rounded");
eq(parseDurationMs("200000h"), 7.2e11, "COMPAT-262: the documented exact case still holds");
eq(parseDurationSecs("4581490823437043614ns"), 4581490823.437043,
   "COMPAT-262: seconds counterexample");
{
    const hay = Bytes.alloc(1 << 20, 0x61);
    const needle = Bytes.alloc(1024, 0x61);
    const t0 = Date.now();
    let last = -1;
    for (let i = 0; i < 5; i++)
        last = lastIndexOf(hay, needle);
    const ms = Date.now() - t0;
    eq(last, (1 << 20) - 1024, "OPT-017: lastIndexOf finds the last match");
    check(ms < 300, "OPT-017: 5 adversarial lastIndexOf calls took " + ms + " ms (< 300)");
    eq(lastIndexOf(Bytes.alloc(64, 0x62), Bytes.alloc(4, 0x62)), 60,
       "OPT-017: a small lastIndexOf is still exact");
}
{
    const pos = [];
    for (let i = 0; i < 800; i++) pos.push(1 + (i % 336));
    const days = [], months = [];
    for (let d = 1; d <= 28; d++) days.push(d);
    for (let m = 1; m <= 12; m++) months.push(m);
    const r = new RRule({ freq: "YEARLY", dtstart: date(2026, 1, 1),
        bymonth: months, bymonthday: days, bysetpos: pos });
    const t0 = Date.now();
    const out = r.between(date(2026, 1, 1), date(2126, 1, 1));
    const ms = Date.now() - t0;
    check(out.length > 0 && ms < 2000,
          "OPT-056: a 800-entry duplicated BYSETPOS rule between 2026..2126 in " + ms + " ms");
    let sortedUnique = true;
    for (let i = 1; i < out.length; i++)
        if (!(out[i] > out[i - 1])) { sortedUnique = false; break; }
    check(sortedUnique, "OPT-056: BYSETPOS results stay sorted and de-duplicated");
}

if (fails === 0) print("test_data_w3_fixes: all " + n + " checks passed");
else print("test_data_w3_fixes: " + fails + " FAILED of " + n);
