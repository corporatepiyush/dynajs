// Split from tests/test_algo_blackbox.js: the compress sections.
import * as C from "dyna:compress";

let pass=0, fail=0;
const ok=(c,w)=>{ if(c) pass++; else { fail++; print("  FAIL "+w); } };
for (const n of [0,1,64,1000,70000]) {
  const b=new Uint8Array(n); for(let i=0;i<n;i++) b[i]=(i%251)&255;
  ok(C.gunzip(C.gzip(b)).length===n,"gzip rt n="+n);
  ok(C.lz4Decompress(C.lz4Compress(b)).length===n,"lz4 rt n="+n);
}
print("black-box: "+pass+" passed, "+fail+" failed");
if (fail) throw new Error(fail+" black-box failures");
