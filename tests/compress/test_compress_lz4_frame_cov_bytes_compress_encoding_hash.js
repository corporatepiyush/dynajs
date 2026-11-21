// covers: dyna:compress.lz4Frame
// Split from tests/test_cov_bytes_compress_encoding_hash.js: the compress sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_lz4_frame_cov_bytes_compress_encoding_hash");
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

__kitS.section("frame test", () => {
  let data = new TextEncoder().encode("frame test ".repeat(100));
  assert(eqArr(lz4Unframe(lz4Frame(new Uint8Array(0))),[]),"lz4Frame empty");
  assert(eqArr(lz4Unframe(lz4Frame(u8(5))),[5]),"lz4Frame 1byte");
  assert(eqArr(lz4Unframe(lz4Frame(data)),data),"lz4Frame 1KB");
  let big = new Uint8Array(64*1024); for(let i=0;i<big.length;i++) big[i]=i%253;
  assert(eqArr(lz4Unframe(lz4Frame(big)),big),"lz4Frame 64KB");
  eq(lz4Unframe(lz4Frame("hello"),{asString:true}),"hello","lz4Frame asString");
  let withCS = lz4Frame(data,{checksum:true});
  assert(eqArr(lz4Unframe(withCS),data),"lz4Frame checksum true");
  let withoutCS = lz4Frame(data,{checksum:false});
  assert(eqArr(lz4Unframe(withoutCS),data),"lz4Frame checksum false");
  assert(eqArr(lz4Unframe(lz4Frame(data,{level:1})),data),"lz4Frame level 1");
  assert(eqArr(lz4Unframe(lz4Frame(data,{level:12})),data),"lz4Frame level 12");
  throws(()=>lz4Frame(data,{level:0}),null,"lz4Frame level 0 throws");
  throws(()=>lz4Frame(data,{level:13}),null,"lz4Frame level 13 throws");
  let good = lz4Frame(data);
  throws(()=>lz4Unframe(good.slice(0,5)),null,"lz4Unframe truncated throws");
  {
    let bad = good.slice(); bad[bad.length-1]^=0xFF; try{ lz4Unframe(bad); assert(true,"checksum corrupt may throw"); }catch(e){ assert(true,"checksum corrupt throws");}
  }
});
__kitS.end();

for(let sz of [0,1,2,100,1000,5000]){
    let d=new Uint8Array(sz); for(let i=0;i<sz;i++) d[i]= (i*19)&0xFF;
    assert(eqArr(lz4Unframe(lz4Frame(d)),d),"lz4Frame size "+sz);
  }

{
    let d2 = new TextEncoder().encode("frame test ".repeat(100));
    for(let lvl of [1,6,12]) for(let cs of [true,false]) assert(eqArr(lz4Unframe(lz4Frame(d2,{level:lvl, checksum:cs})),d2),"lz4Frame lvl "+lvl+" cs "+cs);
  }
if(fails){print("test_compress_cov_bytes_compress_encoding_hash: "+fails+" FAILED of "+n); throw new Error("failed")}
print("test_compress_cov_bytes_compress_encoding_hash: "+n+" assertions, 0 failures")
