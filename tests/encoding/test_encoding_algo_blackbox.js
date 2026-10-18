// Split from tests/test_algo_blackbox.js: the encoding sections.
import * as E from "dyna:encoding";

let pass=0, fail=0;
const ok=(c,w)=>{ if(c) pass++; else { fail++; print("  FAIL "+w); } };
for (const n of [0,1,2,3,4,5,7,8,15,16,64,1000]) {
  const b=new Uint8Array(n); for(let i=0;i<n;i++) b[i]=(i*167+13)&255;
  ok(E.HexDecode(E.HexEncode(b)).length===n,"hex rt n="+n);
  ok(E.Base64Decode(E.Base64Encode(b)).length===n,"b64 rt n="+n);
  ok(E.Base32Decode(E.Base32Encode(b)).length===n,"b32 rt n="+n);
  ok(E.Base85Decode(E.Base85Encode(b)).length===n,"b85 rt n="+n);
}
print("black-box: "+pass+" passed, "+fail+" failed");
if (fail) throw new Error(fail+" black-box failures");
