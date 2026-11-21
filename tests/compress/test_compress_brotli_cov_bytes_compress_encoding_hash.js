// covers: dyna:compress.brotli
// Split from tests/test_cov_bytes_compress_encoding_hash.js: the compress sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_brotli_cov_bytes_compress_encoding_hash");
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

__kitS.section("brotli empty", () => {
  let data = new Uint8Array(1024); for(let i=0;i<1024;i++) data[i]= (i*3)&0xFF;
  assert(eqArr(unbrotli(brotli(new Uint8Array(0))),[]),"brotli empty");
  assert(eqArr(unbrotli(brotli(u8(99))),[99]),"brotli 1byte");
  assert(eqArr(unbrotli(brotli(data)),data),"brotli 1KB");
  let big = new Uint8Array(64*1024); for(let i=0;i<big.length;i++) big[i]=i%251;
  assert(eqArr(unbrotli(brotli(big)),big),"brotli 64KB");
  eq(unbrotli(brotli("hello"),{asString:true}),"hello","brotli asString");
  assert(eqArr(unbrotli(brotli(data,{level:0})),data),"brotli level 0");
  assert(eqArr(unbrotli(brotli(data,{level:11})),data),"brotli level 11");
  assert(eqArr(unbrotli(brotli(data,{level:5})),data),"brotli level 5");
  throws(()=>brotli(data,{level:-1}),null,"brotli level -1 throws");
  throws(()=>brotli(data,{level:12}),null,"brotli level 12 throws");
  throws(()=>brotli(data,{level:22}),null,"brotli level 22 throws");
  let good = brotli(data);
  throws(()=>unbrotli(good.slice(0,2)),null,"unbrotli truncated throws");
  throws(()=>unbrotli(u8()),null,"unbrotli empty throws");
  {
    let bad = good.slice(); bad[2]^=0xFF; try{ unbrotli(bad);}catch(e){ assert(true,"brotli corrupt ok");}
  }
});
__kitS.end();

for(let sz of [0,1,2,127,128,1023,1024,2048]){
    let d=new Uint8Array(sz); for(let i=0;i<sz;i++) d[i]= (i*11)&0xFF;
    assert(eqArr(unbrotli(brotli(d)),d),"brotli size "+sz);
  }

{
    let d2 = new Uint8Array(1024); for(let i=0;i<1024;i++) d2[i]= (i*3)&0xFF;
    for(let lvl of [1,3,7,9]) assert(eqArr(unbrotli(brotli(d2,{level:lvl})),d2),"brotli lvl "+lvl);
    let dv2 = new DataView(d2.buffer, 5, 100);
    assert(eqArr(unbrotli(brotli(bytesOf(dv2))), bytesOf(dv2)),"brotli DataView");
  }
if(fails){print("test_compress_cov_bytes_compress_encoding_hash: "+fails+" FAILED of "+n); throw new Error("failed")}
print("test_compress_cov_bytes_compress_encoding_hash: "+n+" assertions, 0 failures")
