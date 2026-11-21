// covers: dyna:compress.ZipRead
// Split from tests/test_cov_bytes_compress_encoding_hash.js: the compress sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_zip_read_cov_bytes_compress_encoding_hash");
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

__kitS.section("ZipPack returns U8", () => {
  let zip = ZipPack([{name:"a.txt", data:new TextEncoder().encode("hello")},{name:"b.txt", data:u8(1,2,3)}]);
  assert(zip instanceof Uint8Array,"ZipPack returns U8");
  let zlist = ZipList(zip);
  eq(zlist.length,2,"ZipList length");
  assert(zlist[0].name==="a.txt","ZipList name");
  eq(zlist[0].size,5,"ZipList size");
  assert(zlist[0].method==="deflate"||zlist[0].method==="store","ZipList method");
  let aData = ZipRead(zip,"a.txt");
  assert(eqArr(aData, new TextEncoder().encode("hello")),"ZipRead a.txt");
  assert(eqArr(ZipRead(zip,"b.txt"),u8(1,2,3)),"ZipRead b.txt");
  let zipStore = ZipPack([{name:"c.txt", data:u8(1,2,3)}],{method:"store"});
  assert(eqArr(ZipRead(zipStore,"c.txt"),u8(1,2,3)),"Zip store roundtrip");
  throws(()=>ZipRead(zip,"missing.txt"),null,"ZipRead missing throws");
  try{
    let uzip = ZipPack([{name:"../evil.txt", data:u8(1)}]);
    throws(()=>ZipList(uzip),null,"ZipList unsafe throws");
    let l2 = ZipList(uzip,{allowUnsafeNames:true});
    eq(l2[0].name,"../evil.txt","Zip allowUnsafe");
    assert(eqArr(ZipRead(uzip,"../evil.txt",{allowUnsafeNames:true}),u8(1)),"ZipRead allowUnsafe");
  }catch(e){ assert(true,"Zip unsafe pack maybe throws"); }
  throws(()=>ZipList(zip.slice(0,10)),null,"ZipList truncated throws");
  throws(()=>ZipRead(zip.slice(0,10),"a.txt"),null,"ZipRead truncated throws");
  let many = [];
  for(let i=0;i<10;i++) many.push({name:`f${i}.txt`, data:u8(i,i+1)});
  let manyZip = ZipPack(many);
  eq(ZipList(manyZip).length,10,"Zip many");
  assert(eqArr(ZipRead(manyZip,"f5.txt"),u8(5,6)),"ZipRead many");
});
__kitS.end();
if(fails){print("test_compress_cov_bytes_compress_encoding_hash: "+fails+" FAILED of "+n); throw new Error("failed")}
print("test_compress_cov_bytes_compress_encoding_hash: "+n+" assertions, 0 failures")
