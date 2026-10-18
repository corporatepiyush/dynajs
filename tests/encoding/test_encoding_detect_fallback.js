// flags: --std
// timeout: 60
import { DetectEncoding } from "dyna:encoding";

let n = 0, failed = 0;
function ok(c, msg) {
    n++;
    if (!c) {
        failed++;
        console.log("  FAIL " + msg);
    }
}

{
    const raw = new Uint8Array(32).fill(0x80);
    ok(DetectEncoding(raw, { fallback: "latin1" }) === "latin1",
        "fallback string is returned when nothing detects (freed-then-read regression)");
    ok(DetectEncoding(raw, { fallback: "utf-16le" }) === "utf-16le",
        "second fallback call stays correct");
    ok(DetectEncoding(raw) === "utf-8",
        "default fallback is utf-8");
    ok(DetectEncoding(new Uint8Array([0xEF, 0xBB, 0xBF, 0x41]), { fallback: "latin1" }) === "utf-8",
        "BOM still wins over the fallback");
    ok(DetectEncoding(raw, { fallback: "latin1", allowList: ["latin1", "utf-8"] }) === "latin1",
        "allowList accepts the fallback");
    let threw = false;
    try {
        DetectEncoding(raw, { fallback: "latin1", allowList: ["utf-8"] });
    } catch (e) {
        threw = /not in allowList/.test(String(e));
    }
    ok(threw, "fallback not in allowList still throws");
    for (let i = 0; i < 200; i++) {
        const s = "fb" + i;
        const keep = s + "x".repeat(24);
        if (DetectEncoding(raw, { fallback: keep.slice(0, 8) }) !== keep.slice(0, 8)) {
            ok(false, "repeat call " + i + " mismatched");
            break;
        }
    }
    console.log("test_encoding_detect_fallback: " + (n - failed) + " passed, " + failed + " failed");
    if (failed)
        throw new Error("test_encoding_detect_fallback: " + failed + " failures");
}
