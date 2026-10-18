// Split from tests/test_cov_sys_time_random_uuid_misc.js: the uuid sections.
import * as uuid from "dyna:uuid";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { assert(a === b, m + " got " + JSON.stringify(a) + " want " + JSON.stringify(b)); }
function throws(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL: " + m + " did not throw"); }
    catch (e) { if (re && !re.test(String(e.message))) { fails++; print("FAIL: " + m + " wrong msg: " + e.message); } }
}
function assertClose(a, b, tol, m) {
    n++;
    if (!(Math.abs(a - b) <= tol)) { fails++; print("FAIL: " + m + " |" + a + "-" + b + "|>" + tol); }
}
print("=== sys ===");
print("=== uuid ===");
{
    const seen = new Set();
    for (let i = 0; i < 200; i++) {
        const u = uuid.v4();
        assert(uuid.validate(u), "v4 valid: " + u);
        assert(u.length === 36 && u[14] === "4", "v4 version nibble");
        assert(!seen.has(u), "v4 unique");
        seen.add(u);
    }
    const v7a = uuid.v7();
    assert(uuid.validate(v7a), "v7 valid");
    assert(v7a[14] === "7", "v7 version nibble");
    const ul = uuid.ULID();
    assert(ul.length === 26, "ULID length 26: " + ul);
    const ul2 = uuid.ULID();
    assert(ul2 >= ul || ul2 !== ul, "ULIDs distinct"); n--;
    n++;
    assert(ul !== ul2, "ULID unique");
    assert(typeof uuid.ULIDTime(uuid.ULID()) === "number", "ULIDTime extracts ms from a ULID");
    const nid = uuid.NanoID();
    assert(nid.length === 21, "NanoID default length 21: " + nid);
    const nidCustom = uuid.NanoID(10);
    assert(nidCustom.length === 10, "NanoID size 10");
    const hexGen = uuid.NanoIDAlphabet("abc", 12);
    const hexId = typeof hexGen === "function" ? hexGen() : hexGen;
    assert(typeof hexId === "string" && hexId.length === 12 && /^[abc]+$/.test(hexId),
           "custom-alphabet NanoID emits only a/b/c: " + hexId);
    const v5a = uuid.v5(uuid.NAMESPACE_URL, "https://example.com");
    const v5b = uuid.v5(uuid.NAMESPACE_URL, "https://example.com");
    eq(v5a, v5b, "v5 deterministic");
    assert(v5a !== uuid.v5(uuid.NAMESPACE_URL, "https://other.com"), "v5 name-sensitive");
    assert(v5a !== uuid.v5(uuid.NAMESPACE_DNS, "https://example.com"), "v5 ns-sensitive");
    eq(uuid.v5(uuid.NAMESPACE_DNS, "python.org"),
       "886313e1-3b8a-5372-9b90-0c9aee199e5d", "v5 RFC vector python.org");
    if (uuid.v3) assert(uuid.v3(uuid.NAMESPACE_DNS, "x") !== v5a, "v3 differs from v5");
    eq(uuid.NIL, "00000000-0000-0000-0000-000000000000", "NIL constant");
    assert(uuid.validate(uuid.NIL), "NIL validates");
    if (uuid.MAX) assert(uuid.validate(uuid.MAX), "MAX validates");
    for (const bad of ["", "zzzz", "not-a-uuid", "g0000000-0000-0000-0000-000000000000",
                       "00000000-0000-0000-0000-00000000000", "0000000-00000-0000-0000-000000000000"]) {
        eq(uuid.validate(bad), false, "validate rejects " + JSON.stringify(bad));
    }
    eq(uuid.version(uuid.v4()), 4, "version(v4)=4");
    eq(uuid.version(uuid.v7()), 7, "version(v7)=7");
    eq(uuid.variant(uuid.v4()), "RFC4122", "variant RFC4122");
    const u = uuid.v4();
    const bs = uuid.bytes(u);
    assert(bs.length === 16, "bytes length 16");
    eq(uuid.fromBytes(bs), u.toLowerCase() === u ? u : u, "fromBytes(bytes(u)) == u");
    eq(uuid.parse(u), u, "parse canonicalizes");
    const p = uuid.bytes(u);
    eq(JSON.stringify([...p]), JSON.stringify([...bs]), "bytes stable");
    assert(typeof uuid.NanoIDAlphabet === "function", "NanoIDAlphabet exported as factory");
}
if (fails) {
    print("test_uuid_cov_sys_time_random_uuid_misc: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_uuid_cov_sys_time_random_uuid_misc failed");
}
print("test_uuid_cov_sys_time_random_uuid_misc: " + n + " assertions, 0 failures");
