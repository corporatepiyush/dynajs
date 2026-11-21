// covers: dyna:hash.CRC32C
// Split from tests/test_algo_blackbox.js: the crypto sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_hash_crc32_c_crypto_algo_blackbox");
import * as H from "dyna:hash";
import * as K from "dyna:crypto";

let pass=0, fail=0;
const ok=(c,w)=>{ if(c) pass++; else { fail++; print("  FAIL "+w); } };

__kitS.section("CRC32C check value 0xE3069283", () => {
ok(H.CRC32C("123456789")===0xE3069283,"CRC32C check value 0xE3069283, got "+H.CRC32C("123456789").toString(16));
});

const bytes = new Uint8Array([0x61,0x62,0x63]);

__kitS.section("CRC32C Uint8Array == string", () => {
ok(H.CRC32C(bytes)===H.CRC32C("abc"),"CRC32C Uint8Array == string");
print("black-box: "+pass+" passed, "+fail+" failed");
if (fail) throw new Error(fail+" black-box failures");
});
__kitS.end();
