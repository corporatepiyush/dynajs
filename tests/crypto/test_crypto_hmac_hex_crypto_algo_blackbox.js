// covers: dyna:crypto.HMACHex
// Split from tests/test_algo_blackbox.js: the crypto sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_crypto_hmac_hex_crypto_algo_blackbox");
import * as H from "dyna:hash";
import * as K from "dyna:crypto";

let pass=0, fail=0;
const ok=(c,w)=>{ if(c) pass++; else { fail++; print("  FAIL "+w); } };

__kitS.section("The quick brown fox jumps over the lazy dog", () => {
ok(K.HMACHex("sha256","key","The quick brown fox jumps over the lazy dog")===
   "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8","HMAC-SHA256 rfc vector");
print("black-box: "+pass+" passed, "+fail+" failed");
if (fail) throw new Error(fail+" black-box failures");
});
__kitS.end();
