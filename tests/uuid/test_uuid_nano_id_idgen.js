// covers: dyna:uuid.NanoID
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_uuid_nano_id_idgen");
import { NanoID, NanoIDAlphabet, ULID, ULIDTime } from "dyna:uuid";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}

const DEFAULT_ALPHA = "useandom-26T198340PX75pxJACKVERYMINDBUSHWOLFGQZbfghjklqvwyzrict";

eq(NanoID().length, 21, "NanoID default length is 21");

for (const size of [1, 2, 8, 21, 32, 64, 255, 256, 1000]) {
    eq(NanoID(size).length, size, "NanoID(" + size + ") length");
}

__kitS.section("every NanoID character is in the default alphabet", () => {
    let bad = 0;
    for (let i = 0; i < 500; i++)
        for (const ch of NanoID(64))
            if (DEFAULT_ALPHA.indexOf(ch) < 0) bad++;
    eq(bad, 0, "every NanoID character is in the default alphabet");
});

__kitS.section("20000 NanoIDs are all distinct", () => {
    const seen = new Set();
    for (let i = 0; i < 20000; i++) seen.add(NanoID());
    eq(seen.size, 20000, "20000 NanoIDs are all distinct");
});

__kitS.section("NanoID default alphabet is flat", () => {
    const counts = new Map();
    const N = 2000, LEN = 64;
    for (let i = 0; i < N; i++)
        for (const ch of NanoID(LEN)) counts.set(ch, (counts.get(ch) || 0) + 1);
    const total = N * LEN, expect = total / DEFAULT_ALPHA.length;
    let chi = 0;
    for (const a of DEFAULT_ALPHA) {
        const o = counts.get(a) || 0;
        chi += ((o - expect) * (o - expect)) / expect;
    }
    assert(chi < 112, "NanoID default alphabet is flat (chi2 " + chi.toFixed(1) + " < 112)");
    eq(counts.size, DEFAULT_ALPHA.length, "every alphabet symbol was produced");
});

for (const bad of [0, -1, 100000]) {
    let threw = false;
    try { NanoID(bad); } catch (e) { threw = e instanceof RangeError; }
    assert(threw, "NanoID(" + bad + ") throws RangeError");
}

__kitS.section("throws RangeError (no int32 wrap)", () => {
    for (const bad of [2 ** 32 + 5, 2 ** 64 + 5, 6.5, NaN]) {
        let threw = false;
        try { NanoID(bad); } catch (e) { threw = e instanceof RangeError; }
        assert(threw, "NanoID(" + bad + ") throws RangeError (no int32 wrap)");
    }
    {
        let threw = false;
        try { NanoIDAlphabet("ab", 2 ** 32 + 5); } catch (e) { threw = e instanceof RangeError; }
        assert(threw, "NanoIDAlphabet size wrap throws RangeError");
    }
    {
        let threw = false;
        try { ULID(2 ** 64 + 5); } catch (e) { threw = e instanceof RangeError; }
        assert(threw, "ULID(2**64 + 5) throws RangeError (no int64 wrap)");
    }
});
__kitS.end();

if (fails) {
    print("test_ids: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_ids failed");
}
print("test_ids: " + n + " assertions, 0 failures");
