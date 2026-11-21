// covers: dyna:uuid.ULIDTime
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_uuid_ulid_time_idgen");
import { NanoID, NanoIDAlphabet, ULID, ULIDTime } from "dyna:uuid";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}

const DEFAULT_ALPHA = "useandom-26T198340PX75pxJACKVERYMINDBUSHWOLFGQZbfghjklqvwyzrict";

__kitS.section("ULIDTime round-trips", () => {
for (const ms of [0, 1, 1000, 1469918176385, 281474976710655]) {
    eq(ULIDTime(ULID(ms)), ms, "ULIDTime round-trips " + ms);
}
});

__kitS.section("ULIDTime accepts lowercase", () => {
eq(ULIDTime(ULID(12345).toLowerCase()), 12345, "ULIDTime accepts lowercase");
});

__kitS.section("ULIDTime refuses", () => {
for (const bad of ["", "short", "0".repeat(27), "0".repeat(25) + "U"]) {
    let threw = false;
    try { ULIDTime(bad); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, "ULIDTime refuses " + JSON.stringify(bad.slice(0, 30)));
}
});
__kitS.end();

if (fails) {
    print("test_ids: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_ids failed");
}
print("test_ids: " + n + " assertions, 0 failures");
