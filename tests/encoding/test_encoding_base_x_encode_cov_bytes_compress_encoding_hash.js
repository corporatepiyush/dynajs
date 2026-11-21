// covers: dyna:encoding.BaseXEncode
// Split from tests/test_cov_bytes_compress_encoding_hash.js: the encoding sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_base_x_encode_cov_bytes_compress_encoding_hash");
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

__kitS.section("BaseX empty", () => {
  eq(BaseXEncode(new Uint8Array(0),"01"),"","BaseX empty");
  eq(BaseXDecode("", "01").length,0,"BaseX decode empty");
  let b = u8(0xFF,0x00);
  let enc2 = BaseXEncode(b,"01");
  assert(enc2.length>0,"BaseX binary enc length");
  assert(eqArr(BaseXDecode(enc2,"01"),b),"BaseX binary roundtrip");
  assert(eqArr(BaseXDecode(BaseXEncode(u8(0),"01"),"01"),u8(0)),"BaseX 0 roundtrip binary alphabet");
  let alpha62 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  assert(eqArr(BaseXDecode(BaseXEncode(u8(1,2,3),alpha62),alpha62),u8(1,2,3)),"BaseX 62 roundtrip");
  let alphaLarge = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  assert(alphaLarge.length===62,"alphaLarge len");
  let encLarge = BaseXEncode(u8(10,20,30),alphaLarge);
  assert(eqArr(BaseXDecode(encLarge,alphaLarge),u8(10,20,30)),"BaseX large 62 roundtrip");
  throws(()=>BaseXEncode(u8(1),"a"),null,"BaseX alphabet too short throws");
  throws(()=>BaseXEncode(u8(1),"0123456789ABCDEF0123456789ABCDEF0"),null,"BaseX duplicate throws");
  throws(()=>BaseXEncode(u8(1),""),null,"BaseX empty alphabet throws");
  throws(()=>BaseXDecode("hello","01"),null,"BaseX decode char outside alphabet throws");
  throws(()=>BaseXEncode(null,"01"),null,"BaseXEncode null throws");
  throws(()=>BaseXDecode(null,"01"),null,"BaseXDecode null throws");
  let alpha256 = "";
  for(let i=0;i<256;i++) alpha256+=String.fromCharCode(33+i%94);
  let alpha256uniq = "";
  for(let i=0;i<256;i++) alpha256uniq+=String.fromCharCode(i);
  throws(()=>BaseXEncode(u8(1),alpha256uniq),null,"BaseX alphabet 256 too long throws");
});
__kitS.end();
if(fails){print("test_encoding_cov_bytes_compress_encoding_hash: "+fails+" FAILED of "+n); throw new Error("failed")}
print("test_encoding_cov_bytes_compress_encoding_hash: "+n+" assertions, 0 failures")
