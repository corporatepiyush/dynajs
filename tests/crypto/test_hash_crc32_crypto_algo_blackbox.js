// covers: dyna:hash.CRC32
// Split from tests/test_algo_blackbox.js: the crypto sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_hash_crc32_crypto_algo_blackbox");
import * as H from "dyna:hash";
import * as K from "dyna:crypto";

let pass=0, fail=0;
const ok=(c,w)=>{ if(c) pass++; else { fail++; print("  FAIL "+w); } };

__kitS.section("CRC32 check value 0xCBF43926", () => {
ok(H.CRC32("123456789")===0xCBF43926,"CRC32 check value 0xCBF43926, got "+H.CRC32("123456789").toString(16));
});

const SIZES=[0,1,3,4,7,8,9,12,15,16,17,31,32,63,64,65,191,192,193,1023,1024,65535,65536];

__kitS.section("SHA256 len at n", () => {
for (const n of SIZES) {
  const s = "x".repeat(n);
  ok(H.SHA256Hex(s).length===64, "SHA256 len at n="+n);
  ok(typeof H.CRC32(s)==="number" && H.CRC32(s)>=0, "CRC32 at n="+n);
  ok(typeof H.CRC32C(s)==="number" && H.CRC32C(s)>=0, "CRC32C at n="+n);
}
});

const bytes = new Uint8Array([0x61,0x62,0x63]);

__kitS.section("CRC32 Uint8Array == string", () => {
ok(H.CRC32(bytes)===H.CRC32("abc"),"CRC32 Uint8Array == string");
print("black-box: "+pass+" passed, "+fail+" failed");
if (fail) throw new Error(fail+" black-box failures");
});
__kitS.end();
