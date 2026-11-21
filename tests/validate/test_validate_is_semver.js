// covers: dyna:validate.IsSemver
import { sections as __kitSections } from "../lib/kit.js";
import { perfCheck } from "../lib/perf.js";
const __kitS = __kitSections("test_validate_is_semver");
import { cpuNow } from "../lib/cpuclock.js";
import { IsEmail, IsIBAN, IsCreditCard, IsAlpha, IsAlphanumeric, IsAscii,
         IsUUID, IsSemver, IsJWT, IsE164,
         IsIPv4, IsIPv6, IsIP, IsPort, IsBase64, IsBase64Url, IsHex,
         IsHexColor, IsNumeric, IsDateString, IsRFC3339, IsJSON,
         IsMimeType, IsStrongPassword }
    from "dyna:validate";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function ok(v, msg) { assert(v === true, msg + " (expected true)"); }
function no(v, msg) { assert(v === false, msg + " (expected false)"); }
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}

__kitS.section("a tampered delegate table does not change IsSemver", () => {
    const before = IsSemver("1.2.3");
    globalThis.__dyna_vx = globalThis.__dyna_vx || {};
    globalThis.__dyna_vx["dyna:semver.isValid"] = function () { return true; };
    assert(IsSemver("1.2.3") === before, "a tampered delegate table does not change IsSemver");
    assert(IsSemver("not a version") === false, "a tampered delegate cannot force a positive result");
    delete globalThis.__dyna_vx["dyna:semver.isValid"];
});

__kitS.section("IsSemver x", () => {
    const N = 200000;
    const t0 = cpuNow();
    for (let i = 0; i < N; i++) IsSemver("1.2.3");
    const tSemver = cpuNow() - t0;
    const t1 = cpuNow();
    for (let i = 0; i < N; i++) IsEmail("a@b.co");
    const tEmail = cpuNow() - t1;
    assert(tSemver < 400,
        "IsSemver x" + N + " must stay on the cached delegate (" + tSemver + " ms)");
    perfCheck(assert, tSemver < tEmail * 15 + 150,
        "IsSemver must not recompile its module per call (" + tSemver +
        " ms vs IsEmail " + tEmail + " ms)");
});
__kitS.end();

if (fails) {
    print("test_validate: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_validate failed");
}
print("test_validate: " + n + " assertions, 0 failures");
