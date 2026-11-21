// covers: dyna:encoding.Base64Decode
// Split from tests/test_cov_bytes_compress_encoding_hash.js: the encoding sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_base64_decode_cov_bytes_compress_encoding_hash");
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

__kitS.section("Base64 empty", () => {
  eq(Base64Encode(new Uint8Array(0)),"","Base64 empty");
  eq(Base64Encode(u8(0x68,0x69)),"aGk=","Base64 hi");
  assert(eqArr(Base64Decode("aGk="),u8(0x68,0x69)),"Base64 decode hi");
  eq(Base64Encode("f"),"Zg==","Base64 f string");
  eq(Base64Encode(u8(0)),"AA==","Base64 1byte pad ==");
  eq(Base64Encode(u8(0,0)),"AAA=","Base64 2bytes pad =");
  eq(Base64Encode(u8(0,0,0)),"AAAA","Base64 3bytes no pad");
  assert(eqArr(Base64Decode("AA=="),u8(0)),"Base64 decode 1byte");
  assert(eqArr(Base64Decode("AAA="),u8(0,0)),"Base64 decode 2bytes");
  throws(()=>Base64Decode("a"),null,"Base64 bad length throws");
  throws(()=>Base64Decode("!!!!"),null,"Base64 invalid chars throws");
  throws(()=>Base64Decode("Z==="),null,"Base64 misplaced pad throws");
  throws(()=>Base64Decode("aGk=\n"),null,"Base64 newline throws");
  eq(Base64URLEncode(new Uint8Array(0)),"","Base64URL empty");
  eq(Base64URLEncode(u8(0xF8)),"-A","Base64URL 0xF8 dash");
  eq(Base64URLEncode(u8(0xFC)),"_A","Base64URL 0xFC underscore");
  assert(eqArr(Base64URLDecode("-A"),u8(0xF8)),"Base64URL decode dash");
  assert(eqArr(Base64URLDecode("_A"),u8(0xFC)),"Base64URL decode underscore");
  throws(()=>Base64URLDecode("+A"),null,"Base64URL rejects +");
  throws(()=>Base64URLDecode("/A"),null,"Base64URL rejects /");
  assert(eqArr(Base64URLDecode("Zg=="),u8(0x66)),"Base64URL tolerates ==");
  for(let len of [0,1,2,3,4,5,16,64]){
    let b=new Uint8Array(len); for(let i=0;i<len;i++) b[i]=(i*37)&0xFF;
    assert(eqArr(Base64Decode(Base64Encode(b)),b),"Base64 roundtrip len "+len);
    assert(eqArr(Base64URLDecode(Base64URLEncode(b)),b),"Base64URL roundtrip len "+len);
  }
  throws(()=>Base64URLDecode("**"),null,"Base64URL invalid char throws");
  throws(()=>Base64Encode(null),null,"Base64Encode null throws");
  assert(Base64Decode(null) instanceof Uint8Array,"Base64Decode null returns bytes (lenient)");
});
__kitS.end();
if(fails){print("test_encoding_cov_bytes_compress_encoding_hash: "+fails+" FAILED of "+n); throw new Error("failed")}
print("test_encoding_cov_bytes_compress_encoding_hash: "+n+" assertions, 0 failures")
