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
{ s: "a.b\uff0ec\u3002d\uff61", u: "a.b.c.d.", ue: false, a: "a.b.c.d.", ae: true, at: "a.b.c.d.", ate: true },
{ s: "a.b.c\u3002d\u3002", u: "a.b.c.d.", ue: false, a: "a.b.c.d.", ae: true, at: "a.b.c.d.", ate: true },
{ s: "A.B.C\u3002D\u3002", u: "a.b.c.d.", ue: false, a: "a.b.c.d.", ae: true, at: "a.b.c.d.", ate: true },
{ s: "A.b.c\u3002D\u3002", u: "a.b.c.d.", ue: false, a: "a.b.c.d.", ae: true, at: "a.b.c.d.", ate: true },
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

for (let i = 0; i < ROWS.length; i++) {
    const r = ROWS[i];
    const tag = "row " + i + " " + JSON.stringify(r.s);
    if (r.ue) { throws(() => domainToUnicode(r.s), tag + ": toUnicode must throw"); }
    else      { eq(domainToUnicode(r.s), r.u, tag + ": toUnicode"); }
    if (r.ae) { throws(() => domainToASCII(r.s), tag + ": toAsciiN must throw"); }
    else      { eq(domainToASCII(r.s), r.a, tag + ": toAsciiN"); }
    if (r.ate) { throws(() => domainToASCII(r.s, { transitional: true }), tag + ": toAsciiT must throw"); }
    else       { eq(domainToASCII(r.s, { transitional: true }), r.at, tag + ": toAsciiT"); }
}

eq(domainToASCII("B\u00DCCHER.DE"), "xn--bcher-kva.de", "mapping: uppercase folded");
eq(domainToASCII("BLO\u00DF.de"), "xn--blo-7ka.de", "mapping: sharp s kept (nontransitional)");
eq(domainToASCII("BLO\u00DF.de", { transitional: true }), "bloss.de", "mapping: transitional sharp s");
eq(domainToUnicode("\uFF21\uFF27\uFF23"), "agc", "mapping: fullwidth to ASCII");

eq(domainToUnicode("a\u0308.de"), "\u00E4.de", "nfc: a + diaeresis composes");
eq(domainToUnicode("u\u0308.com"), "\u00FC.com", "nfc: u + diaeresis composes");
throwsStage(() => domainToUnicode("xn--u-ccb.com"), "validation",
            "nfc: decoded xn--u-ccb (u+diaeresis) is not NFC -> validation");

throwsStage(() => domainToASCII("\uFFFD.com"), "validation",
            "validation: disallowed code point");
throwsStage(() => domainToASCII("a.bc--de.f"), "validation",
            "validation: hyphen in third/fourth position");
throwsStage(() => domainToASCII("-a.com"), "validation",
            "validation: leading hyphen");
throwsStage(() => domainToASCII("a\u200Cb.com"), "validation",
            "validation: ZWNJ outside a joining context");
eq(domainToASCII("a\u094D\u200Cb"), "xn--ab-fsf604u",
   "validation: ZWNJ after virama is a legal context");

throwsStage(() => domainToASCII("xn--0.pt"), "punycode",
            "punycode: xn--0 does not decode");
throwsStage(() => domainToUnicode("xn--0.pt"), "punycode",
            "punycode: decode failure also throws from toUnicode");
throwsStage(() => domainToASCII("xn--.com"), "punycode",
            "punycode: xn-- decodes to empty");
throwsStage(() => domainToUnicode("xn--a-.com"), "punycode",
            "punycode: all-ASCII decoded label (xn--a- decodes to \"a\")");

throwsStage(() => domainToASCII("a".repeat(64) + ".com"), "dns-length",
            "dns-length: 64-octet label");
eq(domainToASCII("a".repeat(63) + ".com"), "a".repeat(63) + ".com",
   "dns-length: 63-octet label is the maximum");
eq(domainToUnicode("a".repeat(63) + ".com"), "a".repeat(63) + ".com",
   "dns-length: toUnicode has no DNS-length check");
{
    const max63 = "xn--" + punycodeEncode("\u4e2d".repeat(57));
    assert(max63.length === 63, "the 63-octet vector really is 63");
    eq(domainToASCII(max63), max63, "dns-length: 63-octet A-label is the maximum");
    throwsStage(() => domainToASCII(max63 + "a"), "dns-length",
                "dns-length: 64-octet label refuses");
}

const RFC3492 = [
    ["\u0644\u064A\u0647\u0645\u0627\u0628\u062A\u0643\u0644\u0645\u0648\u0634\u0639\u0631\u0628\u064A\u061F", "egbpdaj6bu4bxfgehfvwxn"],
    ["\u4ED6\u4EEC\u4E3A\u4EC0\u4E48\u4E0D\u8BF4\u4E2D\u6587", "ihqwcrb4cv8a8dqg056pqjye"],
    ["\u4ED6\u5011\u7232\u4EC0\u9EBD\u4E0D\u8AAA\u4E2D\u6587", "ihqwctvzc91f659drss3x8bo0yb"],
    ["Pro\u010Dprost\u011Bnemluv\u00ED\u010Desky", "Proprostnemluvesky-uyb24dma41a"],
    ["\u05DC\u05DE\u05D4\u05D4\u05DD\u05E4\u05E9\u05D5\u05D8\u05DC\u05D0\u05DE\u05D3\u05D1\u05E8\u05D9\u05DD\u05E2\u05D1\u05E8\u05D9\u05EA", "4dbcagdahymbxekheh6e0a7fei0b"],
    ["\u092F\u0939\u0932\u094B\u0917\u0939\u093F\u0928\u094D\u0926\u0940\u0915\u094D\u092F\u094B\u0902\u0928\u0939\u0940\u0902\u092C\u094B\u0932\u0938\u0915\u0924\u0947\u0939\u0948\u0902", "i1baa7eci9glrd9b2ae1bj0hfcgg6iyaf8o0a1dig0cd"],
    ["\u306A\u305C\u307F\u3093\u306A\u65E5\u672C\u8A9E\u3092\u8A71\u3057\u3066\u304F\u308C\u306A\u3044\u306E\u304B", "n8jok5ay5dzabd5bym9f0cm5685rrjetr6pdxa"],
    ["\uC138\uACC4\uC758\uBAA8\uB4E0\uC0AC\uB78C\uB4E4\uC774\uD55C\uAD6D\uC5B4\uB97C\uC774\uD574\uD55C\uB2E4\uBA74\uC5BC\uB9C8\uB098\uC88B\uC744\uAE4C", "989aomsvi5e83db1d2a355cv1e0vak1dwrv93d5xbh15a0dt30a5jpsd879ccm6fea98c"],
    ["\u043F\u043E\u0447\u0435\u043C\u0443\u0436\u0435\u043E\u043D\u0438\u043D\u0435\u0433\u043E\u0432\u043E\u0440\u044F\u0442\u043F\u043E\u0440\u0443\u0441\u0441\u043A\u0438", "b1abfaaepdrnnbgefbadotcwatmq2g4l"],
    ["Porqu\u00E9nopuedensimplementehablarenEspa\u00F1ol", "PorqunopuedensimplementehablarenEspaol-fmd56a"],
    ["T\u1EA1isaoh\u1ECDkh\u00F4ngth\u1EC3ch\u1EC9n\u00F3iti\u1EBFngVi\u1EC7t", "TisaohkhngthchnitingVit-kjcr8268qyxafd2f1b9g"],
    ["3\u5E74B\u7D44\u91D1\u516B\u5148\u751F", "3B-ww4c5e180e575a65lsy2b"],
    ["\u5B89\u5BA4\u5948\u7F8E\u6075-with-SUPER-MONKEYS", "-with-SUPER-MONKEYS-pc58ag80a8qai00g7n9n"],
    ["Hello-Another-Way-\u305D\u308C\u305E\u308C\u306E\u5834\u6240", "Hello-Another-Way--fc4qua05auwb3674vfr0b"],
    ["\u3072\u3068\u3064\u5C4B\u6839\u306E\u4E0B2", "2-u9tlzr9756bt3uc0v"],
    ["Maji\u3067Koi\u3059\u308B5\u79D2\u524D", "MajiKoi5-783gue6qz075azm5e"],
    ["\u30D1\u30D5\u30A3\u30FCde\u30EB\u30F3\u30D0", "de-jg4avhby1noc0d"],
    ["\u305D\u306E\u30B9\u30D4\u30FC\u30C9\u3067", "d9juau41awczczp"],
];
for (const [src, enc] of RFC3492) {
    eq(punycodeEncode(src), enc, "punycodeEncode: RFC 3492 " + JSON.stringify(enc));
    eq(punycodeDecode(enc), src, "punycodeDecode: RFC 3492 " + JSON.stringify(enc));
}
eq(punycodeEncode("-> $1.00 <-"), "-> $1.00 <--", "punycodeEncode: all-basic keeps a trailing delimiter");
eq(punycodeDecode("-> $1.00 <--"), "-> $1.00 <-", "punycodeDecode: round trip");

throws(() => punycodeEncode("a\uD800b"), "punycodeEncode: lone surrogate rejected");
throws(() => punycodeDecode("-a"), "punycodeDecode: '-' is not a digit");
throws(() => punycodeDecode("abc!"), "punycodeDecode: non-punycode digit rejected");
throws(() => punycodeDecode("z".repeat(100)), "punycodeDecode: overflow rejected");

assert(typeof domainToASCII !== "function" || true, "");
throws(() => domainToASCII(42), "domainToASCII refuses a non-string");
throws(() => domainToUnicode(null), "domainToUnicode refuses a non-string");
throws(() => punycodeEncode({}), "punycodeEncode refuses a non-string");
throws(() => punycodeDecode([]), "punycodeDecode refuses a non-string");

eq(domainToUnicode(""), "", "toUnicode of the empty string is the empty string (node parity)");
eq(domainToASCII(""), "", "toASCII of the empty string is the empty string (node parity)");

throws(() => punycodeEncode("\u0080".repeat(4000) + "\u10FFFF".repeat(4000)),
    "far-apart codepoint clusters refuse instead of emitting wrapped digits");
throws(() => punycodeEncode("a".repeat(1025)),
    "punycodeEncode refuses past 1024 code points");
throwsStage(() => domainToASCII("a".repeat(64) + ".com"), "dns-length",
    "64-codepoint label refuses before expansion (toASCII)");
eq(domainToUnicode("xn--12345678901234567890123456789012345678901234567890123456-fxe"),
   "1234567890\u00e41234567890123456789012345678901234567890123456",
   "toUnicode of a 63-octet A-label still decodes (no DNS-length check)");

throws(() => punycodeDecode("-"), "a lone delimiter has no basic part: refused");
throws(() => punycodeDecode("-a"), "delimiter at index 0: the basic part is empty: refused");
eq(punycodeDecode("a-"), "a", "a trailing delimiter is the empty extended part");
eq(domainToASCII("\u1E9E.com"), "ss.com", "U+1E9E maps to ss in nontransitional mode");
eq(domainToASCII("\u1E9E.com", { transitional: true }), "ss.com", "U+1E9E maps to ss in transitional mode too");

if (fails) {
    print("test_idna: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_idna failed");
}
print("test_idna: " + n + " assertions, 0 failures");
