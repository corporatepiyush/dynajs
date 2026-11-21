// covers: dyna:encoding.HexEncode
// Split from tests/test_cov_bytes_compress_encoding_hash.js: the encoding sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_hex_encode_cov_bytes_compress_encoding_hash");
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

__kitS.section("Hex empty", () => {
  eq(HexEncode(new Uint8Array(0)),"","Hex empty");
  eq(HexEncode(u8(0xDE,0xAD)),"dead","Hex dead");
  eq(HexEncode("abc"),"616263","Hex abc string");
  eq(HexEncode(u8(0x00,0xFF)),"00ff","Hex 00ff");
  assert(eqArr(HexDecode(""),[]),"HexDecode empty");
  assert(eqArr(HexDecode("dead"),u8(0xDE,0xAD)),"HexDecode dead");
  assert(eqArr(HexDecode("DEAD"),u8(0xDE,0xAD)),"HexDecode upper");
  assert(eqArr(HexDecode(HexEncode(u8(1,2,3))),u8(1,2,3)),"Hex roundtrip");
  throws(()=>HexDecode("abc"),null,"Hex odd length throws");
  throws(()=>HexDecode("zz"),null,"Hex invalid digit throws");
  throws(()=>HexDecode("0g"),null,"Hex invalid 2nd throws");
  throws(()=>HexDecode("g0"),null,"Hex invalid 1st throws");
  throws(()=>HexEncode(42),null,"HexEncode number throws");
  throws(()=>HexEncode(null),null,"HexEncode null throws");
  throws(()=>HexEncode([1,2]),null,"HexEncode array throws");
  throws(()=>HexEncode(new Uint16Array([1])),null,"HexEncode Uint16 throws");
  let big = new Uint8Array(1024); for(let i=0;i<1024;i++) big[i]=i&0xFF;
  assert(eqArr(HexDecode(HexEncode(big)),big),"Hex large roundtrip");
});
__kitS.end();
if(fails){print("test_encoding_cov_bytes_compress_encoding_hash: "+fails+" FAILED of "+n); throw new Error("failed")}
print("test_encoding_cov_bytes_compress_encoding_hash: "+n+" assertions, 0 failures")
