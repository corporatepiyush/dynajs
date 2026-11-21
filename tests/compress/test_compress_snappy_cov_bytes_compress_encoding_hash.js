// covers: dyna:compress.snappy
// Split from tests/test_cov_bytes_compress_encoding_hash.js: the compress sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_snappy_cov_bytes_compress_encoding_hash");
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

__kitS.section("snappy empty", () => {
  let data = new Uint8Array(1024); for(let i=0;i<1024;i++) data[i]=i&0xFF;
  assert(eqArr(unsnappy(snappy(new Uint8Array(0))),[]),"snappy empty");
  assert(eqArr(unsnappy(snappy(u8(1))),[1]),"snappy 1byte");
  assert(eqArr(unsnappy(snappy(data)),data),"snappy 1KB");
  let big = new Uint8Array(64*1024); for(let i=0;i<big.length;i++) big[i]=i%251;
  assert(eqArr(unsnappy(snappy(big)),big),"snappy 64KB");
  eq(unsnappy(snappy("hello"),{asString:true}),"hello","snappy asString");
  let good = snappy(data);
  throws(()=>unsnappy(good.slice(0,2)),null,"unsnappy truncated throws");
  throws(()=>unsnappy(u8()),null,"unsnappy empty throws");
  throws(()=>snappy(null),null,"snappy null throws");
  throws(()=>unsnappy(null),null,"unsnappy null throws");
  {
    let bad = good.slice(); bad[0]^=0xFF; try{ unsnappy(bad);}catch(e){ assert(true,"snappy corrupt ok");}
  }
});
__kitS.end();

for(let sz of [0,1,2,100,1000,5000]){
    let d=new Uint8Array(sz); for(let i=0;i<sz;i++) d[i]= (i*13)&0xFF;
    assert(eqArr(unsnappy(snappy(d)),d),"snappy size "+sz);
  }

assert(eqArr(unsnappy(snappy("hello 😀")), new TextEncoder().encode("hello 😀")),"snappy unicode");
if(fails){print("test_compress_cov_bytes_compress_encoding_hash: "+fails+" FAILED of "+n); throw new Error("failed")}
print("test_compress_cov_bytes_compress_encoding_hash: "+n+" assertions, 0 failures")
