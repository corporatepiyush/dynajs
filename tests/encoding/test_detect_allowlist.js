// DetectEncoding's allowList, found by the signature-driven input generator
// (tests/lib/sigfuzz.js): the options are read BEFORE the input bytes are
// borrowed (an option getter can detach the buffer), and every allowList entry
// must be a string, so a sparse list of length 2^32-1 fails at index 0.
import { DetectEncoding } from "dyna:encoding";
let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function err(f) { try { f(); return "no throw"; } catch (e) { return e.constructor.name + ": " + e.message; } }
async function aerr(f) { try { await f(); return "no throw"; } catch (e) { return e.constructor.name + ": " + e.message; } }
const huge = () => { const a = []; a.length = 2 ** 32 - 1; return a; };
function fast(label, f, re) {
    const t0 = performance.now();
    const m = err(f);
    const dt = performance.now() - t0;
    ok(re.test(m), label + " (" + m + ")");
    ok(dt < 500, label + ": refused before any work (" + dt.toFixed(1) + " ms)");
}
const bom = () => new Uint8Array([0xef, 0xbb, 0xbf, 0x41]);
fast("a sparse allowList of 2^32-1 slots", () => DetectEncoding(bom(), { allowList: huge() }), /^TypeError: detectEncoding: allowList\[0\] must be a string$/);
eq(err(() => DetectEncoding(bom(), { allowList: ["utf-8", 8] })), "TypeError: detectEncoding: allowList[1] must be a string", "a number entry is refused by index");
{
    const buf = new ArrayBuffer(4);
    new Uint8Array(buf).set([0xef, 0xbb, 0xbf, 0x41]);
    const opts = { get allowList() { buf.transfer(); return ["utf-8"]; } };
    const m = err(() => DetectEncoding(new Uint8Array(buf), opts));
    ok(m === "no throw" || /detached|TypeError/.test(m), "an option getter that detaches the input cannot make it read freed bytes (" + m + ")");
}
eq(DetectEncoding(bom(), { allowList: ["utf-8"] }), "utf-8", "a string allowList still detects (control)");

print("test_detect_allowlist" + ": " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
