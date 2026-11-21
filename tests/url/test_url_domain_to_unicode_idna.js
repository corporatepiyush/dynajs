// covers: dyna:url.domainToUnicode
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_url_domain_to_unicode_idna");
import { domainToASCII, domainToUnicode, punycodeEncode, punycodeDecode } from "dyna:url";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throws(fn, msg) {
    let t = false, e = null;
    try { fn(); } catch (ex) { t = true; e = ex; }
    assert(t, msg);
    return e;
}
function throwsStage(fn, stage, msg) {
    const e = throws(fn, msg);
    if (e) {
        const m = String(e && e.message !== undefined ? e.message : e);
        if (!m.includes(stage))
            assert(false, msg + " (message " + JSON.stringify(m) + " does not name stage " + stage + ")");
    }
}

const ROWS = [
{ s: "\u00e0\u05d0", u: "", ue: true, a: "xn--0ca24w", ae: true, at: "xn--0ca24w", ate: true },
{ s: "a\u0300\u05d0", u: "\u00e0\u05d0", ue: true, a: "xn--0ca24w", ae: true, at: "xn--0ca24w", ate: true },
{ s: "A\u0300\u05d0", u: "\u00e0\u05d0", ue: true, a: "xn--0ca24w", ae: true, at: "xn--0ca24w", ate: true },
{ s: "\u00c0\u05d0", u: "\u00e0\u05d0", ue: true, a: "xn--0ca24w", ae: true, at: "xn--0ca24w", ate: true },
{ s: "xn--0ca24w", u: "\u00e0\u05d0", ue: true, a: "xn--0ca24w", ae: true, at: "xn--0ca24w", ate: true },
{ s: "0\u00e0.\u05d0", u: "", ue: true, a: "xn--0-sfa.xn--4db", ae: true, at: "xn--0-sfa.xn--4db", ate: true },
{ s: "0a\u0300.\u05d0", u: "0\u00e0.\u05d0", ue: true, a: "xn--0-sfa.xn--4db", ae: true, at: "xn--0-sfa.xn--4db", ate: true },
{ s: "0A\u0300.\u05d0", u: "0\u00e0.\u05d0", ue: true, a: "xn--0-sfa.xn--4db", ae: true, at: "xn--0-sfa.xn--4db", ate: true },
{ s: "a\u200cb", u: "", ue: true, a: "xn--ab-j1t", ae: true, at: "ab", ate: false },
{ s: "A\u200cB", u: "a\u200cb", ue: true, a: "xn--ab-j1t", ae: true, at: "ab", ate: false },
{ s: "A\u200cb", u: "a\u200cb", ue: true, a: "xn--ab-j1t", ae: true, at: "ab", ate: false },
{ s: "a\u200db", u: "", ue: true, a: "xn--ab-m1t", ae: true, at: "ab", ate: false },
{ s: "A\u200dB", u: "a\u200db", ue: true, a: "xn--ab-m1t", ae: true, at: "ab", ate: false },
{ s: "A\u200db", u: "a\u200db", ue: true, a: "xn--ab-m1t", ae: true, at: "ab", ate: false },
{ s: "\u200cx\u200dn\u200c-\u200d-b\u00df", u: "", ue: true, a: "xn--xn--b-pqa5796ccahd", ae: true, at: "xn--bss", ate: false },
{ s: "\u200cX\u200dN\u200c-\u200d-BSS", u: "\u200cx\u200dn\u200c-\u200d-bss", ue: true, a: "xn--xn--bss-7z6ccid", ae: true, at: "xn--bss", ate: false },
{ s: "1234567890a\u03081234567890123456789012345678901234567890123456", u: "1234567890\u00e41234567890123456789012345678901234567890123456", ue: false, a: "xn--12345678901234567890123456789012345678901234567890123456-fxe", ae: true, at: "xn--12345678901234567890123456789012345678901234567890123456-fxe", ate: true },
{ s: "1234567890A\u03081234567890123456789012345678901234567890123456", u: "1234567890\u00e41234567890123456789012345678901234567890123456", ue: false, a: "xn--12345678901234567890123456789012345678901234567890123456-fxe", ae: true, at: "xn--12345678901234567890123456789012345678901234567890123456-fxe", ate: true },
{ s: "1234567890\u00c41234567890123456789012345678901234567890123456", u: "1234567890\u00e41234567890123456789012345678901234567890123456", ue: false, a: "xn--12345678901234567890123456789012345678901234567890123456-fxe", ae: true, at: "xn--12345678901234567890123456789012345678901234567890123456-fxe", ate: true },
{ s: "xn--12345678901234567890123456789012345678901234567890123456-fxe", u: "1234567890\u00e41234567890123456789012345678901234567890123456", ue: false, a: "xn--12345678901234567890123456789012345678901234567890123456-fxe", ae: true, at: "xn--12345678901234567890123456789012345678901234567890123456-fxe", ate: true },
{ s: "a.b\uff0ec\u3002d\uff61", u: "a.b.c.d.", ue: false, a: "a.b.c.d.", ae: false, at: "a.b.c.d.", ate: false },
{ s: "a.b.c\u3002d\u3002", u: "a.b.c.d.", ue: false, a: "a.b.c.d.", ae: false, at: "a.b.c.d.", ate: false },
{ s: "A.B.C\u3002D\u3002", u: "a.b.c.d.", ue: false, a: "a.b.c.d.", ae: false, at: "a.b.c.d.", ate: false },
{ s: "A.b.c\u3002D\u3002", u: "a.b.c.d.", ue: false, a: "a.b.c.d.", ae: false, at: "a.b.c.d.", ate: false },
{ s: "Fa\u00df.de", u: "fa\u00df.de", ue: false, a: "xn--fa-hia.de", ae: false, at: "fass.de", ate: false },
{ s: "\u03b2\u03bf\u0301\u03bb\u03bf\u03c2.com", u: "\u03b2\u03cc\u03bb\u03bf\u03c2.com", ue: false, a: "xn--nxasmm1c.com", ae: false, at: "xn--nxasmq6b.com", ate: false },
{ s: "A\u094d\u200cB", u: "a\u094d\u200cb", ue: false, a: "xn--ab-fsf604u", ae: false, at: "xn--ab-fsf", ate: false },
{ s: "A\u094d\u200dB", u: "a\u094d\u200db", ue: false, a: "xn--ab-fsf014u", ae: false, at: "xn--ab-fsf", ate: false },
{ s: "xn--fa-hia.de", u: "fa\u00df.de", ue: false, a: "xn--fa-hia.de", ae: false, at: "xn--fa-hia.de", ate: false },
{ s: "a\u0300.\u05d0\u0308", u: "\u00e0.\u05d0\u0308", ue: false, a: "xn--0ca.xn--ssa73l", ae: false, at: "xn--0ca.xn--ssa73l", ate: false },
{ s: "A\u0300.\u05d0\u0308", u: "\u00e0.\u05d0\u0308", ue: false, a: "xn--0ca.xn--ssa73l", ae: false, at: "xn--0ca.xn--ssa73l", ate: false },
{ s: "\u00c0.\u05d0\u0308", u: "\u00e0.\u05d0\u0308", ue: false, a: "xn--0ca.xn--ssa73l", ae: false, at: "xn--0ca.xn--ssa73l", ate: false },
{ s: "xn--0ca.xn--ssa73l", u: "\u00e0.\u05d0\u0308", ue: false, a: "xn--0ca.xn--ssa73l", ae: false, at: "xn--0ca.xn--ssa73l", ate: false },
{ s: "a\u0300\u0308.\u05d0", u: "\u00e0\u0308.\u05d0", ue: false, a: "xn--0ca81i.xn--4db", ae: false, at: "xn--0ca81i.xn--4db", ate: false },
{ s: "A\u0300\u0308.\u05d0", u: "\u00e0\u0308.\u05d0", ue: false, a: "xn--0ca81i.xn--4db", ae: false, at: "xn--0ca81i.xn--4db", ate: false },
{ s: "\u00c0\u0308.\u05d0", u: "\u00e0\u0308.\u05d0", ue: false, a: "xn--0ca81i.xn--4db", ae: false, at: "xn--0ca81i.xn--4db", ate: false },
{ s: "xn--0ca81i.xn--4db", u: "\u00e0\u0308.\u05d0", ue: false, a: "xn--0ca81i.xn--4db", ae: false, at: "xn--0ca81i.xn--4db", ate: false },
{ s: "A\u094d\u200cb", u: "a\u094d\u200cb", ue: false, a: "xn--ab-fsf604u", ae: false, at: "xn--ab-fsf", ate: false },
{ s: "xn--ab-fsf", u: "a\u094db", ue: false, a: "xn--ab-fsf", ae: false, at: "xn--ab-fsf", ate: false },
{ s: "A\u094dB", u: "a\u094db", ue: false, a: "xn--ab-fsf", ae: false, at: "xn--ab-fsf", ate: false },
{ s: "A\u094db", u: "a\u094db", ue: false, a: "xn--ab-fsf", ae: false, at: "xn--ab-fsf", ate: false },
{ s: "xn--ab-fsf604u", u: "a\u094d\u200cb", ue: false, a: "xn--ab-fsf604u", ae: false, at: "xn--ab-fsf604u", ate: false },
{ s: "A\u094d\u200db", u: "a\u094d\u200db", ue: false, a: "xn--ab-fsf014u", ae: false, at: "xn--ab-fsf", ate: false },
{ s: "xn--ab-fsf014u", u: "a\u094d\u200db", ue: false, a: "xn--ab-fsf014u", ae: false, at: "xn--ab-fsf014u", ate: false },
{ s: "xn--7a", u: "\u00a1", ue: false, a: "xn--7a", ae: false, at: "xn--7a", ate: false },
{ s: "xn--pkf", u: "\u19da", ue: false, a: "xn--pkf", ae: false, at: "xn--pkf", ate: false },
{ s: "xn--3y9a", u: "\uab60", ue: false, a: "xn--3y9a", ae: false, at: "xn--3y9a", ate: false },
{ s: "www.eXample.cOm", u: "www.example.com", ue: false, a: "www.example.com", ae: false, at: "www.example.com", ate: false },
{ s: "B\u00fccher.de", u: "b\u00fccher.de", ue: false, a: "xn--bcher-kva.de", ae: false, at: "xn--bcher-kva.de", ate: false },
{ s: "Bu\u0308cher.de", u: "b\u00fccher.de", ue: false, a: "xn--bcher-kva.de", ae: false, at: "xn--bcher-kva.de", ate: false },
{ s: "bu\u0308cher.de", u: "b\u00fccher.de", ue: false, a: "xn--bcher-kva.de", ae: false, at: "xn--bcher-kva.de", ate: false },
{ s: "B\u00dcCHER.DE", u: "b\u00fccher.de", ue: false, a: "xn--bcher-kva.de", ae: false, at: "xn--bcher-kva.de", ate: false },
{ s: "BU\u0308CHER.DE", u: "b\u00fccher.de", ue: false, a: "xn--bcher-kva.de", ae: false, at: "xn--bcher-kva.de", ate: false },
{ s: "xn--aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", u: "\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080\u0080", ue: true, a: "xn--aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", ae: true, at: "xn--aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", ate: true }];

__kitS.section("mapping: fullwidth to ASCII", () => {
eq(domainToUnicode("\uFF21\uFF27\uFF23"), "agc", "mapping: fullwidth to ASCII");
});

__kitS.section("nfc: a + diaeresis composes", () => {
eq(domainToUnicode("a\u0308.de"), "\u00E4.de", "nfc: a + diaeresis composes");
});

__kitS.section("nfc: u + diaeresis composes", () => {
eq(domainToUnicode("u\u0308.com"), "\u00FC.com", "nfc: u + diaeresis composes");
});

throwsStage(() => domainToUnicode("xn--u-ccb.com"), "validation",
            "nfc: decoded xn--u-ccb (u+diaeresis) is not NFC -> validation");

throwsStage(() => domainToUnicode("xn--0.pt"), "punycode",
            "punycode: decode failure also throws from toUnicode");

throwsStage(() => domainToUnicode("xn--a-.com"), "punycode",
            "punycode: all-ASCII decoded label (xn--a- decodes to \"a\")");

__kitS.section("dns-length: toUnicode has no DNS-length check", () => {
eq(domainToUnicode("a".repeat(63) + ".com"), "a".repeat(63) + ".com",
   "dns-length: toUnicode has no DNS-length check");
});

__kitS.section("domainToUnicode refuses a non-string", () => {
throws(() => domainToUnicode(null), "domainToUnicode refuses a non-string");
});

__kitS.section("toUnicode of the empty string is the empty string (node parity)", () => {
eq(domainToUnicode(""), "", "toUnicode of the empty string is the empty string (node parity)");
});

__kitS.section("toUnicode of a 63-octet A-label still decodes (no DNS-length check)", () => {
eq(domainToUnicode("xn--12345678901234567890123456789012345678901234567890123456-fxe"),
   "1234567890\u00e41234567890123456789012345678901234567890123456",
   "toUnicode of a 63-octet A-label still decodes (no DNS-length check)");
});
__kitS.end();

if (fails) {
    print("test_idna: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_idna failed");
}
print("test_idna: " + n + " assertions, 0 failures");
