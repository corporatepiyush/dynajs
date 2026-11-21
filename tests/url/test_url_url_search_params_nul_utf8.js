// covers: dyna:url.URLSearchParams
// F12: names and values were stored as C strings, so U+0000 cut them short (new URLSearchParams([["k","a\0b"]])
// serialized "k=a"), and a percent-decoded byte run that is not UTF-8 (a lone %C3) was kept raw, so toString()
// echoed "%C3" while get() showed U+FFFD. WHATWG URL application/x-www-form-urlencoded parsing percent-decodes to
// bytes and then runs UTF-8 decode without BOM (each maximal ill-formed subpart becomes one U+FFFD); serializing
// percent-encodes U+0000 as %00 and U+FFFD as %EF%BF%BD. Pairs now carry their lengths and the decode replaces
// ill-formed subparts. Also: the constructor probed a non-URLSearchParams object argument with a throwing
// class check and left that TypeError pending; a later native call that consults the pending exception
// (csv.stringify's option reader) then failed with "URLSearchParams object expected". The last section pins it.
// Oracles: the WHATWG spec text above and the Unicode "maximal subpart" rule (Unicode 15 ch. 3, Table 3-8).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_url_url_search_params_nul_utf8");
import { URLSearchParams } from "dyna:url";
import * as csv from "dyna:csv";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }

__kitS.section("U+0000 survives every entry point (the ISSUES repro first)", () => {
    eq(new URLSearchParams([["k", "a\u0000b"]]).toString(), "k=a%00b", "ISSUES repro");
    eq(new URLSearchParams({ "k\0": "\0" }).toString(), "k%00=%00", "record form");
    eq(new URLSearchParams("a=x\u0000y&b=1").get("a"), "x\0y", "raw NUL in a constructor string");
    eq(new URLSearchParams("a=x\u0000y&b=1").toString(), "a=x%00y&b=1", "and it serializes as %00");
    const p = new URLSearchParams("a=%00&a%00=2");
    eq(p.get("a"), "\0", "decoded %00 value");
    eq(p.get("a\0"), "2", "a key holding NUL is its own key");
    ok(p.has("a\0") && !p.has("a\0x"), "has() compares the whole key");
    p.append("z", "1\0"); p.set("a\0", "v\0");
    eq(p.toString(), "a=%00&a%00=v%00&z=1%00", "append/set keep NUL");
    p.delete("a", "\0");
    eq(p.toString(), "a%00=v%00&z=1%00", "delete(name, value) matches the NUL value");
    eq(JSON.stringify([...p.keys()]), JSON.stringify(["a\0", "z"]), "keys()");
    eq(JSON.stringify(p.getAll("z")), JSON.stringify(["1\0"]), "getAll()");
    const s = new URLSearchParams("b\0=1&b=2&a=3"); s.sort();
    eq(s.toString(), "a=3&b=2&b%00=1", "sort: a key is less than the same key plus NUL");
});

__kitS.section("ill-formed UTF-8 decodes to U+FFFD and serializes as %EF%BF%BD", () => {
    const F = "�";
    const cases = [["%C3", F], ["%C3x", F + "x"], ["%E2%82", F], ["%E2%82%AC", "€"], ["%F0%9F%98", F],
        ["%F0%9F%98%80", "\u{1F600}"], ["%C0%AF", F + F], ["%ED%A0%80", F + F + F], ["%F4%90%80%80", F + F + F + F],
        ["%80", F], ["%FF%FE", F + F], ["%EF%BB%BFa", "﻿a"], ["%E0%A0", F], ["%E0%80%80", F + F + F]];
    for (const [q, want] of cases) {
        const p = new URLSearchParams("a=" + q);
        eq(p.get("a"), want, "decode " + q);
        eq(p.toString(), "a=" + encodeURIComponent(want).replace(/%20/g, "+"), "serialize " + q);
    }
    eq(new URLSearchParams("a=%C3").toString(), "a=%EF%BF%BD", "ISSUES repro: %C3 no longer round-trips raw");
    eq(new URLSearchParams("%C3=1").toString(), "%EF%BF%BD=1", "keys too");
});

__kitS.section("the constructor leaves no pending exception behind (csv observes it)", () => {
    new URLSearchParams([["k", "a"]]);
    new URLSearchParams({ k: "v" });
    let msg = "ok";
    try { csv.stringify([{ a: "1" }]); csv.stringify([["x"]]); } catch (e) { msg = e.message; }
    eq(msg, "ok", "a later csv.stringify sees no stale TypeError");
    eq(new URLSearchParams(new URLSearchParams("q=1")).toString(), "q=1", "copying another URLSearchParams still works");
});

if (fails) throw new Error(fails + " of " + n + " checks failed");
print("test_url_url_search_params_nul_utf8: " + n + " checks passed");
__kitS.end();
