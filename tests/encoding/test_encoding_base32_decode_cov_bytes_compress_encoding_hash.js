// covers: dyna:encoding.Base32Decode
// Split from tests/test_cov_bytes_compress_encoding_hash.js: the encoding sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_base32_decode_cov_bytes_compress_encoding_hash");
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

__kitS.section("Base32 empty", () => {
  eq(Base32Encode(new Uint8Array(0)),"","Base32 empty");
  eq(Base32Decode("").length,0,"Base32 decode empty");
  eq(Base32Encode("f"),"MY======","Base32 f");
  eq(Base32Encode("fo"),"MZXQ====","Base32 fo");
  eq(Base32Encode("foobar"),"MZXW6YTBOI======","Base32 foobar");
  assert(eqArr(Base32Decode("MY======"),u8(0x66)),"Base32 decode f");
  eq(Base32HexEncode(new Uint8Array(0)),"","Base32Hex empty");
  eq(Base32HexDecode("").length,0,"Base32Hex decode empty");
  eq(Base32HexEncode("f"),"CO======","Base32Hex f");
  for(let len of [0,1,2,3,4,5,6,7,8]){
    let b=new Uint8Array(len); for(let i=0;i<len;i++) b[i]=i;
    assert(eqArr(Base32Decode(Base32Encode(b)),b),"Base32 roundtrip len "+len);
    assert(eqArr(Base32HexDecode(Base32HexEncode(b)),b),"Base32Hex roundtrip len "+len);
  }
  throws(()=>Base32Decode("my======"),null,"Base32 lowercase throws");
  throws(()=>Base32Decode("0Y======"),null,"Base32 digit 0 invalid");
  throws(()=>Base32HexDecode("WO======"),null,"Base32Hex W invalid");
  throws(()=>Base32Decode("MY====="),null,"Base32 bad length throws");
  throws(()=>Base32Decode("MY======MZXW6YTB"),null,"Base32 padding before final throws");
});
__kitS.end();
if(fails){print("test_encoding_cov_bytes_compress_encoding_hash: "+fails+" FAILED of "+n); throw new Error("failed")}
print("test_encoding_cov_bytes_compress_encoding_hash: "+n+" assertions, 0 failures")
