// timeout: 180
// Split from tests/test_data_w3_fixes.js: the core sections.
import { TOML } from "dyna:config";
import { PgPool, RedisPool } from "../../src/pool.js";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { check(a === b, m + " -- got " + JSON.stringify(a) + ", want " + JSON.stringify(b)); }
async function throws(fn, m) { n++; try { await fn(); fails++; print("FAIL: " + m + " (did not throw)"); } catch (e) { } }
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
peq("s = '''a''''", "a'", "TOML multiline literal: one apostrophe before the closing delimiter");
peq("s = '''a'''''", "a''", "TOML multiline literal: two apostrophes before the closing delimiter");
peq("s = \"\"\"plain\"\"\"", "plain", "a plain multiline basic string still parses");
peq("s = \"\"\"a\"b\"\"\"", "a\"b", "a single interior quote still parses");
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
if (fails === 0) print("test_core_data_w3_fixes: all " + n + " checks passed");
else print("test_core_data_w3_fixes: " + fails + " FAILED of " + n);
