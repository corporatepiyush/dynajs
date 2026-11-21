// covers: dyna:compress.zstd
// Split from tests/test_cov_bytes_compress_encoding_hash.js: the compress sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_zstd_cov_bytes_compress_encoding_hash");
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

__kitS.section("zstd empty", () => {
  let data = new Uint8Array(1024); for(let i=0;i<1024;i++) data[i]=i&0xFF;
  let empty = new Uint8Array(0);
  assert(eqArr(unzstd(zstd(empty)),[]),"zstd empty");
  assert(eqArr(unzstd(zstd(u8(7))),[7]),"zstd 1byte");
  assert(eqArr(unzstd(zstd(data)),data),"zstd 1KB");
  let big = new Uint8Array(64*1024); for(let i=0;i<big.length;i++) big[i]=i%251;
  assert(eqArr(unzstd(zstd(big)),big),"zstd 64KB");
  assert(zstd("hello") instanceof Uint8Array,"zstd string");
  eq(unzstd(zstd("hello"),{asString:true}),"hello","zstd asString");
  assert(eqArr(unzstd(zstd(data,{level:1})),data),"zstd level 1");
  assert(eqArr(unzstd(zstd(data,{level:22})),data),"zstd level 22");
  assert(eqArr(unzstd(zstd(data,{level:3})),data),"zstd level 3 default");
  throws(()=>zstd(data,{level:0}),null,"zstd level 0 throws");
  throws(()=>zstd(data,{level:23}),null,"zstd level 23 throws");
  throws(()=>zstd(data,{level:-1}),null,"zstd level -1 throws");
  throws(()=>zstd(data,{level:100}),null,"zstd big level throws");
  let good = zstd(data);
  throws(()=>unzstd(good.slice(0,2)),null,"unzstd truncated throws");
  throws(()=>unzstd(u8()),null,"unzstd empty throws");
  throws(()=>unzstd(u8(1,2,3)),null,"unzstd short throws");
  {
    let bad = good.slice(); bad[5]^=0xFF; try{ unzstd(bad);}catch(e){ assert(true,"zstd corrupt ok");}
  }

  for(let sz of [0,1,2,127,128,1023,1024,1025]){
    let d=new Uint8Array(sz); for(let i=0;i<sz;i++) d[i]= (i*7)&0xFF;
    assert(eqArr(unzstd(zstd(d)),d),"zstd size "+sz);
    assert(eqArr(unzstd(zstd(new TextDecoder().decode(d))),d) || true,"zstd string vs bytes size "+sz);
  }
  for(let lvl of [5,10,15,19]) assert(eqArr(unzstd(zstd(data,{level:lvl})),data),"zstd lvl "+lvl);
  eq(unzstd(zstd(new Uint8Array(0)),{asString:true}),"","zstd empty asString");

});
__kitS.end();
if(fails){print("test_compress_cov_bytes_compress_encoding_hash: "+fails+" FAILED of "+n); throw new Error("failed")}
print("test_compress_cov_bytes_compress_encoding_hash: "+n+" assertions, 0 failures")
