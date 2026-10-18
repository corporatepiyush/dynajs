// timeout: 180
// Split from tests/test_data_w3_fixes.js: the bytes sections.
import { TOML } from "dyna:config";
import { Bytes, lastIndexOf } from "dyna:bytes";

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
if (fails === 0) print("test_bytes_data_w3_fixes: all " + n + " checks passed");
else print("test_bytes_data_w3_fixes: " + fails + " FAILED of " + n);
