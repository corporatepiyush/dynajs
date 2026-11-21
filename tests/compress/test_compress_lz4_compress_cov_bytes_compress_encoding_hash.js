// covers: dyna:compress.lz4Compress
// Split from tests/test_cov_bytes_compress_encoding_hash.js: the compress sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_lz4_compress_cov_bytes_compress_encoding_hash");
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

__kitS.section("hello hello hello hello hello hello hello hello", () => {
  let data = new TextEncoder().encode("hello hello hello hello hello hello hello hello ".repeat(20));
  let empty = new Uint8Array(0);
  assert(lz4Decompress(lz4Compress(empty)).length===0,"lz4 empty");
  let one = u8(42);
  assert(eqArr(lz4Decompress(lz4Compress(one)),one),"lz4 1byte");
  assert(eqArr(lz4Decompress(lz4Compress(data)),data),"lz4 1KB");
  let big = new Uint8Array(64*1024); for(let i=0;i<big.length;i++) big[i]= (i*7)&0xFF;
  assert(eqArr(lz4Decompress(lz4Compress(big)),big),"lz4 64KB");
  assert(eqArr(lz4Decompress(lz4Compress(data,{level:1})),data),"lz4 level 1");
  assert(eqArr(lz4Decompress(lz4Compress(data,{level:12})),data),"lz4 level 12");
  assert(lz4Compress(data,{level:0}).length > 0, "lz4 level 0 clamps into range (d.ts: values outside the range are clamped by the codec)");
  assert(lz4Compress(data,{level:13}).length > 0, "lz4 level 13 clamps into range (d.ts: values outside the range are clamped by the codec)");
  assert(lz4Compress(data,{level:22}).length > 0, "lz4 level 22 clamps into range (d.ts: values outside the range are clamped by the codec)");
  let dict = new TextEncoder().encode("hello ");
  let compDict = lz4Compress(data,{dict:dict, level:3});
  assert(eqArr(lz4Decompress(compDict,{dict:dict}),data),"lz4 dict roundtrip");
  try{ let bad = lz4Decompress(compDict); assert(!eqArr(bad,data),"lz4 without dict should differ"); }catch(e){ assert(true,"lz4 dict mismatch throws ok"); }
  let wrongDict = new TextEncoder().encode("world ");
  try{ let bad2 = lz4Decompress(compDict,{dict:wrongDict}); assert(!eqArr(bad2,data),"wrong dict differs"); }catch(e){ assert(true,"wrong dict throws ok"); }

  for(let sz of [0,1,2,63,64,65,1023,1024,1025]){
    let d=new Uint8Array(sz); for(let i=0;i<sz;i++) d[i]= (i*5)&0xFF;
    assert(eqArr(lz4Decompress(lz4Compress(d)),d),"lz4 size "+sz);
  }
  assert(eqArr(lz4Decompress(lz4Compress("hello lz4")), new TextEncoder().encode("hello lz4")),"lz4 string");

  throws(()=>lz4Decompress(u8(1,2,3)),null,"lz4Decompress truncated throws");
  throws(()=>lz4Compress(null),null,"lz4Compress null throws");
});
__kitS.end();
if(fails){print("test_compress_cov_bytes_compress_encoding_hash: "+fails+" FAILED of "+n); throw new Error("failed")}
print("test_compress_cov_bytes_compress_encoding_hash: "+n+" assertions, 0 failures")
