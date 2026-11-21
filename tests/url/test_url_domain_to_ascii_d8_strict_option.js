// covers: dyna:url.domainToASCII
// Owner decision D8: by default domainToASCII drops a UTS #46 disallowed code point (lone surrogates included), as
// before policy P15 ("a\u0080b.com" -> "ab.com"); { strict: true } refuses it with a TypeError (the P15 behaviour).
// Every other validation failure throws in both modes. strict must be a boolean. The schema idn-hostname check
// shares the IDNA core and stays strict (pinned here through dyna:schema).
import { suite } from "../lib/kit.js";
import { domainToASCII, domainToUnicode } from "dyna:url";
import { Schema } from "dyna:schema";

const t = suite("test_url_domain_to_ascii_d8_strict_option");
const S = { strict: true };

t.test("default drops disallowed code points", (a) => {
    a.eq(domainToASCII("a\u0080b.com"), "ab.com", "C1 control");
    a.eq(domainToASCII("a\ud800b.com"), "ab.com", "lone surrogate");
    a.eq(domainToASCII("�büch.de"), "xn--bch-hoa.de", "U+FFFD dropped, rest maps");
    a.eq(domainToASCII("a\u0080b.com", { strict: false }), "ab.com", "strict: false is the default");
    a.eq(domainToASCII("a\u0080b.com", { transitional: true }), "ab.com", "transitional too");
});

t.test("strict refuses them", (a) => {
    a.throws(() => domainToASCII("a\u0080b.com", S), TypeError);
    a.throws(() => domainToASCII("a\ud800b.com", S), TypeError);
    a.throws(() => domainToASCII("a\u0080b.com", { strict: true, transitional: true }), TypeError);
    a.eq(domainToASCII("Bücher.de", S), "xn--bcher-kva.de", "valid input maps");
});

t.test("other validation errors throw in both modes", (a) => {
    for (const o of [undefined, S]) {
        a.throws(() => domainToASCII("xn--a.com", o), TypeError, "bad A-label");
        a.throws(() => domainToASCII("-a.com", o), TypeError, "leading hyphen");
        a.throws(() => domainToASCII("a".repeat(64) + ".com", o), TypeError, "label over 63");
    }
    a.throws(() => domainToUnicode("a\u0080b.com"), TypeError, "domainToUnicode is unchanged");
});

t.test("schema idn-hostname stays strict", (a) => {
    const idn = (s) => Schema.compile({ format: "idn-hostname" }, { formats: "assertion" }).validate(s).valid;
    a.eq(idn("a\u0080b.com"), false, "a disallowed code point is invalid");
    a.eq(idn("b\u00fccher.example"), true, "a valid IDN passes");
});

t.test("bad option types", (a) => {
    a.throws(() => domainToASCII("a.com", { strict: 1 }), TypeError);
    a.throws(() => domainToASCII("a.com", { strict: "true" }), TypeError);
    a.throws(() => domainToASCII("a.com", { strikt: true }), TypeError, "unknown key");
    a.throws(() => domainToUnicode("a.com", { strict: true }), TypeError, "domainToUnicode has no strict");
});

await t.run();
