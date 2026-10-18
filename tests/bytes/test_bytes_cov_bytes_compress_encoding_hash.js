// Split from tests/test_cov_bytes_compress_encoding_hash.js: the bytes sections.
import { Bytes, Text, bytesOf, compare, equal, indexOf, lastIndexOf, contains, count, concat, copy, fill, toUtf8, fromUtf8, isValidUtf8, isValidUtf16, countUtf8, countUtf16, latin1ToUtf8, utf8ToLatin1, utf8ToUtf16, utf16ToUtf8, decode, encode, encodingExists, encodings, readUint8, readInt8, readInt16BE, readUint32LE, readUint32BE, readInt32LE, readInt32BE, readBigUint64BE, readBigInt64BE, readDoubleLE, writeInt8, writeInt16BE, writeUint32LE, writeUint32BE, writeInt32LE, writeBigUint64BE, writeBigInt64BE, writeDoubleLE } from "dyna:bytes";
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
  eq(b1.readUint8(0),0,"alloc zeroed");
  let b4 = Bytes.alloc(4);
  b4.writeUint32BE(0,0x01020304);
  eq(b4.readUint32BE(0),0x01020304,"alloc write/read");
  let bbig = Bytes.alloc(256*1024);
  eq(bbig.length,256*1024,"alloc 256KB");
  bbig.fill(0xAB); eq(bbig.readUint8(0),0xAB,"large fill");
  eq(bbig.readUint8(256*1024-1),0xAB,"large last byte");
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
  eq(bStr.toUtf8(),"hello","toUtf8");
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
  f.fill(7); eq(f.readUint8(0),7,"fill whole");
  f.fill(9,1,4); eq(f.readUint8(0),7,"fill preserved before");
  eq(f.readUint8(1),9,"fill 1");
  eq(f.readUint8(3),9,"fill 3");
  eq(f.readUint8(4),7,"fill after");
  f.fill(0x1FF,0,1); eq(f.readUint8(0),0xFF,"fill wrap");
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
  mid.fill(88);
  eq(owner.toString(),"aXXXefgh","slice view writes through");
  let inner = owner.slice(1,5).slice(1,3);
  inner.fill(89);
  assert(owner.toString().includes("YY"),"nested slice alias");
}
{
  let b = Bytes.alloc(8);
  b.writeUint8(0,0xAA); eq(b.readUint8(0),0xAA,"u8 first");
  b.writeUint8(7,0xBB); eq(b.readUint8(7),0xBB,"u8 last");
  throws(()=>b.readUint8(8),null,"read past end");
  throws(()=>b.readUint8(-1),null,"read negative");
  throws(()=>b.writeUint8(8,1),null,"write past end");
  let b2 = Bytes.alloc(2);
  b2.writeUint16LE(0,0x1234); eq(b2.readUint16LE(0),0x1234,"u16LE 0");
  b2.writeUint16BE(0,0x0102); eq(b2.readUint16BE(0),0x0102,"u16BE 0");
  throws(()=>b2.readUint16LE(1),null,"u16 past");
  throws(()=>b2.writeUint16BE(1,5),null,"u16 write past");
  b2.writeInt16LE(0,-1); eq(b2.readInt16LE(0),-1,"i16 -1");
  let b4 = Bytes.alloc(4);
  b4.writeUint32LE(0,0xFFFFFFFF); eq(b4.readUint32LE(0),0xFFFFFFFF,"u32 max");
  b4.writeInt32BE(0,-2147483648); eq(b4.readInt32BE(0),-2147483648,"i32 min");
  throws(()=>b4.readUint32LE(1),null,"u32 past");
  throws(()=>b4.writeUint32BE(1,1),null,"u32 write past");
  let b8 = Bytes.alloc(8);
  b8.writeBigUint64LE(0,0x0102030405060708n); eq(b8.readBigUint64LE(0),0x0102030405060708n,"u64 LE");
  b8.writeBigUint64BE(0,0x0102030405060708n); eq(b8.readBigUint64BE(0),0x0102030405060708n,"u64 BE");
  b8.writeBigInt64LE(0,-1n); eq(b8.readBigInt64LE(0),-1n,"i64 -1");
  throws(()=>b8.readBigUint64LE(1),null,"u64 past 1");
  let bf = Bytes.alloc(8);
  bf.writeFloatLE(0,1.5); eq(bf.readFloatLE(0),1.5,"f32 LE");
  bf.writeFloatBE(0,1.5); eq(bf.readFloatBE(0),1.5,"f32 BE");
  bf.writeDoubleLE(0,Math.PI); eq(bf.readDoubleLE(0),Math.PI,"f64 LE");
  bf.writeDoubleBE(0,Math.PI); eq(bf.readDoubleBE(0),Math.PI,"f64 BE");
  throws(()=>bf.readDoubleLE(1),null,"f64 past");
  let bn = Bytes.alloc(4);
  bn.writeUint8(3,0x99); eq(bn.readUint8(3),0x99,"read at length-1 ok");
  throws(()=>bn.readUint8(4),null,"read at length throws");
  throws(()=>bn.writeUint8(4,1),null,"write at length throws");
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
  let utf8b = t2.toUtf8();
  assert(utf8b instanceof Uint8Array,"toUtf8 bytes");
  eq(utf8b.length,5,"café 5 bytes");
  let back = new Text("café");
  assert(eqArr(back.toUtf8(), utf8b),"toUtf8 deterministic");
  throws(()=>new Text("😀").utf8ToLatin1(),null,"utf8ToLatin1 throws on wide");
  let lat = new Bytes(u8(0xE9)).toString();
  let conv = latin1ToUtf8(u8(0xE9));
  assert(conv.length===2,"latin1 0xE9 -> 2 bytes utf8");
  let rev = utf8ToLatin1(conv);
  assert(rev[0]===0xE9,"roundtrip latin1");
  let u16 = utf8ToUtf16("hello");
  assert(u16.length===10,"utf8ToUtf16 hello 10 bytes (2 per char)");
  let u8back = utf16ToUtf8(u16);
  eq(toUtf8(u8back),"hello","utf16 roundtrip");
  throws(()=>utf16ToUtf8(u8(0x61)),null,"utf16 odd length throws");
  throws(()=>utf16ToUtf8(u8(0x00,0xD8)),null,"unpaired surrogate throws");
  let tb = new Text("hi").toBytes();
  assert(Bytes.isBytes(tb),"toBytes is Bytes");
  eq(tb.length,2,"toBytes length");
}
{
  let ab = new ArrayBuffer(16);
  let u = new Uint8Array(ab,4,8);
  let b = bytesOf(u);
  eq(b.length,8,"bytesOf Uint8Array subarray length");
  let dv = new DataView(ab,2,6);
  let bdv = bytesOf(dv);
  eq(bdv.length,6,"bytesOf DataView length");
  let ab2 = bytesOf(ab);
  eq(ab2.length,16,"bytesOf ArrayBuffer");
  let u8v = new Uint8Array([1,2,3]);
  assert(bytesOf(u8v) instanceof Uint8Array,"bytesOf returns Uint8Array");
  eq(bytesOf(new Uint8Array(0)).length,0,"bytesOf empty");
  eq(bytesOf(new ArrayBuffer(0)).length,0,"bytesOf empty AB");
  eq(bytesOf(new DataView(new ArrayBuffer(0))).length,0,"bytesOf empty DataView");
  let one = bytesOf(u8(0xFF));
  eq(one[0],0xFF,"bytesOf 1-byte");
  let four = new Uint8Array([1,2,3,4]);
  eq(bytesOf(four).length,4,"4-byte straddle");
  throws(()=>bytesOf(null),null,"bytesOf null");
  throws(()=>bytesOf("abc"),null,"bytesOf string");
  throws(()=>bytesOf([1,2]),null,"bytesOf array");
  assert(bytesOf(new Uint16Array([1,2])).length===4,"bytesOf Uint16 ok");
  assert(bytesOf(new Float32Array([1.5])).length===4,"bytesOf Float32 ok");
  let bigU = new Uint8Array(256*1024);
  bigU.fill(0xCD);
  let bigB = bytesOf(bigU);
  eq(bigB.length,256*1024,"bytesOf large");
  eq(bigB[0],0xCD,"large first byte");
  eq(bigB[256*1024-1],0xCD,"large last byte");
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
  let fb = new Uint8Array(3); fill(fb,5); assert(fb[0]===5 && fb[2]===5,"free fill");
  eq(toUtf8(u8(97,98,99)),"abc","free toUtf8");
  assert(eqArr(fromUtf8("abc"),u8(97,98,99)),"free fromUtf8");
  eq(toUtf8(fromUtf8("😀")), "😀","utf8 roundtrip");
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
  let buf = new Uint8Array(8);
  writeUint32LE(buf,0,0x12345678); eq(readUint32LE(buf,0),0x12345678,"free r/w u32 LE");
  writeUint32BE(buf,0,0x12345678); eq(readUint32BE(buf,0),0x12345678,"free r/w u32 BE");
  writeDoubleLE(buf,0,NaN); assert(Number.isNaN(readDoubleLE(buf,0)),"free f64 NaN");
  throws(()=>readUint8(null,0),null,"readUint8 null throws");
  throws(()=>readUint8([1,2],0),null,"readUint8 array throws");
  throws(()=>compare("abc","abd"),null,"compare strings throws");
}
{
    const sb = new Uint8Array(4);
    eq(writeInt8(sb, 0, -1), 1, "writeInt8 returns next offset");
    eq(readInt8(sb, 0), -1, "readInt8 sign-extends");
    writeInt8(sb, 1, 127); writeInt8(sb, 2, -128);
    eq(readInt8(sb, 1), 127, "readInt8 max");
    eq(readInt8(sb, 2), -128, "readInt8 min");
    throws(()=>readInt8(sb, 4), null, "readInt8 at end throws");

    const s16 = new Uint8Array(4);
    eq(writeInt16BE(s16, 0, -2), 2, "writeInt16BE offset");
    eq(readInt16BE(s16, 0), -2, "readInt16BE negative");
    writeInt16BE(s16, 2, 32767);
    eq(readInt16BE(s16, 2), 32767, "readInt16BE max");
    throws(()=>readInt16BE(s16, 3), null, "readInt16BE straddle throws");

    const s32 = new Uint8Array(8);
    eq(writeInt32LE(s32, 0, -123456789), 4, "writeInt32LE offset");
    eq(readInt32LE(s32, 0), -123456789, "readInt32LE round-trip");
    writeInt32LE(s32, 4, 2147483647);
    eq(readInt32LE(s32, 4), 2147483647, "readInt32LE INT_MAX");
    throws(()=>readInt32LE(s32, 6), null, "readInt32LE straddle throws");
    const cmp = new Uint8Array(4);
    writeUint32LE(cmp, 0, 16909060);
    assert(readInt32BE(cmp, 0) !== 16909060, "BE read of LE bytes differs (endianness live)");
    eq(readUint8(cmp, 0), 4, "LE stores low byte first");

    const b64 = new Uint8Array(16);
    const BIG = 18446744073709551615n;
    const NEG = -9223372036854775808n;
    eq(writeBigUint64BE(b64, 0, BIG), 8, "writeBigUint64BE offset");
    eq(readBigUint64BE(b64, 0), BIG, "readBigUint64BE u64 max");
    eq(writeBigInt64BE(b64, 8, NEG), 16, "writeBigInt64BE offset");
    eq(readBigInt64BE(b64, 8), NEG, "readBigInt64BE i64 min");
    throws(()=>readBigInt64BE(b64, 9), null, "readBigInt64BE straddle throws");
}
if(fails){print("test_bytes_cov_bytes_compress_encoding_hash: "+fails+" FAILED of "+n); throw new Error("failed")}
print("test_bytes_cov_bytes_compress_encoding_hash: "+n+" assertions, 0 failures")
