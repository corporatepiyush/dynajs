// covers: dyna:encoding.PutUvarint
// Split from tests/test_cov_bytes_compress_encoding_hash.js: the encoding sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_put_uvarint_cov_bytes_compress_encoding_hash");
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

__kitS.section("PutUvarint 0", () => {
  assert(eqArr(PutUvarint(0),u8(0)),"PutUvarint 0");
  assert(eqArr(PutUvarint(1),u8(1)),"PutUvarint 1");
  assert(eqArr(PutUvarint(127),u8(127)),"PutUvarint 127");
  assert(eqArr(PutUvarint(128),u8(128,1)),"PutUvarint 128");
  assert(eqArr(PutUvarint(300),u8(172,2)),"PutUvarint 300");
  assert(eqArr(PutUvarint(300),u8(0xAC,0x02)),"PutUvarint 300 alt");
  eq(hex(PutUvarint(300)),"ac02","PutUvarint 300 hex");
  assert(eqArr(PutUvarint(Number.MAX_SAFE_INTEGER),u8(255,255,255,255,255,255,255,15)),"PutUvarint MAX_SAFE");
  assert(eqArr(PutUvarint(9007199254740992n),u8(128,128,128,128,128,128,128,16)),"PutUvarint 2^53 bigint");
  assert(eqArr(PutUvarint(18446744073709551615n),u8(255,255,255,255,255,255,255,255,255,1)),"PutUvarint 2^64-1");
  for(let v of [0,1,127,128,300,16383,16384,2097151, 4294967296]){
    let enc=PutUvarint(v); let [dec,nb]=Uvarint(enc); eq(dec,v,"Uvarint roundtrip "+v); eq(nb,enc.length,"Uvarint nb "+v);
  }
  for(let v of [0n, 1n, 9007199254740992n, (1n<<63n), (1n<<64n)-1n]){
    let enc=PutUvarint(v); let [dec,nb]=Uvarint(enc); let db=typeof dec==="bigint"?dec:BigInt(dec); assert(db===v,"Uvarint bigint "+v); eq(nb,enc.length,"Uvarint bigint nb "+v);
  }
  { let [v,nb]=Uvarint(u8()); eq(v,0,"Uvarint empty v"); eq(nb,0,"Uvarint empty nb"); }
  { let [v,nb]=Uvarint(u8(0x80)); eq(v,0,"Uvarint trunc v"); eq(nb,0,"Uvarint trunc nb"); }
  { let over=new Uint8Array(11).fill(0x80); throws(()=>Uvarint(over),null,"Uvarint overflow throws"); }
  throws(()=>PutUvarint(-1),null,"PutUvarint -1 throws");
  throws(()=>PutUvarint(1.5),null,"PutUvarint 1.5 throws");
  throws(()=>PutUvarint(Number.MAX_SAFE_INTEGER+1),null,"PutUvarint >MAX_SAFE throws");
  assert(eqArr(PutVarint(0),u8(0)),"PutVarint 0");
  assert(eqArr(PutVarint(-1),u8(1)),"PutVarint -1");
  assert(eqArr(PutVarint(1),u8(2)),"PutVarint 1");
  assert(eqArr(PutVarint(-2),u8(3)),"PutVarint -2");
  assert(eqArr(PutVarint(2),u8(4)),"PutVarint 2");
  for(let v of [0,1,-1,2,-2,63,-64,64,-65,1000000,-1000000]){
    let enc=PutVarint(v); let [dec,nb]=Varint(enc); eq(dec,v,"Varint roundtrip "+v);
  }
  throws(()=>PutVarint(1.5),null,"PutVarint 1.5 throws");
  assert(eqArr(PutVarint(-9007199254740992n),u8(255,255,255,255,255,255,255,31)),"PutVarint -2^53");
  { let [v,nb]=Varint(u8(255,255,255,255,255,255,255,31)); assert(v===-9007199254740992n,"Varint -2^53 bigint"); }
});
__kitS.end();
if(fails){print("test_encoding_cov_bytes_compress_encoding_hash: "+fails+" FAILED of "+n); throw new Error("failed")}
print("test_encoding_cov_bytes_compress_encoding_hash: "+n+" assertions, 0 failures")
