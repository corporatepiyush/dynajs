// covers: dyna:bytes.compare
// Split from tests/test_cov_bytes_compress_encoding_hash.js: the bytes sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_bytes_compare_cov_bytes_compress_encoding_hash");
import { Bytes, Text, compare, equal, indexOf, lastIndexOf, contains, count, concat, copy, isValidUtf8, isValidUtf16, countUtf8, countUtf16, latin1ToUtf8, utf8ToLatin1, utf8ToUtf16, utf16ToUtf8, decode, encode, encodingExists, encodings } from "dyna:bytes";
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

__kitS.section("free indexOf byte", () => {
  let a = u8(1,2,3,2,3,4);
  eq(indexOf(a,2),1,"free indexOf byte");
  eq(lastIndexOf(a,3),4,"free lastIndexOf byte");
  assert(contains(a,2)===true,"free contains true");
  assert(contains(a,9)===false,"free contains false");
  eq(count(a,u8(2,3)),2,"free count view");
  assert(equal(u8(1,2),u8(1,2)),"free equal true");
  assert(!equal(u8(1,2),u8(1,3)),"free equal false");
  eq(compare(u8(1,2),u8(1,3)),-1,"free compare less");
  eq(compare(u8(1,3),u8(1,2)),1,"free compare greater");
  assert(eqArr(concat([u8(1,2),u8(3,4)]),[1,2,3,4]),"free concat");
  let dst = new Uint8Array(4);
  eq(copy(dst,u8(9,8),1,0,2),2,"free copy count");
  eq(dst[1],9,"copy dstOff");
  assert(isValidUtf8("hello")===true,"isValidUtf8 string true");
  assert(isValidUtf8(u8(0xFF))===false,"isValidUtf8 false");
  assert(isValidUtf16(u8(0x61,0x00))===true,"isValidUtf16 true");
  assert(isValidUtf16(u8(0x00))===false,"isValidUtf16 odd false");
  eq(countUtf8("héllo"),5,"free countUtf8");
  eq(countUtf16(u8(0x61,0x00,0x62,0x00)),2,"free countUtf16");
  assert(encodingExists("utf-8")===true,"encodingExists utf-8");
  assert(encodingExists("UTF8")===true,"case insensitive");
  assert(!encodingExists("nonexistent-xyz"),"encodingExists false");
  let encList = encodings();
  assert(Array.isArray(encList),"encodings array");
  assert(encList[0]==="utf-8","encodings first utf-8");
  assert(encList.length>1,"encodings >1");
  eq(decode(u8(97,98,99),"utf-8"),"abc","decode utf-8");
  assert(encode("abc","utf-8") instanceof Uint8Array,"encode utf-8");
  throws(()=>decode(u8(1), "nope-charset"),null,"decode unknown charset");
  throws(()=>compare("abc","abd"),null,"compare strings throws");
});
__kitS.end();
if(fails){print("test_bytes_cov_bytes_compress_encoding_hash: "+fails+" FAILED of "+n); throw new Error("failed")}
print("test_bytes_cov_bytes_compress_encoding_hash: "+n+" assertions, 0 failures")
