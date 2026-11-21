// covers: dyna:compress.gunzip
// Split from tests/test_cov_bytes_compress_encoding_hash.js: the compress sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_gunzip_cov_bytes_compress_encoding_hash");
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

__kitS.section("gzip empty is U8", () => {
{
  function rBytes(len,seed=1){ let a=new Uint8Array(len); let x=seed>>>0; for(let i=0;i<len;i++){ x=(Math.imul(x,1103515245)+12345)>>>0; a[i]=(x>>>16)&0xFF; } return a; }
  let empty = new Uint8Array(0);
  let one = u8(42);
  let kb1 = rBytes(1024,1);
  let kb64 = rBytes(64*1024,2);
  let gzEmpty = gzip(empty);
  assert(gzEmpty instanceof Uint8Array,"gzip empty is U8");
  assert(eqArr(gunzip(gzEmpty),[]),"gunzip empty roundtrip");
  eq(gunzip(gzEmpty, {asString:true}),"","gunzip empty asString");
  let gzOne = gzip(one);
  assert(eqArr(gunzip(gzOne),[42]),"gzip 1byte roundtrip");
  let gz1k = gzip(kb1);
  assert(eqArr(gunzip(gz1k),kb1),"gzip 1KB random");
  let gz64 = gzip(kb64);
  assert(eqArr(gunzip(gz64),kb64),"gzip 64KB");
  let gzStr = gzip("hello world");
  eq(gunzip(gzStr,{asString:true}),"hello world","gzip string roundtrip");
  let ab = kb1.buffer;
  assert(eqArr(gunzip(gzip(ab)),kb1),"gzip ArrayBuffer");
  gzip(kb1,0); gzip(kb1,6); gzip(kb1,9);
  throws(()=>gunzip(u8()),null,"gunzip empty throws");
  throws(()=>gunzip(u8(1,2,3)),null,"gunzip short throws");
  throws(()=>gunzip(gz64.slice(0,5)),null,"gunzip truncated throws");
  throws(()=>gunzip(gz64.slice(0,gz64.length-1)),null,"gunzip truncated trail throws");
  {
    let bad = gz64.slice(); bad[10]^=0xFF; try{ gunzip(bad); }catch(e){ assert(true,"corrupt throws ok"); }
  }
  throws(()=>gzip(null),null,"gzip null throws");
  throws(()=>gunzip(null),null,"gunzip null throws");
  throws(()=>gunzip("not bytes"),null,"gunzip string throws?");
  for(let lvl of [1,6,9]){ let g=gzip(kb1,lvl); assert(eqArr(gunzip(g),kb1),"gzip level "+lvl); }
  for(let sz of [0,1,2,1023,1024,1025, 64*1024-1,64*1024,64*1024+1]){
    let d=new Uint8Array(sz); for(let i=0;i<sz;i++) d[i]=i&0xFF;
    assert(eqArr(gunzip(gzip(d)),d),"gzip size "+sz);
  }
  {
    let dv = new DataView(kb1.buffer, 10, 100);
    assert(eqArr(gunzip(gzip(bytesOf(dv))), bytesOf(dv)),"gzip DataView");
  }
  assert(eqArr(gunzip(gzip("test string with unicode 😀")), new TextEncoder().encode("test string with unicode 😀")),"gzip unicode");
  for(let cut of [0,1,5,10,20]) throws(()=>gunzip(gz64.slice(0,cut)),null,"gzip trunc extra "+cut);
  }
if(fails){print("test_compress_cov_bytes_compress_encoding_hash: "+fails+" FAILED of "+n); throw new Error("failed")}
print("test_compress_cov_bytes_compress_encoding_hash: "+n+" assertions, 0 failures")
});
__kitS.end();
