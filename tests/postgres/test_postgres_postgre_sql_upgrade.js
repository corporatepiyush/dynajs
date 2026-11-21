// covers: dyna:postgres.PostgreSQL
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_postgres_postgre_sql_upgrade");
import { PostgreSQL } from "dyna:postgres";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function throws(fn, ctor, msg) {
    let caught = null;
    try { fn(); } catch (e) { caught = e; }
    assert(caught instanceof ctor, msg + " (" + ctor.name + ", got " + caught + ")");
}

__kitS.section("PostgreSQL.prototype.queryIter exists natively (no pool.js import)", () => {
    assert(typeof PostgreSQL.prototype.queryIter === "function",
        "PostgreSQL.prototype.queryIter exists natively (no pool.js import)");
    const proto = Object.getPrototypeOf(PostgreSQL.prototype.queryIter.call(
        { query: () => { } }));
    assert(typeof proto.next === "function", "queryIter().next exists");
    assert(typeof proto["return"] === "function", "queryIter().return exists");
    assert(typeof proto[Symbol.asyncIterator] === "function",
        "queryIter()[Symbol.asyncIterator] exists");
    assert(!("close" in proto) || typeof proto.close === "function",
        "no stray close() on the stream prototype");
    let caught = null;
    const fakeConn = { query: () => undefined };
    try { PostgreSQL.prototype.queryIter.call(fakeConn, "SELECT 1", undefined, { batchh: 2 }); }
    catch (e) { caught = e; }
    assert(caught instanceof TypeError &&
           caught.message === 'unknown option "batchh" (valid: batch, maxRows)',
        "queryIter opts bag is strict, got: " + caught);
    caught = null;
    try { PostgreSQL.prototype.queryIter.call(fakeConn, "SELECT 1", {}, {}); }
    catch (e) { caught = e; }
    assert(caught instanceof TypeError && /parameters must be an array/.test(caught.message),
        "params must be an array, got: " + caught);
    let called = 0;
    const lazyConn = { query: () => { called++; return undefined; } };
    const it = PostgreSQL.prototype.queryIter.call(lazyConn, "SELECT 1");
    assert(called === 0, "queryIter() construction is lazy (no query() call)");
    assert(it && typeof it.next === "function", "the stream object is returned");
});
__kitS.end();

print("test_net_upgrade: all tests passed (" + n + " assertions)");
