// covers: dyna:bytes.Text
// Split from tests/test_cov_bytes_compress_encoding_hash.js: the bytes sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_bytes_text_cov_bytes_compress_encoding_hash");
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

__kitS.section("Text value", () => {
  let t = new Text("hello");
  eq(t.value,"hello","Text value");
  eq(t.isWide,false,"Text ascii not wide");
  eq(new Text("héllo").isWide,false,"latin1 not wide");
  eq(new Text("héllo→").isWide,true,"wide true");
  eq(new Text("a\u0000b").isWide,false,"NUL not wide");
  eq(new Text("").isWide,false,"empty not wide");
  eq(new Text("").countUtf8(),0,"empty countUtf8");
  eq(new Text("abc").countUtf8(),3,"countUtf8 abc");
  eq(new Text("😀").countUtf8(),1,"countUtf8 emoji 1 codepoint");
  eq(new Text("😀🎉").countUtf16(),2,"countUtf16 emoji 2");
  eq(new Text("héllo").countUtf16(),5,"countUtf16 latin");
  assert(new Text("héllo").isValidUtf8===true,"Text valid utf8 (DT-8 getter)");
  assert(new Text("abc").isValidUtf16()===true,"valid utf16");
  let t2 = new Text("café");
  let utf8b = t2.toBytes().array;
  assert(utf8b instanceof Uint8Array,"toBytes().array is a Uint8Array");
  eq(utf8b.length,5,"café 5 bytes");
  eq(hex(utf8b),"636166c3a9","café as UTF-8");
  throws(()=>new Text("😀").utf8ToLatin1(),null,"utf8ToLatin1 throws on wide");
  let lat = new Bytes(u8(0xE9)).toString();
  let conv = latin1ToUtf8(u8(0xE9));
  assert(conv.length===2,"latin1 0xE9 -> 2 bytes utf8");
  let rev = utf8ToLatin1(conv);
  assert(rev[0]===0xE9,"roundtrip latin1");
  let u16 = utf8ToUtf16("hello");
  assert(u16.length===10,"utf8ToUtf16 hello 10 bytes (2 per char)");
  let u8back = utf16ToUtf8(u16);
  eq(decode(u8back,"utf-8"),"hello","utf16 roundtrip");
  throws(()=>utf16ToUtf8(u8(0x61)),null,"utf16 odd length throws");
  throws(()=>utf16ToUtf8(u8(0x00,0xD8)),null,"unpaired surrogate throws");
  let tb = new Text("hi").toBytes();
  assert(Bytes.isBytes(tb),"toBytes is Bytes");
  eq(tb.length,2,"toBytes length");
});
__kitS.end();
if(fails){print("test_bytes_cov_bytes_compress_encoding_hash: "+fails+" FAILED of "+n); throw new Error("failed")}
print("test_bytes_cov_bytes_compress_encoding_hash: "+n+" assertions, 0 failures")
