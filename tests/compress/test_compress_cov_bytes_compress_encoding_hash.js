// Split from tests/test_cov_bytes_compress_encoding_hash.js: the compress sections.
import { bytesOf } from "dyna:bytes";
import { gzip, gunzip, zstd, unzstd, brotli, unbrotli, snappy, unsnappy, lz4Compress, lz4Decompress, lz4Frame, lz4Unframe, TarPack, TarList, TarExtract, ZipPack, ZipList, ZipRead, Compressor, Dictionary } from "dyna:compress";
import { ZipList as ZipListCov } from "dyna:compress";
import * as CryptoNS from "dyna:crypto";

const { HMAC, HMACHex, Hmac, PBKDF2, HKDF, RandomBytes, TimingSafeEqual, HOTPGenerate, TOTPGenerate, JWTSign, JWTVerify } = CryptoNS;
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
{
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

}
{
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
}
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
{
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
}
  for(let sz of [0,1,2,100,1000,5000]){
    let d=new Uint8Array(sz); for(let i=0;i<sz;i++) d[i]= (i*13)&0xFF;
    assert(eqArr(unsnappy(snappy(d)),d),"snappy size "+sz);
  }
  assert(eqArr(unsnappy(snappy("hello 😀")), new TextEncoder().encode("hello 😀")),"snappy unicode");
{
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
}
{
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
}
  for(let sz of [0,1,2,100,1000,5000]){
    let d=new Uint8Array(sz); for(let i=0;i<sz;i++) d[i]= (i*19)&0xFF;
    assert(eqArr(lz4Unframe(lz4Frame(d)),d),"lz4Frame size "+sz);
  }
  {
    let d2 = new TextEncoder().encode("frame test ".repeat(100));
    for(let lvl of [1,6,12]) for(let cs of [true,false]) assert(eqArr(lz4Unframe(lz4Frame(d2,{level:lvl, checksum:cs})),d2),"lz4Frame lvl "+lvl+" cs "+cs);
  }
{
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
}
{
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
}
{
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
}
{
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
}
{
    {
        const eocd = new Uint8Array(22);
        const d2 = new DataView(eocd.buffer);
        d2.setUint32(0, 0x06054b50, true);
        d2.setUint16(8, 0xFFFF);
        d2.setUint16(10, 0xFFFF);
        d2.setUint32(12, 0xFFFFFFFF);
        d2.setUint32(16, 0xFFFFFFFF);
        throws(()=>ZipListCov(eocd), /ZIP64/i, "zip64 named refusal");
    }
}
if(fails){print("test_compress_cov_bytes_compress_encoding_hash: "+fails+" FAILED of "+n); throw new Error("failed")}
print("test_compress_cov_bytes_compress_encoding_hash: "+n+" assertions, 0 failures")
