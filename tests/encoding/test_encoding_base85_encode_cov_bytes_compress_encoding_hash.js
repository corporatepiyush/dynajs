// covers: dyna:encoding.Base85Encode
// Split from tests/test_cov_bytes_compress_encoding_hash.js: the encoding sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_base85_encode_cov_bytes_compress_encoding_hash");
import { HexEncode, HexDecode, Base64Encode, Base64Decode, Base64URLEncode, Base64URLDecode, Base32Encode, Base32Decode, Base32HexEncode, Base32HexDecode, Base85Encode, Base85Decode, Base58Encode, Base58Decode, Base58CheckEncode, Base58CheckDecode, BaseXEncode, BaseXDecode, PutUvarint, Uvarint, PutVarint, Varint, DetectEncoding, detectEncoding, JSON5Parse, JSON5Stringify, StableStringify, JSONPath, QREncode, QRToString } from "dyna:encoding";
import * as CryptoNS from "dyna:crypto";

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
print("compress done: "+n+" assertions");

__kitS.section("Base85 empty -> empty", () => {
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
});
__kitS.end();
if(fails){print("test_encoding_cov_bytes_compress_encoding_hash: "+fails+" FAILED of "+n); throw new Error("failed")}
print("test_encoding_cov_bytes_compress_encoding_hash: "+n+" assertions, 0 failures")
