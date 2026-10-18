// Split from tests/test_cov_bytes_compress_encoding_hash.js: the bytes sections.
import { Bytes, Text, compare, equal, indexOf, lastIndexOf, contains, count, concat, copy, isValidUtf8, isValidUtf16, countUtf8, countUtf16, latin1ToUtf8, utf8ToLatin1, utf8ToUtf16, utf16ToUtf8, decode, encode, encodingExists, encodings } from "dyna:bytes";
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
{
  let b0 = Bytes.alloc(0);
  eq(b0.length,0,"Bytes.alloc 0");
  assert(b0.isAscii===true,"alloc 0 isAscii vacuous");
  assert(b0.isValidUtf8===true,"alloc 0 valid utf8");
  let b1 = Bytes.alloc(1);
  eq(b1.length,1,"alloc 1");
  eq(b1.array[0],0,"alloc zeroed");
  let b4 = Bytes.alloc(4);
  new DataView(b4.array.buffer, b4.array.byteOffset, b4.array.byteLength).setUint32(0,0x01020304);
  eq(b4.array[0],1,"alloc storage written through a DataView over .array");
  eq(b4.array[3],4,"big-endian last byte");
  let bbig = Bytes.alloc(256*1024);
  eq(bbig.length,256*1024,"alloc 256KB");
  bbig.array.fill(0xAB); eq(bbig.array[0],0xAB,"large fill");
  eq(bbig.array[256*1024-1],0xAB,"large last byte");
  assert(bbig.isAscii===false,"a fill through .array is seen by the cached isAscii");
  throws(()=>Bytes.alloc(-1),null,"alloc negative");
  throws(()=>Bytes.alloc(),null,"alloc no arg");
  let bn1 = Bytes.alloc(0); eq(bn1.length,0,"alloc N=0");
  let bn2 = Bytes.alloc(1); eq(bn2.length,1,"alloc N=1");
  let bn3 = Bytes.alloc(2); eq(bn3.length,2,"alloc N=2");
}
{
  assert(Bytes.isBytes(new Bytes("x")),"isBytes true");
  assert(!Bytes.isBytes("x"),"isBytes false string");
  assert(!Bytes.isBytes(new Uint8Array(1)),"isBytes false u8");
  assert(!Bytes.isBytes(null),"isBytes false null");
  assert(!Bytes.isBytes(42),"isBytes false number");
}
{
  let bStr = new Bytes("hello");
  eq(bStr.length,5,"Bytes str length");
  eq(bStr.toString(),"hello","Bytes toString");
  assert(bStr.isAscii===true,"ascii flag");
  let bU8 = new Bytes(u8(1,2,3));
  eq(bU8.length,3,"Bytes u8 length");
  let bAB = new Bytes(new Uint8Array([9,8,7]).buffer);
  eq(bAB.length,3,"Bytes ArrayBuffer");
  let bDV = new Bytes(new DataView(new Uint8Array([5,6,7]).buffer));
  eq(bDV.length,3,"Bytes DataView");
  let bNul = new Bytes("a\u0000b\u0000c");
  eq(bNul.length,5,"NUL embedded length");
  assert(bNul.toString().charCodeAt(1)===0,"NUL preserved");
  let bEmoji = new Bytes("😀");
  eq(bEmoji.length,4,"emoji 4 bytes");
  assert(bEmoji.isAscii===false,"emoji not ascii");
  assert(bEmoji.isValidUtf8===true,"emoji valid");
  eq(new Bytes("").length,0,"empty Bytes");
  eq(new Bytes(u8(0xFF)).length,1,"1-byte");
  eq(new Bytes(u8(0xFF)).isValidUtf8,false,"0xFF invalid utf8");
  eq(new Bytes(u8(0xC3,0x28)).isValidUtf8,false,"truncated invalid");
  throws(()=>new Bytes(new Uint16Array(2)),/new Uint8Array\(v\.buffer, v\.byteOffset, v\.byteLength\)/,"a wider view is refused, naming the standard reinterpretation");
  throws(()=>new Bytes(),null,"Bytes no arg");
  throws(()=>new Bytes(new Float64Array(2)),null,"Bytes Float64 rejected");
  throws(()=>new Bytes(null),null,"Bytes null rejected");
  throws(()=>new Bytes(123),null,"Bytes number rejected");
}
{
  eq(Bytes.concat([]).length,0,"Bytes.concat empty");
  assert(eqArr(Bytes.concat([u8(1,2),u8(3)]).array, u8(1,2,3)),"Bytes.concat basic");
  eq(Bytes.concat([new Bytes("ab"), new Bytes("cd")]).toString(),"abcd","Bytes.concat Bytes handles");
  eq(Bytes.concat([u8()]).length,0,"concat empty elem");
  let a = new Bytes("abc");
  let b = new Bytes("abd");
  eq(a.compare(b.array),-1,"compare less");
  eq(b.compare(a.array),1,"compare greater");
  eq(a.compare(new Bytes("abc").array),0,"compare equal");
  eq(a.compare(new Uint8Array(0)),1,"compare vs empty");
  assert(a.equals(new Uint8Array([97,98,99])),"equals true");
  assert(!a.equals(u8(1,2)),"equals false");
  assert(!a.equals(u8(97,98)),"equals diff len false");
  let hay = new Bytes("hello world hello");
  eq(hay.indexOf(108),2,"indexOf byte l");
  eq(hay.indexOf(u8(108,108)),2,"indexOf view ll");
  eq(hay.lastIndexOf(108),15,"lastIndexOf l");
  eq(hay.lastIndexOf(u8(108,108)),14,"lastIndexOf view");
  eq(hay.indexOf(255),-1,"indexOf absent byte");
  eq(hay.indexOf(u8(99,99)),-1,"indexOf absent view");
  assert(hay.includes(101),"includes byte e");
  assert(!hay.includes(0),"includes absent");
  assert(hay.includes(u8(119,111)),"includes view wo");
  eq(hay.count(108),5,"count l");
  eq(hay.count(u8(108,108)),2,"count ll non-overlap");
  eq(hay.count(u8()), hay.length+1,"count empty needle");
  eq(new Bytes("").count(u8()),1,"count empty hay empty needle");
  eq(hay.indexOfAny(u8(119,111)),4,"indexOfAny earliest");
  eq(hay.indexOfAny(u8(255)),-1,"indexOfAny absent");
  let f = Bytes.alloc(5);
  f.array.fill(7); eq(f.array[0],7,"fill whole");
  f.array.fill(9,1,4); eq(f.array[0],7,"fill preserved before");
  eq(f.array[1],9,"fill 1");
  eq(f.array[3],9,"fill 3");
  eq(f.array[4],7,"fill after");
  eq(f.indexOf(9),1,"indexOf sees bytes written through .array");
  eq(f.count(9),3,"count sees bytes written through .array");
}
{
  let owner = new Bytes("abcdefgh");
  let s0 = owner.slice();
  eq(s0.length,8,"slice no args");
  eq(s0.toString(),"abcdefgh","slice whole");
  let s1 = owner.slice(1,4);
  eq(s1.toString(),"bcd","slice 1-4");
  let s2 = owner.slice(-2);
  eq(s2.toString(),"gh","slice negative");
  eq(owner.slice(0,99).length,8,"slice clamped end");
  eq(owner.slice(4,1).length,0,"inverted empty");
  let mid = owner.slice(1,4);
  mid.array.fill(88);
  eq(owner.toString(),"aXXXefgh","slice view writes through");
  let inner = owner.slice(1,5).slice(1,3);
  inner.array.fill(89);
  assert(owner.toString().includes("YY"),"nested slice alias");
}
{
  let owner = Bytes.alloc(8);
  let s = owner.slice(2,6);
  let a = s.array;
  eq(a.byteLength,4,"a slice's array is bounded by the slice");
  eq(a.byteOffset-owner.array.byteOffset,2,"a slice's array starts at the slice");
  assert(a.buffer===owner.array.buffer,"a slice's array shares the owner's storage");
  let dv = new DataView(a.buffer, a.byteOffset, a.byteLength);
  dv.setUint16(0,0xAABB); dv.setUint16(2,0xCCDD,true);
  eq(hex(owner.array),"0000aabbddcc0000","DataView writes over a slice land at the slice's offset in the owner");
  throws(()=>dv.getUint8(4),null,"the slice's DataView ends where the slice ends");
  throws(()=>dv.setUint32(1,0),null,"a straddling write past the slice is refused");
  eq(hex(owner.array),"0000aabbddcc0000","the refused write changed nothing");
}
{
  let t = new Text("hello");
  eq(t.value,"hello","Text value");
  eq(t.isWide,false,"Text ascii not wide");
  eq(new Text("héllo").isWide,false,"latin1 not wide");
  eq(new Text("héllo→").isWide,true,"wide true");
  eq(new Text("a\u0000b").isWide,false,"NUL not wide");
  eq(new Text("").isWide,false,"empty not wide");
  eq(new Text("").countUtf8(),0,"empty countUtf8");
  eq(new Text("abc").countUtf8(),3,"countUtf8 abc");
  eq(new Text("😀").countUtf8(),1,"countUtf8 emoji 1 codepoint");
  eq(new Text("😀🎉").countUtf16(),2,"countUtf16 emoji 2");
  eq(new Text("héllo").countUtf16(),5,"countUtf16 latin");
  assert(new Text("héllo").isValidUtf8===true,"Text valid utf8 (DT-8 getter)");
  assert(new Text("abc").isValidUtf16()===true,"valid utf16");
  let t2 = new Text("café");
  let utf8b = t2.toBytes().array;
  assert(utf8b instanceof Uint8Array,"toBytes().array is a Uint8Array");
  eq(utf8b.length,5,"café 5 bytes");
  eq(hex(utf8b),"636166c3a9","café as UTF-8");
  throws(()=>new Text("😀").utf8ToLatin1(),null,"utf8ToLatin1 throws on wide");
  let lat = new Bytes(u8(0xE9)).toString();
  let conv = latin1ToUtf8(u8(0xE9));
  assert(conv.length===2,"latin1 0xE9 -> 2 bytes utf8");
  let rev = utf8ToLatin1(conv);
  assert(rev[0]===0xE9,"roundtrip latin1");
  let u16 = utf8ToUtf16("hello");
  assert(u16.length===10,"utf8ToUtf16 hello 10 bytes (2 per char)");
  let u8back = utf16ToUtf8(u16);
  eq(decode(u8back,"utf-8"),"hello","utf16 roundtrip");
  throws(()=>utf16ToUtf8(u8(0x61)),null,"utf16 odd length throws");
  throws(()=>utf16ToUtf8(u8(0x00,0xD8)),null,"unpaired surrogate throws");
  let tb = new Text("hi").toBytes();
  assert(Bytes.isBytes(tb),"toBytes is Bytes");
  eq(tb.length,2,"toBytes length");
}
{
  let ab = new ArrayBuffer(16);
  new Uint8Array(ab).forEach((_,i,a)=>{ a[i]=i; });
  let b = new Bytes(new Uint8Array(ab,4,8));
  eq(b.length,8,"Bytes from a Uint8Array subarray takes its length");
  eq(b.array[0],4,"and starts at its offset");
  let bdv = new Bytes(new DataView(ab,2,6));
  eq(bdv.length,6,"Bytes from an offset DataView takes its length");
  eq(bdv.array[5],7,"and ends at its end");
  eq(new Bytes(ab).length,16,"Bytes from an ArrayBuffer");
  eq(new Bytes(new Uint8Array(0)).length,0,"Bytes from an empty view");
  eq(new Bytes(new ArrayBuffer(0)).length,0,"Bytes from an empty ArrayBuffer");
  eq(new Bytes(new DataView(new ArrayBuffer(0))).length,0,"Bytes from an empty DataView");
  let w = new Uint16Array([0x0201,0x0403]);
  eq(new Bytes(new Uint8Array(w.buffer,w.byteOffset,w.byteLength)).length,4,"the reinterpretation the refusal names is accepted");
}
{
  let a = u8(1,2,3,2,3,4);
  eq(indexOf(a,2),1,"free indexOf byte");
  eq(lastIndexOf(a,3),4,"free lastIndexOf byte");
  assert(contains(a,2)===true,"free contains true");
  assert(contains(a,9)===false,"free contains false");
  eq(count(a,u8(2,3)),2,"free count view");
  assert(equal(u8(1,2),u8(1,2)),"free equal true");
  assert(!equal(u8(1,2),u8(1,3)),"free equal false");
  eq(compare(u8(1,2),u8(1,3)),-1,"free compare less");
  eq(compare(u8(1,3),u8(1,2)),1,"free compare greater");
  assert(eqArr(concat([u8(1,2),u8(3,4)]),[1,2,3,4]),"free concat");
  let dst = new Uint8Array(4);
  eq(copy(dst,u8(9,8),1,0,2),2,"free copy count");
  eq(dst[1],9,"copy dstOff");
  assert(isValidUtf8("hello")===true,"isValidUtf8 string true");
  assert(isValidUtf8(u8(0xFF))===false,"isValidUtf8 false");
  assert(isValidUtf16(u8(0x61,0x00))===true,"isValidUtf16 true");
  assert(isValidUtf16(u8(0x00))===false,"isValidUtf16 odd false");
  eq(countUtf8("héllo"),5,"free countUtf8");
  eq(countUtf16(u8(0x61,0x00,0x62,0x00)),2,"free countUtf16");
  assert(encodingExists("utf-8")===true,"encodingExists utf-8");
  assert(encodingExists("UTF8")===true,"case insensitive");
  assert(!encodingExists("nonexistent-xyz"),"encodingExists false");
  let encList = encodings();
  assert(Array.isArray(encList),"encodings array");
  assert(encList[0]==="utf-8","encodings first utf-8");
  assert(encList.length>1,"encodings >1");
  eq(decode(u8(97,98,99),"utf-8"),"abc","decode utf-8");
  assert(encode("abc","utf-8") instanceof Uint8Array,"encode utf-8");
  throws(()=>decode(u8(1), "nope-charset"),null,"decode unknown charset");
  throws(()=>compare("abc","abd"),null,"compare strings throws");
}
if(fails){print("test_bytes_cov_bytes_compress_encoding_hash: "+fails+" FAILED of "+n); throw new Error("failed")}
print("test_bytes_cov_bytes_compress_encoding_hash: "+n+" assertions, 0 failures")
