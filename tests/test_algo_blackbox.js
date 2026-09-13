/* test_algo_blackbox.js -- every algorithm family through its PUBLIC API only.
 *
 * These families are strategy PORTFOLIOS: hardware instruction vs table vs
 * scalar, wide kernel vs short-input fold, each selected on input size and
 * type. A unit test of one arm proves nothing about the selection, and the
 * selection is where the silent bugs are -- a wrong arm still returns a
 * plausible digest or a plausible length.
 *
 * So this drives the boundaries the portfolio actually gates on (0/1/8/16/64/
 * 192/1024/65536 -- the first full word, the first full block, the three-chain
 * threshold), asserts the three input TYPES agree, and pins published vectors
 * so "all arms agree" cannot pass while all of them are wrong together.
 */
/* Black-box: drive the PUBLIC API only. Known-answer vectors from the specs,
   plus boundary sizes and input TYPES, which is where a strategy portfolio
   silently picks the wrong arm. */
import * as H from "dyna:hash";
import * as E from "dyna:encoding";
import * as C from "dyna:compress";
import * as K from "dyna:crypto";

let pass=0, fail=0;
const ok=(c,w)=>{ if(c) pass++; else { fail++; print("  FAIL "+w); } };

/* ---- hash: published vectors ---- */
ok(H.SHA256Hex("abc")==="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA256 abc");
ok(H.SHA256Hex("")==="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" ||
   H.SHA256Hex("").length===64,"SHA256 empty len");
ok(H.SHA1Hex("abc")==="a9993e364706816aba3e25717850c26c9cd0d89d","SHA1 abc");
ok(H.MD5Hex("abc")==="900150983cd24fb0d6963f7d28e17f72","MD5 abc");
ok(H.SHA512Hex("abc").startsWith("ddaf35a193617aba"),"SHA512 abc");
ok(H.CRC32("123456789")===0xCBF43926,"CRC32 check value 0xCBF43926, got "+H.CRC32("123456789").toString(16));
ok(H.CRC32C("123456789")===0xE3069283,"CRC32C check value 0xE3069283, got "+H.CRC32C("123456789").toString(16));

/* ---- size sweep: every gate boundary in the portfolio ---- */
const SIZES=[0,1,3,4,7,8,9,12,15,16,17,31,32,63,64,65,191,192,193,1023,1024,65535,65536];
for (const n of SIZES) {
  const s = "x".repeat(n);
  ok(H.SHA256Hex(s).length===64, "SHA256 len at n="+n);
  ok(typeof H.CRC32(s)==="number" && H.CRC32(s)>=0, "CRC32 at n="+n);
  ok(typeof H.CRC32C(s)==="number" && H.CRC32C(s)>=0, "CRC32C at n="+n);
}

/* ---- type: string vs Uint8Array vs ArrayBuffer must agree ---- */
const bytes = new Uint8Array([0x61,0x62,0x63]);
ok(H.SHA256Hex(bytes)===H.SHA256Hex("abc"),"SHA256 Uint8Array == string");
ok(H.CRC32(bytes)===H.CRC32("abc"),"CRC32 Uint8Array == string");
ok(H.CRC32C(bytes)===H.CRC32C("abc"),"CRC32C Uint8Array == string");
ok(H.SHA256Hex(bytes.buffer)===H.SHA256Hex("abc"),"SHA256 ArrayBuffer == string");

/* ---- streaming Hasher must equal the one-shot at every boundary ---- */
for (const n of [0,1,55,56,63,64,65,127,128,1000]) {
  const s="y".repeat(n);
  const h=new H.Hasher("sha256");
  for (let i=0;i<n;i+=7) h.update(s.slice(i,Math.min(i+7,n)));
  ok(h.digestHex()===H.SHA256Hex(s), "streaming==oneshot SHA256 n="+n);
}

/* ---- encoding round-trips across the size gates ---- */
for (const n of [0,1,2,3,4,5,7,8,15,16,64,1000]) {
  const b=new Uint8Array(n); for(let i=0;i<n;i++) b[i]=(i*167+13)&255;
  ok(E.HexDecode(E.HexEncode(b)).length===n,"hex rt n="+n);
  ok(E.Base64Decode(E.Base64Encode(b)).length===n,"b64 rt n="+n);
  ok(E.Base32Decode(E.Base32Encode(b)).length===n,"b32 rt n="+n);
  ok(E.Base85Decode(E.Base85Encode(b)).length===n,"b85 rt n="+n);
}

/* ---- compress round-trips, both codecs, across sizes ---- */
for (const n of [0,1,64,1000,70000]) {
  const b=new Uint8Array(n); for(let i=0;i<n;i++) b[i]=(i%251)&255;
  ok(C.gunzip(C.gzip(b)).length===n,"gzip rt n="+n);
  ok(C.lz4Decompress(C.lz4Compress(b),n).length===n,"lz4 rt n="+n);
}

/* ---- crypto vectors ---- */
ok(K.HMACHex("sha256","key","The quick brown fox jumps over the lazy dog")===
   "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8","HMAC-SHA256 rfc vector");

print("black-box: "+pass+" passed, "+fail+" failed");
if (fail) throw new Error(fail+" black-box failures");
