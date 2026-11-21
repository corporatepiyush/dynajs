// covers: dyna:hash.SHA512Hex
// Split from tests/test_algo_blackbox.js: the crypto sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_hash_sha512_hex_crypto_algo_blackbox");
import * as H from "dyna:hash";
import * as K from "dyna:crypto";

let pass=0, fail=0;
const ok=(c,w)=>{ if(c) pass++; else { fail++; print("  FAIL "+w); } };

__kitS.section("SHA512 abc", () => {
ok(H.SHA512Hex("abc").startsWith("ddaf35a193617aba"),"SHA512 abc");
print("black-box: "+pass+" passed, "+fail+" failed");
if (fail) throw new Error(fail+" black-box failures");
});
__kitS.end();
