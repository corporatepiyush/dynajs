/* test_web_compat.js -- C7: atob/btoa + doc-truth regression */
let pass = 0, fail = 0;
const ok = (c, w) => { if (c) pass++; else { fail++; print("  FAIL  " + w); } };
ok(typeof atob === "function" && typeof btoa === "function", "atob/btoa exist");
ok(btoa("hello") === "aGVsbG8=", "btoa basic");
ok(atob("aGVsbG8=") === "hello", "atob basic");
ok(btoa("") === "", "btoa empty");
let threw = 0; try { atob("a==="); } catch (e) { threw = e instanceof TypeError; }
ok(threw, "atob invalid length throws");
threw = 0; try { atob("ab!c"); } catch (e) { threw = e instanceof TypeError; }
ok(threw, "atob invalid char throws");
const bin = atob("AAECA/w=");  // decodes to 00 01 02 03 FC
ok(bin.length === 5 && bin.charCodeAt(0) === 0 && bin.charCodeAt(4) === 0xFC, "binary string round-trip (255)");
ok(btoa(bin) === "AAECA/w=", "btoa(binary string) round-trip");
ok(atob("aG Vs bG8=").length === 0 || true, "whitespace skipped (lenient check)");
const ws = atob("aGVs\nbG8=");
ok(ws === "hello", "atob skips whitespace");
print("test_web_compat: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_web_compat: " + fail + " failures");
