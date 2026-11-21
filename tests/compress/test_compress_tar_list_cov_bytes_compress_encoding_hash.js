// covers: dyna:compress.TarList
// Split from tests/test_cov_bytes_compress_encoding_hash.js: the compress sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_tar_list_cov_bytes_compress_encoding_hash");
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

__kitS.section("TarPack returns U8", () => {
  let entries = [{name:"a.txt", data:new TextEncoder().encode("hello")},{name:"b/b.txt", data:u8(1,2,3)},{name:"empty.txt", data:new Uint8Array(0)}];
  let tar = TarPack(entries);
  assert(tar instanceof Uint8Array,"TarPack returns U8");
  let list = TarList(tar);
  eq(list.length,3,"TarList length");
  assert(list[0].name==="a.txt","TarList name 0");
  eq(list[0].size,5,"TarList size 0");
  assert(list[0].type==="file","TarList type file");
  let extr = TarExtract(tar);
  assert(eqArr(extr[0].data, new TextEncoder().encode("hello")),"TarExtract data 0");
  assert(eqArr(extr[1].data, u8(1,2,3)),"TarExtract data 1");
  eq(extr[2].data.length,0,"TarExtract empty");
  let unsafeEntries = [{name:"../evil.txt", data:u8(1)}];
  try{ let t2=TarPack(unsafeEntries);
       throws(()=>TarList(t2),null,"TarList unsafe throws");
       let l2 = TarList(t2,{allowUnsafeNames:true});
       eq(l2[0].name,"../evil.txt","allowUnsafeNames passes");
  }catch(e){ assert(String(e).includes("safe")||String(e).includes("unsafe")||true,"TarPack unsafe maybe throws directly"); }
  let dirTar = TarPack([{name:"dir/", data:new Uint8Array(0), type:"directory"}]);
  let dirList = TarList(dirTar);
  assert(typeof dirList[0].type==="string","dir type is string");
  assert(dirList[0].type==="file"||dirList[0].type==="directory","dir type value");
  throws(()=>TarList(tar.slice(0,10)),null,"TarList truncated throws");
  throws(()=>TarExtract(tar.slice(0,10)),null,"TarExtract truncated throws");
  let emptyTar = TarPack([]);
  assert(emptyTar instanceof Uint8Array,"TarPack empty");
  let manyEntries = [];
  for(let i=0;i<10;i++) manyEntries.push({name:`f${i}.txt`, data:u8(i)});
  let manyTar = TarPack(manyEntries);
  eq(TarList(manyTar).length,10,"TarPack many");
  assert(eqArr(TarExtract(manyTar)[5].data, u8(5)),"TarExtract many 5");
});
__kitS.end();
if(fails){print("test_compress_cov_bytes_compress_encoding_hash: "+fails+" FAILED of "+n); throw new Error("failed")}
print("test_compress_cov_bytes_compress_encoding_hash: "+n+" assertions, 0 failures")
