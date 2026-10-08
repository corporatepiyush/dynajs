// flags: --std
// timeout: 120
import { domainToASCII, domainToUnicode, punycodeDecode } from "dyna:url";

let n = 0, failed = 0;
function ok(c, msg) {
    n++;
    if (!c) {
        failed++;
        console.log("  FAIL " + msg);
    }
}
function throws(fn, msg) {
    n++;
    try {
        fn();
    } catch (e) {
        return;
    }
    failed++;
    console.log("  FAIL " + msg + " (no throw)");
}

throws(() => domainToASCII("a\u00B7b.com"), "MIDDLE DOT outside l\u00B7l is refused");
ok(domainToASCII("l\u00B7l.com") === "xn--ll-0ea.com", "l\u00B7l stays valid");
throws(() => domainToASCII("a\u30FBb.com"), "KATAKANA MIDDLE DOT without kana/han is refused");
ok(domainToASCII("\u30AB\u30FB\u30CA.com").length > 0, "katakana + middle dot is valid");
throws(() => domainToASCII("\u03B1\u0375b.com"), "GREEK KERAIA before non-Greek is refused");
ok(domainToASCII("\u03B1\u0375\u03B2.com").length > 0, "keraia before Greek is valid");

ok(domainToASCII("\u1E9E") === "xn--zca", "U+1E9E maps to sharp s (non-transitional)");
ok(domainToUnicode("\u1E9E") === "\u00DF", "domainToUnicode(U+1E9E) is sharp s");
ok(domainToASCII("\u1E9E", { transitional: true }) === "ss", "transitional 1E9E still maps to ss");

ok(domainToASCII("b\u00FCcher.de.") === "xn--bcher-kva.de.", "an IDN with a trailing dot is accepted");
ok(domainToASCII("xn--bcher-kva.de.") === "xn--bcher-kva.de.", "an A-label FQDN is accepted");
ok(domainToASCII("example.com.") === "example.com.", "the ASCII fast path is unchanged");

ok(punycodeDecode("-a").charCodeAt(0) === 0x80, "a leading-delimiter (empty basic part) decodes per RFC 3492");

{
    const t0 = Date.now();
    let threw = false;
    try { domainToASCII("\u00FC".repeat(200000) + ".com"); } catch (e) { threw = true; }
    const dt = Date.now() - t0;
    ok(threw && dt < 3000, "a 200k-code-point label is refused fast (dt=" + dt + "ms)");
}
console.log("test_idna_contexto: " + (n - failed) + " passed, " + failed + " failed");
if (failed)
    throw new Error("test_idna_contexto: " + failed + " failures");
