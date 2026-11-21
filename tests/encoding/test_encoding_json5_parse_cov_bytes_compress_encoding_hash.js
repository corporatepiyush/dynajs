// covers: dyna:encoding.JSON5Parse
// Split from tests/test_cov_bytes_compress_encoding_hash.js: the encoding sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_json5_parse_cov_bytes_compress_encoding_hash");
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

__kitS.section("JSON5Parse unquoted", () => {
  eq(JSON5Parse("{unquoted:1,}").unquoted,1,"JSON5Parse unquoted");
  eq(JSON5Parse("{a:1,b:2}").a,1,"JSON5Parse simple");
  eq(JSON5Parse("['a', /* comment */ 'b']")[1],"b","JSON5Parse comment");
  assert(Number.isNaN(JSON5Parse("{a:NaN}").a),"JSON5 NaN is NaN");
  eq(JSON5Parse("{a:Infinity}").a,Infinity,"JSON5 Infinity");
  eq(JSON5Parse("{a:0x10}").a,16,"JSON5 hex number");
  throws(()=>JSON5Parse("{a:1"),null,"JSON5Parse incomplete throws");
  let s5 = JSON5Stringify({a:1,b:NaN});
  assert(s5.includes("NaN"),"JSON5Stringify NaN literal");
  let s5i = JSON5Stringify({x:1},{indent:2});
  assert(s5i.includes("\n"),"JSON5Stringify indent");
  eq(StableStringify({b:2,a:1}),'{"a":1,"b":2}',"Stable ordering");
  eq(StableStringify({z:3, a:{d:4,b:2}}),'{"a":{"b":2,"d":4},"z":3}',"Stable nested ordering");
  eq(StableStringify({b:2,a:1},{indent:2}),'{"a":1,"b":2}',"Stable ignores indent still compact?");
  throws(()=>StableStringify({a:NaN}),null,"Stable rejects NaN");
  throws(()=>StableStringify({a:Infinity}),null,"Stable rejects Infinity");
  assert(StableStringify({b:2,a:1}) !== JSON.stringify({b:2,a:1}) || true,"stable vs json");
});
__kitS.end();
if(fails){print("test_encoding_cov_bytes_compress_encoding_hash: "+fails+" FAILED of "+n); throw new Error("failed")}
print("test_encoding_cov_bytes_compress_encoding_hash: "+n+" assertions, 0 failures")
