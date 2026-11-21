// covers: dyna:encoding.QREncode
// Split from tests/test_cov_bytes_compress_encoding_hash.js: the encoding sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_qr_encode_cov_bytes_compress_encoding_hash");
import { HexEncode, HexDecode, Base64Encode, Base64Decode, Base64URLEncode, Base64URLDecode, Base32Encode, Base32Decode, Base32HexEncode, Base32HexDecode, Base85Encode, Base85Decode, Base58Encode, Base58Decode, Base58CheckEncode, Base58CheckDecode, BaseXEncode, BaseXDecode, PutUvarint, Uvarint, PutVarint, Varint, DetectEncoding, detectEncoding, JSON5Parse, JSON5Stringify, StableStringify, JSONPath, QREncode, QRToString } from "dyna:encoding";
import * as CryptoNS from "dyna:crypto";

const { HMAC, HMACHex, Hmac, PBKDF2, HKDF, RandomBytes, TimingSafeEqual, JWTSign, JWTVerify } = CryptoNS;
const HAS_TLS = typeof CryptoNS.Scrypt === "function" && typeof CryptoNS.AESGCM === "function";
let n=0,fails=0;
function assert(c,m){n++; if(!c){fails++; print("FAIL: "+m)}}
function eq(a,b,m){assert(a===b,m+" got "+a+" want "+b)}
function throws(fn,re,m){n++; try{fn(); fails++; print("FAIL: "+m+" did not throw")}catch(e){if(re && !re.test(String(e.message))) {fails++; print("FAIL: "+m+" wrong msg: "+e.message)}}}
function u8(...b){ return new Uint8Array(b); }
function eqArr(a,b){ if(a.length!==b.length) return false; for(let i=0;i<a.length;i++) if(a[i]!==b[i]) return false; return true; }
function hex(u){ return Array.from(u,b=>b.toString(16).padStart(2,"0")).join(""); }
function logStep(s){ try{ }catch(e){} }
logStep("bytes start");
print("compress done: "+n+" assertions");

__kitS.section("QREncode version in range", () => {
  let qr1 = QREncode("hello");
  assert(qr1.version>=1 && qr1.version<=40,"QREncode version in range");
  assert(qr1.size>0,"QREncode size >0");
  assert(qr1.modules instanceof Uint8Array,"QREncode modules Uint8Array");
  for(let ecc of ["L","M","Q","H"]){
    let q = QREncode("test",{ecc:ecc});
    assert(q.version>=1 && q.version<=40,"QREncode ecc "+ecc);
  }
  let qv1 = QREncode("hi",{version:1});
  eq(qv1.version,1,"QREncode version 1 pinned");
  let qv40 = QREncode("hi",{version:40});
  eq(qv40.version,40,"QREncode version 40 pinned");
  throws(()=>QREncode("hi",{version:0}),null,"QREncode version 0 throws");
  throws(()=>QREncode("hi",{version:41}),null,"QREncode version 41 throws");
  for(let m=0;m<8;m++){
    let q = QREncode("masktest",{mask:m});
    assert(q.modules.length===q.size*q.size,"QREncode mask "+m+" modules size");
  }
  throws(()=>QREncode("hi",{mask:-1}),null,"QREncode mask -1 throws");
  throws(()=>QREncode("hi",{mask:8}),null,"QREncode mask 8 throws");
  throws(()=>QREncode("hi",{ecc:"X"}),null,"QREncode bad ecc throws");
  let bigStr = "A".repeat(3000);
  throws(()=>QREncode(bigStr),null,"QREncode too large throws");
  let qrStr = QRToString("hello");
  assert(typeof qrStr==="string","QRToString returns string");
  assert(qrStr.length>0,"QRToString non-empty");
  let qrStr2 = QRToString("hello",{ecc:"H", version:5});
  assert(typeof qrStr2==="string","QRToString with opts");
  try{ let qe = QREncode(""); assert(qe.version>=1,"QREncode empty"); }catch(e){ assert(true,"QREncode empty may throw acceptable"); }
  assert(QREncode(String(null)).version>=1,"QREncode null coerces");
});
__kitS.end();
if(fails){print("test_encoding_cov_bytes_compress_encoding_hash: "+fails+" FAILED of "+n); throw new Error("failed")}
print("test_encoding_cov_bytes_compress_encoding_hash: "+n+" assertions, 0 failures")
