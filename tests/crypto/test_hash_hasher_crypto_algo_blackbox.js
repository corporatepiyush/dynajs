// covers: dyna:hash.Hasher
// Split from tests/test_algo_blackbox.js: the crypto sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_hash_hasher_crypto_algo_blackbox");
import * as H from "dyna:hash";
import * as K from "dyna:crypto";

let pass=0, fail=0;
const ok=(c,w)=>{ if(c) pass++; else { fail++; print("  FAIL "+w); } };

__kitS.section("streaming==oneshot SHA256 n", () => {
for (const n of [0,1,55,56,63,64,65,127,128,1000]) {
  const s="y".repeat(n);
  const h=new H.Hasher("sha256");
  for (let i=0;i<n;i+=7) h.update(s.slice(i,Math.min(i+7,n)));
  ok(h.digestHex()===H.SHA256Hex(s), "streaming==oneshot SHA256 n="+n);
}
print("black-box: "+pass+" passed, "+fail+" failed");
if (fail) throw new Error(fail+" black-box failures");
});
__kitS.end();
