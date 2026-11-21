// timeout: 120
// covers: dyna:url.punycodeDecode
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_url_punycode_decode_idna_contexto");
import { domainToASCII, domainToUnicode, punycodeDecode } from "dyna:url";
import { cpuNow } from "../lib/cpuclock.js";

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

__kitS.section("a leading-delimiter (empty basic part) decodes per RFC 3492", () => {
ok(punycodeDecode("-a").charCodeAt(0) === 0x80, "a leading-delimiter (empty basic part) decodes per RFC 3492");
console.log("test_idna_contexto: " + (n - failed) + " passed, " + failed + " failed");
if (failed)
    throw new Error("test_idna_contexto: " + failed + " failures");
});
__kitS.end();
