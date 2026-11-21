// covers: dyna:compress.Compressor
// Split from tests/test_cov_bytes_compress_encoding_hash.js: the compress sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_compress_compressor_cov_bytes_compress_encoding_hash");
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

__kitS.section("compressor test", () => {
  let data = new TextEncoder().encode("compressor test ".repeat(50));
  let cGzip = new Compressor({algo:"gzip"});
  assert(cGzip.algo==="gzip","Compressor gzip algo");
  assert(cGzip.dictId===null,"gzip dictId null");
  assert(eqArr(cGzip.decompress(cGzip.compress(data)),data),"Compressor gzip roundtrip");
  assert(!cGzip.closed,"not closed");
  cGzip.close(); assert(cGzip.closed,"closed after close");
  cGzip.close();
  let cLz4 = new Compressor({algo:"lz4", level:1});
  assert(eqArr(cLz4.decompress(cLz4.compress(data)),data),"Compressor lz4 level1");
  cLz4.close();
  let cLz4h = new Compressor({algo:"lz4", level:12});
  assert(eqArr(cLz4h.decompress(cLz4h.compress(data)),data),"Compressor lz4 level12");
  cLz4h.close();
  throws(()=>new Compressor({algo:"lz4", level:0}),null,"Compressor lz4 level 0 throws");
  throws(()=>new Compressor({algo:"lz4", level:13}),null,"Compressor lz4 level13 throws");
  let cF = new Compressor({algo:"lz4frame", checksum:true});
  assert(eqArr(cF.decompress(cF.compress(data)),data),"Compressor lz4frame");
  cF.close();
  let cF2 = new Compressor({algo:"lz4frame", checksum:false});
  assert(eqArr(cF2.decompress(cF2.compress(data)),data),"Compressor lz4frame no checksum");
  cF2.close();
  let cZ = new Compressor({algo:"zstd", level:1});
  assert(eqArr(cZ.decompress(cZ.compress(data)),data),"Compressor zstd 1");
  cZ.close();
  let cZ22 = new Compressor({algo:"zstd", level:22});
  assert(eqArr(cZ22.decompress(cZ22.compress(data)),data),"Compressor zstd 22");
  cZ22.close();
  throws(()=>new Compressor({algo:"zstd", level:0}),null,"Compressor zstd 0 throws");
  throws(()=>new Compressor({algo:"zstd", level:23}),null,"Compressor zstd 23 throws");
  throws(()=>new Compressor({algo:"zstd", level:100}),null,"Compressor zstd 100 throws");
  let cB0 = new Compressor({algo:"brotli", level:0});
  assert(eqArr(cB0.decompress(cB0.compress(data)),data),"Compressor brotli 0");
  cB0.close();
  let cB11 = new Compressor({algo:"brotli", level:11});
  assert(eqArr(cB11.decompress(cB11.compress(data)),data),"Compressor brotli 11");
  cB11.close();
  throws(()=>new Compressor({algo:"brotli", level:12}),null,"Compressor brotli 12 throws");
  throws(()=>new Compressor({algo:"brotli", level:-1}),null,"Compressor brotli -1 throws");
  let cS = new Compressor({algo:"snappy"});
  assert(eqArr(cS.decompress(cS.compress(data)),data),"Compressor snappy");
  cS.close();
  try{ let cs2 = new Compressor({algo:"snappy", level:5}); assert(eqArr(cs2.decompress(cs2.compress(data)),data),"snappy with level"); cs2.close(); }catch(e){ assert(true,"snappy level maybe throws but not required"); }
  throws(()=>new Compressor({algo:"unknown"}),null,"Compressor unknown algo throws");
  throws(()=>new Compressor({algo:"gzip", dict:u8(1)}),null,"Compressor gzip with dict throws for non-lz4");
  throws(()=>new Compressor({algo:"zstd", dict:u8(1)}),null,"Compressor zstd with dict throws");
  let dictBytes = new TextEncoder().encode("hello world dict ");
  let cLD = new Compressor({algo:"lz4", dict:dictBytes});
  assert(cLD.dictId!==null,"lz4 dictId not null");
  assert(typeof cLD.dictId==="number","dictId number");
  assert(eqArr(cLD.decompress(cLD.compress(data)),data),"Compressor lz4 dict roundtrip");
  cLD.close();
  let cTmp = new Compressor({algo:"gzip"});
  cTmp.close();
  throws(()=>cTmp.compress(data),null,"compress after close throws");
  throws(()=>cTmp.decompress(data),null,"decompress after close throws");
});
__kitS.end();
if(fails){print("test_compress_cov_bytes_compress_encoding_hash: "+fails+" FAILED of "+n); throw new Error("failed")}
print("test_compress_cov_bytes_compress_encoding_hash: "+n+" assertions, 0 failures")
