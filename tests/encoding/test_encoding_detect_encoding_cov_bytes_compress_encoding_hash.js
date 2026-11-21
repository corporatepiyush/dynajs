// covers: dyna:encoding.DetectEncoding
// Split from tests/test_cov_bytes_compress_encoding_hash.js: the encoding sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_detect_encoding_cov_bytes_compress_encoding_hash");
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

__kitS.section("Detect BOM utf-8", () => {
  eq(DetectEncoding(u8(0xEF,0xBB,0xBF,0x61)),"utf-8","Detect BOM utf-8");
  eq(DetectEncoding(u8(0xFE,0xFF,0x00,0x61)),"utf-16be","Detect BOM utf-16be");
  eq(DetectEncoding(u8(0xFF,0xFE,0x61,0x00)),"utf-16le","Detect BOM utf-16le");
  eq(detectEncoding(u8(0x61,0x62)),"utf-8","detectEncoding lowercase alias");
  let gbk = u8(0xD6,0xD0,0xCE,0xC4,0xB1,0xE0,0xC2,0xEB,0xB2,0xE2,0xCA,0xD4,0x20,0x31,0x32,0x33);
  eq(DetectEncoding(gbk,{allowList:["gbk","utf-8"]}),"gbk","Detect allowList hit");
  throws(()=>DetectEncoding(gbk,{allowList:["shift_jis"]}),null,"Detect allowList rejection throws");
  eq(DetectEncoding(gbk,{allowList:["utf-8","shift_jis"], fallback:"utf-8"}),"utf-8","Detect fallback");
  eq(DetectEncoding(u8(0x61,0x62),{fallback:"windows-1252"}),"utf-8","Detect ascii is utf-8");
  throws(()=>DetectEncoding("not a buffer"),null,"Detect non-buffer throws");
  throws(()=>DetectEncoding(null),null,"Detect null throws");
  throws(()=>DetectEncoding(u8(0x61),{allowList:["utf-8", 123]}),/allowList\[1\] must be a string/,"Detect allowList refuses a non-string element (a lenient skip would walk every hole of a sparse list)");
});
__kitS.end();
if(fails){print("test_encoding_cov_bytes_compress_encoding_hash: "+fails+" FAILED of "+n); throw new Error("failed")}
print("test_encoding_cov_bytes_compress_encoding_hash: "+n+" assertions, 0 failures")
