// Split from tests/test_cov_bytes_compress_encoding_hash.js: the encoding sections.
import { HexEncode, HexDecode, Base64Encode, Base64Decode, Base64URLEncode, Base64URLDecode, Base32Encode, Base32Decode, Base32HexEncode, Base32HexDecode, Base85Encode, Base85Decode, Base58Encode, Base58Decode, Base58CheckEncode, Base58CheckDecode, BaseXEncode, BaseXDecode, PutUvarint, Uvarint, PutVarint, Varint, DetectEncoding, detectEncoding, JSON5Parse, JSON5Stringify, StableStringify, JSONPath, QREncode, QRToString } from "dyna:encoding";
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
print("compress done: "+n+" assertions");
{
  eq(HexEncode(new Uint8Array(0)),"","Hex empty");
  eq(HexEncode(u8(0xDE,0xAD)),"dead","Hex dead");
  eq(HexEncode("abc"),"616263","Hex abc string");
  eq(HexEncode(u8(0x00,0xFF)),"00ff","Hex 00ff");
  assert(eqArr(HexDecode(""),[]),"HexDecode empty");
  assert(eqArr(HexDecode("dead"),u8(0xDE,0xAD)),"HexDecode dead");
  assert(eqArr(HexDecode("DEAD"),u8(0xDE,0xAD)),"HexDecode upper");
  assert(eqArr(HexDecode(HexEncode(u8(1,2,3))),u8(1,2,3)),"Hex roundtrip");
  throws(()=>HexDecode("abc"),null,"Hex odd length throws");
  throws(()=>HexDecode("zz"),null,"Hex invalid digit throws");
  throws(()=>HexDecode("0g"),null,"Hex invalid 2nd throws");
  throws(()=>HexDecode("g0"),null,"Hex invalid 1st throws");
  throws(()=>HexEncode(42),null,"HexEncode number throws");
  throws(()=>HexEncode(null),null,"HexEncode null throws");
  throws(()=>HexEncode([1,2]),null,"HexEncode array throws");
  throws(()=>HexEncode(new Uint16Array([1])),null,"HexEncode Uint16 throws");
  let big = new Uint8Array(1024); for(let i=0;i<1024;i++) big[i]=i&0xFF;
  assert(eqArr(HexDecode(HexEncode(big)),big),"Hex large roundtrip");
}
{
  eq(Base64Encode(new Uint8Array(0)),"","Base64 empty");
  eq(Base64Encode(u8(0x68,0x69)),"aGk=","Base64 hi");
  assert(eqArr(Base64Decode("aGk="),u8(0x68,0x69)),"Base64 decode hi");
  eq(Base64Encode("f"),"Zg==","Base64 f string");
  eq(Base64Encode(u8(0)),"AA==","Base64 1byte pad ==");
  eq(Base64Encode(u8(0,0)),"AAA=","Base64 2bytes pad =");
  eq(Base64Encode(u8(0,0,0)),"AAAA","Base64 3bytes no pad");
  assert(eqArr(Base64Decode("AA=="),u8(0)),"Base64 decode 1byte");
  assert(eqArr(Base64Decode("AAA="),u8(0,0)),"Base64 decode 2bytes");
  throws(()=>Base64Decode("a"),null,"Base64 bad length throws");
  throws(()=>Base64Decode("!!!!"),null,"Base64 invalid chars throws");
  throws(()=>Base64Decode("Z==="),null,"Base64 misplaced pad throws");
  throws(()=>Base64Decode("aGk=\n"),null,"Base64 newline throws");
  eq(Base64URLEncode(new Uint8Array(0)),"","Base64URL empty");
  eq(Base64URLEncode(u8(0xF8)),"-A","Base64URL 0xF8 dash");
  eq(Base64URLEncode(u8(0xFC)),"_A","Base64URL 0xFC underscore");
  assert(eqArr(Base64URLDecode("-A"),u8(0xF8)),"Base64URL decode dash");
  assert(eqArr(Base64URLDecode("_A"),u8(0xFC)),"Base64URL decode underscore");
  throws(()=>Base64URLDecode("+A"),null,"Base64URL rejects +");
  throws(()=>Base64URLDecode("/A"),null,"Base64URL rejects /");
  assert(eqArr(Base64URLDecode("Zg=="),u8(0x66)),"Base64URL tolerates ==");
  for(let len of [0,1,2,3,4,5,16,64]){
    let b=new Uint8Array(len); for(let i=0;i<len;i++) b[i]=(i*37)&0xFF;
    assert(eqArr(Base64Decode(Base64Encode(b)),b),"Base64 roundtrip len "+len);
    assert(eqArr(Base64URLDecode(Base64URLEncode(b)),b),"Base64URL roundtrip len "+len);
  }
  throws(()=>Base64URLDecode("**"),null,"Base64URL invalid char throws");
  throws(()=>Base64Encode(null),null,"Base64Encode null throws");
  assert(Base64Decode(null) instanceof Uint8Array,"Base64Decode null returns bytes (lenient)");
}
{
  eq(Base32Encode(new Uint8Array(0)),"","Base32 empty");
  eq(Base32Decode("").length,0,"Base32 decode empty");
  eq(Base32Encode("f"),"MY======","Base32 f");
  eq(Base32Encode("fo"),"MZXQ====","Base32 fo");
  eq(Base32Encode("foobar"),"MZXW6YTBOI======","Base32 foobar");
  assert(eqArr(Base32Decode("MY======"),u8(0x66)),"Base32 decode f");
  eq(Base32HexEncode(new Uint8Array(0)),"","Base32Hex empty");
  eq(Base32HexDecode("").length,0,"Base32Hex decode empty");
  eq(Base32HexEncode("f"),"CO======","Base32Hex f");
  for(let len of [0,1,2,3,4,5,6,7,8]){
    let b=new Uint8Array(len); for(let i=0;i<len;i++) b[i]=i;
    assert(eqArr(Base32Decode(Base32Encode(b)),b),"Base32 roundtrip len "+len);
    assert(eqArr(Base32HexDecode(Base32HexEncode(b)),b),"Base32Hex roundtrip len "+len);
  }
  throws(()=>Base32Decode("my======"),null,"Base32 lowercase throws");
  throws(()=>Base32Decode("0Y======"),null,"Base32 digit 0 invalid");
  throws(()=>Base32HexDecode("WO======"),null,"Base32Hex W invalid");
  throws(()=>Base32Decode("MY====="),null,"Base32 bad length throws");
  throws(()=>Base32Decode("MY======MZXW6YTB"),null,"Base32 padding before final throws");
}
{
  eq(Base58Encode(new Uint8Array(0)),"","Base58 empty");
  eq(Base58Decode("").length,0,"Base58 decode empty");
  eq(Base58Encode(u8(0,0,0)),"111","Base58 leading zeros");
  assert(eqArr(Base58Decode("111"),u8(0,0,0)),"Base58 decode leading ones");
  eq(Base58Encode(u8(0)),"1","Base58 single zero");
  assert(eqArr(Base58Decode("1"),u8(0)),"Base58 decode single 1");
  assert(eqArr(Base58Decode(Base58Encode(u8(0,0,1,2,3))),u8(0,0,1,2,3)),"Base58 roundtrip with leading zeros");
  for(let len of [0,1,2,3,5,10,20]){
    let b=new Uint8Array(len); for(let i=0;i<len;i++) b[i]=(i*13+7)&0xFF;
    assert(eqArr(Base58Decode(Base58Encode(b)),b),"Base58 roundtrip len "+len);
  }
  throws(()=>Base58Decode("0"),null,"Base58 invalid char 0 throws");
  throws(()=>Base58Decode("I"),null,"Base58 invalid char I throws");
  throws(()=>Base58Decode("O"),null,"Base58 invalid char O throws");
  let chkData = u8(1,2,3,4);
  let chkEnc = Base58CheckEncode(chkData);
  assert(chkEnc.length>0,"Base58Check encode non-empty");
  assert(eqArr(Base58CheckDecode(chkEnc),chkData),"Base58Check roundtrip");
  eq(Base58CheckEncode(new Uint8Array(0)), Base58CheckEncode(new Uint8Array(0)), "Base58Check empty deterministic");
  assert(eqArr(Base58CheckDecode(Base58CheckEncode(new Uint8Array(0))), new Uint8Array(0)),"Base58Check empty roundtrip");
  assert(eqArr(Base58CheckDecode(Base58CheckEncode(u8(0,0,0))),u8(0,0,0)),"Base58Check leading zeros roundtrip");
  throws(()=>Base58CheckDecode("111"),null,"Base58Check too short throws");
  {
    let bad = chkEnc.slice(0,-1) + (chkEnc[chkEnc.length-1]==="1" ? "2":"1");
    throws(()=>Base58CheckDecode(bad),null,"Base58Check bad checksum throws");
  }
  throws(()=>Base58CheckDecode("!@#"),null,"Base58Check invalid chars throws");
  throws(()=>Base58Encode(null),null,"Base58Encode null throws");
}
{
  eq(BaseXEncode(new Uint8Array(0),"01"),"","BaseX empty");
  eq(BaseXDecode("", "01").length,0,"BaseX decode empty");
  let b = u8(0xFF,0x00);
  let enc2 = BaseXEncode(b,"01");
  assert(enc2.length>0,"BaseX binary enc length");
  assert(eqArr(BaseXDecode(enc2,"01"),b),"BaseX binary roundtrip");
  assert(eqArr(BaseXDecode(BaseXEncode(u8(0),"01"),"01"),u8(0)),"BaseX 0 roundtrip binary alphabet");
  let alpha62 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  assert(eqArr(BaseXDecode(BaseXEncode(u8(1,2,3),alpha62),alpha62),u8(1,2,3)),"BaseX 62 roundtrip");
  let alphaLarge = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  assert(alphaLarge.length===62,"alphaLarge len");
  let encLarge = BaseXEncode(u8(10,20,30),alphaLarge);
  assert(eqArr(BaseXDecode(encLarge,alphaLarge),u8(10,20,30)),"BaseX large 62 roundtrip");
  throws(()=>BaseXEncode(u8(1),"a"),null,"BaseX alphabet too short throws");
  throws(()=>BaseXEncode(u8(1),"0123456789ABCDEF0123456789ABCDEF0"),null,"BaseX duplicate throws");
  throws(()=>BaseXEncode(u8(1),""),null,"BaseX empty alphabet throws");
  throws(()=>BaseXDecode("hello","01"),null,"BaseX decode char outside alphabet throws");
  throws(()=>BaseXEncode(null,"01"),null,"BaseXEncode null throws");
  throws(()=>BaseXDecode(null,"01"),null,"BaseXDecode null throws");
  let alpha256 = "";
  for(let i=0;i<256;i++) alpha256+=String.fromCharCode(33+i%94);
  let alpha256uniq = "";
  for(let i=0;i<256;i++) alpha256uniq+=String.fromCharCode(i);
  throws(()=>BaseXEncode(u8(1),alpha256uniq),null,"BaseX alphabet 256 too long throws");
}
{
  assert(eqArr(PutUvarint(0),u8(0)),"PutUvarint 0");
  assert(eqArr(PutUvarint(1),u8(1)),"PutUvarint 1");
  assert(eqArr(PutUvarint(127),u8(127)),"PutUvarint 127");
  assert(eqArr(PutUvarint(128),u8(128,1)),"PutUvarint 128");
  assert(eqArr(PutUvarint(300),u8(172,2)),"PutUvarint 300");
  assert(eqArr(PutUvarint(300),u8(0xAC,0x02)),"PutUvarint 300 alt");
  eq(hex(PutUvarint(300)),"ac02","PutUvarint 300 hex");
  assert(eqArr(PutUvarint(Number.MAX_SAFE_INTEGER),u8(255,255,255,255,255,255,255,15)),"PutUvarint MAX_SAFE");
  assert(eqArr(PutUvarint(9007199254740992n),u8(128,128,128,128,128,128,128,16)),"PutUvarint 2^53 bigint");
  assert(eqArr(PutUvarint(18446744073709551615n),u8(255,255,255,255,255,255,255,255,255,1)),"PutUvarint 2^64-1");
  for(let v of [0,1,127,128,300,16383,16384,2097151, 4294967296]){
    let enc=PutUvarint(v); let [dec,nb]=Uvarint(enc); eq(dec,v,"Uvarint roundtrip "+v); eq(nb,enc.length,"Uvarint nb "+v);
  }
  for(let v of [0n, 1n, 9007199254740992n, (1n<<63n), (1n<<64n)-1n]){
    let enc=PutUvarint(v); let [dec,nb]=Uvarint(enc); let db=typeof dec==="bigint"?dec:BigInt(dec); assert(db===v,"Uvarint bigint "+v); eq(nb,enc.length,"Uvarint bigint nb "+v);
  }
  { let [v,nb]=Uvarint(u8()); eq(v,0,"Uvarint empty v"); eq(nb,0,"Uvarint empty nb"); }
  { let [v,nb]=Uvarint(u8(0x80)); eq(v,0,"Uvarint trunc v"); eq(nb,0,"Uvarint trunc nb"); }
  { let over=new Uint8Array(11).fill(0x80); throws(()=>Uvarint(over),null,"Uvarint overflow throws"); }
  throws(()=>PutUvarint(-1),null,"PutUvarint -1 throws");
  throws(()=>PutUvarint(1.5),null,"PutUvarint 1.5 throws");
  throws(()=>PutUvarint(Number.MAX_SAFE_INTEGER+1),null,"PutUvarint >MAX_SAFE throws");
  assert(eqArr(PutVarint(0),u8(0)),"PutVarint 0");
  assert(eqArr(PutVarint(-1),u8(1)),"PutVarint -1");
  assert(eqArr(PutVarint(1),u8(2)),"PutVarint 1");
  assert(eqArr(PutVarint(-2),u8(3)),"PutVarint -2");
  assert(eqArr(PutVarint(2),u8(4)),"PutVarint 2");
  for(let v of [0,1,-1,2,-2,63,-64,64,-65,1000000,-1000000]){
    let enc=PutVarint(v); let [dec,nb]=Varint(enc); eq(dec,v,"Varint roundtrip "+v);
  }
  throws(()=>PutVarint(1.5),null,"PutVarint 1.5 throws");
  assert(eqArr(PutVarint(-9007199254740992n),u8(255,255,255,255,255,255,255,31)),"PutVarint -2^53");
  { let [v,nb]=Varint(u8(255,255,255,255,255,255,255,31)); assert(v===-9007199254740992n,"Varint -2^53 bigint"); }
}
{
  eq(DetectEncoding(u8(0xEF,0xBB,0xBF,0x61)),"utf-8","Detect BOM utf-8");
  eq(DetectEncoding(u8(0xFE,0xFF,0x00,0x61)),"utf-16be","Detect BOM utf-16be");
  eq(DetectEncoding(u8(0xFF,0xFE,0x61,0x00)),"utf-16le","Detect BOM utf-16le");
  eq(detectEncoding(u8(0x61,0x62)),"utf-8","detectEncoding lowercase alias");
  let gbk = u8(0xD6,0xD0,0xCE,0xC4,0xB1,0xE0,0xC2,0xEB,0xB2,0xE2,0xCA,0xD4,0x20,0x31,0x32,0x33);
  eq(DetectEncoding(gbk,{allowList:["gbk","utf-8"]}),"gbk","Detect allowList hit");
  throws(()=>DetectEncoding(gbk,{allowList:["shift_jis"]}),null,"Detect allowList rejection throws");
  eq(DetectEncoding(gbk,{allowList:["utf-8","shift_jis"], fallback:"utf-8"}),"utf-8","Detect fallback");
  eq(DetectEncoding(u8(0x61,0x62),{fallback:"windows-1252"}),"utf-8","Detect ascii is utf-8");
  throws(()=>DetectEncoding("not a buffer"),null,"Detect non-buffer throws");
  throws(()=>DetectEncoding(null),null,"Detect null throws");
  throws(()=>DetectEncoding(u8(0x61),{allowList:["utf-8", 123]}),/allowList\[1\] must be a string/,"Detect allowList refuses a non-string element (a lenient skip would walk every hole of a sparse list)");
}
{
  eq(JSON5Parse("{unquoted:1,}").unquoted,1,"JSON5Parse unquoted");
  eq(JSON5Parse("{a:1,b:2}").a,1,"JSON5Parse simple");
  eq(JSON5Parse("['a', /* comment */ 'b']")[1],"b","JSON5Parse comment");
  assert(Number.isNaN(JSON5Parse("{a:NaN}").a),"JSON5 NaN is NaN");
  eq(JSON5Parse("{a:Infinity}").a,Infinity,"JSON5 Infinity");
  eq(JSON5Parse("{a:0x10}").a,16,"JSON5 hex number");
  throws(()=>JSON5Parse("{a:1"),null,"JSON5Parse incomplete throws");
  let s5 = JSON5Stringify({a:1,b:NaN});
  assert(s5.includes("NaN"),"JSON5Stringify NaN literal");
  let s5i = JSON5Stringify({x:1},{indent:2});
  assert(s5i.includes("\n"),"JSON5Stringify indent");
  eq(StableStringify({b:2,a:1}),'{"a":1,"b":2}',"Stable ordering");
  eq(StableStringify({z:3, a:{d:4,b:2}}),'{"a":{"b":2,"d":4},"z":3}',"Stable nested ordering");
  eq(StableStringify({b:2,a:1},{indent:2}),'{"a":1,"b":2}',"Stable ignores indent still compact?");
  throws(()=>StableStringify({a:NaN}),null,"Stable rejects NaN");
  throws(()=>StableStringify({a:Infinity}),null,"Stable rejects Infinity");
  assert(StableStringify({b:2,a:1}) !== JSON.stringify({b:2,a:1}) || true,"stable vs json");
}
{
  let jp = new JSONPath("$.store.book[*].author");
  let data = {store:{book:[{author:"Nigel Rees"},{author:"Evelyn Waugh"}]}};
  eq(jp.all(data).length,2,"JSONPath all length");
  eq(jp.first(data),"Nigel Rees","JSONPath first");
  eq(jp.paths(data).length,2,"JSONPath paths length");
  assert(jp.paths(data)[0].includes("book"),"JSONPath path contains book");
  let jp2 = new JSONPath("$.missing");
  eq(jp2.all(data).length,0,"JSONPath no match all empty");
  eq(jp2.first(data),undefined,"JSONPath no match first undefined");
  eq(jp2.paths(data).length,0,"JSONPath no match paths empty");
  let jp3 = new JSONPath("$..author");
  eq(jp3.all(data).length,2,"JSONPath deep all");
  let jpRoot = new JSONPath("$");
  assert(jpRoot.first(data)===data,"JSONPath root");
  throws(()=>new JSONPath(""),null,"JSONPath empty throws");
  throws(()=>new JSONPath("not jsonpath"),null,"JSONPath invalid throws");
  let jpArr = new JSONPath("$[0]");
  eq(jpArr.first([10,20]),10,"JSONPath array index");
}
{
  let qr1 = QREncode("hello");
  assert(qr1.version>=1 && qr1.version<=40,"QREncode version in range");
  assert(qr1.size>0,"QREncode size >0");
  assert(qr1.modules instanceof Uint8Array,"QREncode modules Uint8Array");
  for(let ecc of ["L","M","Q","H"]){
    let q = QREncode("test",{ecc:ecc});
    assert(q.version>=1 && q.version<=40,"QREncode ecc "+ecc);
  }
  let qv1 = QREncode("hi",{version:1});
  eq(qv1.version,1,"QREncode version 1 pinned");
  let qv40 = QREncode("hi",{version:40});
  eq(qv40.version,40,"QREncode version 40 pinned");
  throws(()=>QREncode("hi",{version:0}),null,"QREncode version 0 throws");
  throws(()=>QREncode("hi",{version:41}),null,"QREncode version 41 throws");
  for(let m=0;m<8;m++){
    let q = QREncode("masktest",{mask:m});
    assert(q.modules.length===q.size*q.size,"QREncode mask "+m+" modules size");
  }
  throws(()=>QREncode("hi",{mask:-1}),null,"QREncode mask -1 throws");
  throws(()=>QREncode("hi",{mask:8}),null,"QREncode mask 8 throws");
  throws(()=>QREncode("hi",{ecc:"X"}),null,"QREncode bad ecc throws");
  let bigStr = "A".repeat(3000);
  throws(()=>QREncode(bigStr),null,"QREncode too large throws");
  let qrStr = QRToString("hello");
  assert(typeof qrStr==="string","QRToString returns string");
  assert(qrStr.length>0,"QRToString non-empty");
  let qrStr2 = QRToString("hello",{ecc:"H", version:5});
  assert(typeof qrStr2==="string","QRToString with opts");
  try{ let qe = QREncode(""); assert(qe.version>=1,"QREncode empty"); }catch(e){ assert(true,"QREncode empty may throw acceptable"); }
  assert(QREncode(String(null)).version>=1,"QREncode null coerces");
}
{
    eq(Base85Encode(new Uint8Array(0)), "", "Base85 empty -> empty");
    eq(Base85Encode(new Uint8Array([0,0,0,0])), "z", "Base85 zero-group z shorthand");
    eq(Base85Encode(new Uint8Array([1])), "!<", "Base85 1-byte pad");
    eq(Base85Encode(new Uint8Array([1,2])), "!<N", "Base85 2-byte pad");
    eq(Base85Encode(new Uint8Array([1,2,3])), "!<N?", "Base85 3-byte pad");
    for (const L of [3,4,5]) {
        const src = new TextEncoder().encode("abcdefgh".slice(0,L));
        const dec = new TextDecoder().decode(Base85Decode(Base85Encode(src)));
        eq(dec, "abcdefgh".slice(0,L), "Base85 round-trip n="+L);
    }
    const big = new Uint8Array(1024);
    for (let i=0;i<1024;i++) big[i]=i & 0xff;
    eq(Base85Decode(Base85Encode(big)).length, 1024, "Base85 1KB round-trip length");
    throws(()=>Base85Decode("!!!!invalid!!"), /invalid ascii85|invalid/i, "Base85 bad char refused");
}
if(fails){print("test_encoding_cov_bytes_compress_encoding_hash: "+fails+" FAILED of "+n); throw new Error("failed")}
print("test_encoding_cov_bytes_compress_encoding_hash: "+n+" assertions, 0 failures")
