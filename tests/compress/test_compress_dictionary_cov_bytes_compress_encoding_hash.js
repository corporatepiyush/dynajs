// covers: dyna:compress.Dictionary
// Split from tests/test_cov_bytes_compress_encoding_hash.js: the compress sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_dictionary_cov_bytes_compress_encoding_hash");
import { gzip, gunzip, zstd, unzstd, brotli, unbrotli, snappy, unsnappy, lz4Compress, lz4Decompress, lz4Frame, lz4Unframe, TarPack, TarList, TarExtract, ZipPack, ZipList, ZipRead, Compressor, Dictionary } from "dyna:compress";
import { ZipList as ZipListCov } from "dyna:compress";
import * as CryptoNS from "dyna:crypto";
const bytesOf = (v) => new Uint8Array(v.buffer, v.byteOffset, v.byteLength);

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
try{ let f=require("os"); }catch(e){}
print("bytes done: "+n+" assertions");

__kitS.section("Dictionary size 2", () => {
  let d1 = new Dictionary(["hello","world"]);
  eq(d1.size,2,"Dictionary size 2");
  assert(typeof d1.id==="number","Dictionary id number");
  let data = new TextEncoder().encode("hello world hello world");
  let comp = d1.compress(data);
  assert(comp instanceof Uint8Array,"Dictionary compress returns U8");
  assert(eqArr(d1.decompress(comp),data),"Dictionary roundtrip");
  d1.close(); assert(d1.closed,"Dictionary closed");
  d1.close();
  let dSingle = new Dictionary(["x"]);
  eq(dSingle.size,1,"Dictionary single");
  assert(eqArr(dSingle.decompress(dSingle.compress(new TextEncoder().encode("xxx"))), new TextEncoder().encode("xxx")),"single phrase roundtrip");
  dSingle.close();
  let manyPhrases = [];
  for(let i=0;i<20;i++) manyPhrases.push("phrase"+i);
  let dMany = new Dictionary(manyPhrases);
  eq(dMany.size,20,"Dictionary many 20");
  assert(eqArr(dMany.decompress(dMany.compress(new TextEncoder().encode("phrase5 phrase10"))), new TextEncoder().encode("phrase5 phrase10")),"many phrases roundtrip");
  dMany.close();
  let fifty = [];
  for(let i=0;i<50;i++) fifty.push("p"+i);
  let d50 = new Dictionary(fifty);
  eq(d50.size,50,"Dictionary 50");
  d50.close();
  throws(()=>new Dictionary([]),null,"Dictionary empty throws");
  throws(()=>new Dictionary([""]),null,"Dictionary empty phrase throws");
  throws(()=>new Dictionary("not array"),null,"Dictionary non-array throws");
  throws(()=>new Dictionary(null),null,"Dictionary null throws");
  let dTmp = new Dictionary(["a"]);
  dTmp.close();
  throws(()=>dTmp.compress(u8(1)),null,"Dictionary compress after close throws");
  throws(()=>dTmp.decompress(u8(1)),null,"Dictionary decompress after close throws");
  let d2 = new Dictionary(["hello"]);
  assert(eqArr(d2.decompress(d2.compress(new Uint8Array(0))), new Uint8Array(0)),"Dictionary empty roundtrip");
  d2.close();
});
__kitS.end();
if(fails){print("test_compress_cov_bytes_compress_encoding_hash: "+fails+" FAILED of "+n); throw new Error("failed")}
print("test_compress_cov_bytes_compress_encoding_hash: "+n+" assertions, 0 failures")
