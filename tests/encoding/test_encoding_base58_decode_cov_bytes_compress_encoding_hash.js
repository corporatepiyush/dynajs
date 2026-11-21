// covers: dyna:encoding.Base58Decode
// Split from tests/test_cov_bytes_compress_encoding_hash.js: the encoding sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_base58_decode_cov_bytes_compress_encoding_hash");
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

__kitS.section("Base58 empty", () => {
  eq(Base58Encode(new Uint8Array(0)),"","Base58 empty");
  eq(Base58Decode("").length,0,"Base58 decode empty");
  eq(Base58Encode(u8(0,0,0)),"111","Base58 leading zeros");
  assert(eqArr(Base58Decode("111"),u8(0,0,0)),"Base58 decode leading ones");
  eq(Base58Encode(u8(0)),"1","Base58 single zero");
  assert(eqArr(Base58Decode("1"),u8(0)),"Base58 decode single 1");
  assert(eqArr(Base58Decode(Base58Encode(u8(0,0,1,2,3))),u8(0,0,1,2,3)),"Base58 roundtrip with leading zeros");
  for(let len of [0,1,2,3,5,10,20]){
    let b=new Uint8Array(len); for(let i=0;i<len;i++) b[i]=(i*13+7)&0xFF;
    assert(eqArr(Base58Decode(Base58Encode(b)),b),"Base58 roundtrip len "+len);
  }
  throws(()=>Base58Decode("0"),null,"Base58 invalid char 0 throws");
  throws(()=>Base58Decode("I"),null,"Base58 invalid char I throws");
  throws(()=>Base58Decode("O"),null,"Base58 invalid char O throws");
  let chkData = u8(1,2,3,4);
  let chkEnc = Base58CheckEncode(chkData);
  assert(chkEnc.length>0,"Base58Check encode non-empty");
  assert(eqArr(Base58CheckDecode(chkEnc),chkData),"Base58Check roundtrip");
  eq(Base58CheckEncode(new Uint8Array(0)), Base58CheckEncode(new Uint8Array(0)), "Base58Check empty deterministic");
  assert(eqArr(Base58CheckDecode(Base58CheckEncode(new Uint8Array(0))), new Uint8Array(0)),"Base58Check empty roundtrip");
  assert(eqArr(Base58CheckDecode(Base58CheckEncode(u8(0,0,0))),u8(0,0,0)),"Base58Check leading zeros roundtrip");
  throws(()=>Base58CheckDecode("111"),null,"Base58Check too short throws");
  {
    let bad = chkEnc.slice(0,-1) + (chkEnc[chkEnc.length-1]==="1" ? "2":"1");
    throws(()=>Base58CheckDecode(bad),null,"Base58Check bad checksum throws");
  }
  throws(()=>Base58CheckDecode("!@#"),null,"Base58Check invalid chars throws");
  throws(()=>Base58Encode(null),null,"Base58Encode null throws");
});
__kitS.end();
if(fails){print("test_encoding_cov_bytes_compress_encoding_hash: "+fails+" FAILED of "+n); throw new Error("failed")}
print("test_encoding_cov_bytes_compress_encoding_hash: "+n+" assertions, 0 failures")
