// covers: dyna:encoding.JSONPath
// Split from tests/test_cov_bytes_compress_encoding_hash.js: the encoding sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_json_path_cov_bytes_compress_encoding_hash");
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

__kitS.section("Nigel Rees", () => {
  let jp = new JSONPath("$.store.book[*].author");
  let data = {store:{book:[{author:"Nigel Rees"},{author:"Evelyn Waugh"}]}};
  eq(jp.all(data).length,2,"JSONPath all length");
  eq(jp.first(data),"Nigel Rees","JSONPath first");
  eq(jp.paths(data).length,2,"JSONPath paths length");
  assert(jp.paths(data)[0].includes("book"),"JSONPath path contains book");
  let jp2 = new JSONPath("$.missing");
  eq(jp2.all(data).length,0,"JSONPath no match all empty");
  eq(jp2.first(data),undefined,"JSONPath no match first undefined");
  eq(jp2.paths(data).length,0,"JSONPath no match paths empty");
  let jp3 = new JSONPath("$..author");
  eq(jp3.all(data).length,2,"JSONPath deep all");
  let jpRoot = new JSONPath("$");
  assert(jpRoot.first(data)===data,"JSONPath root");
  throws(()=>new JSONPath(""),null,"JSONPath empty throws");
  throws(()=>new JSONPath("not jsonpath"),null,"JSONPath invalid throws");
  let jpArr = new JSONPath("$[0]");
  eq(jpArr.first([10,20]),10,"JSONPath array index");
});
__kitS.end();
if(fails){print("test_encoding_cov_bytes_compress_encoding_hash: "+fails+" FAILED of "+n); throw new Error("failed")}
print("test_encoding_cov_bytes_compress_encoding_hash: "+n+" assertions, 0 failures")
