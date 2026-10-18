// Split from tests/test_cov_bytes_compress_encoding_hash.js: the yaml sections.
import { MsgPackEncode as MPE, MsgPackDecode as MPD, CBOREncode as CBE, CBORDecode as CBD } from "dyna:serialize";
import { Patch } from "dyna:json";
import { XMLStringify } from "dyna:xml";
import { HTMLStringify } from "dyna:html";
import { Parse as YamlParse2 } from "dyna:yaml";
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
    throws(()=>YamlParse2("a: 1\na: 2"), /duplicate/i, "yaml dup block key");
    throws(()=>YamlParse2("{a: 1, a: 2}"), /duplicate/i, "yaml dup flow key");
    eq(YamlParse2("__proto__: x\nb: 1").b, 1, "__proto__ doc still parses (own-check)");
    throws(()=>YamlParse2('x: "\\uD800"'), /surrogate/i, "lone high surrogate");
    throws(()=>YamlParse2('x: "\\uDC00"'), /surrogate/i, "lone low surrogate");
    eq(YamlParse2('x: "\\uD83D\\uDE00"').x, "\u{1F600}", "surrogate pair joins");

    {
        const v = 9223372036840000000;
        assert(MPD(MPE(v)) === v || Math.abs(MPD(MPE(v)) - v) < 1000, "msgpack u63+ stays positive");
        assert(CBD(CBE(v)) === v || Math.abs(CBD(CBE(v)) - v) < 1000, "cbor u63+ stays positive");
    }

    {
        const huge=[]; huge.length = 5000000;
        throws(()=>Patch.apply({}, [{op:"add",path:"/x",value:huge}]), /too long|clone/i, "patch clone cap");
        const out=Patch.apply([1,2,3], [{op:"add",path:"/-",value:4}]);
        eq(out.length, 4, "patch normal add unaffected");
    }

    throws(()=>XMLStringify({name:"div", attrs:{'x" onload="alert(1)':"y"}}), /invalid attribute name/i, "xml attr injection");
    throws(()=>XMLStringify({name:'img src=x'}), /valid element name/i, "xml elem injection");
    assert(XMLStringify({name:"div", attrs:{"class":"ok"}}).includes('<div class="ok"'), "xml valid ok");
    throws(()=>HTMLStringify({name:"div", attrs:{'x" onclick="p':"y"}}), /invalid attribute name|onclick/i, "html attr injection");
    throws(()=>HTMLStringify({name:"s cript"}), /valid element name|name/i, "html elem space refused");
}
if(fails){print("test_yaml_cov_bytes_compress_encoding_hash: "+fails+" FAILED of "+n); throw new Error("failed")}
print("test_yaml_cov_bytes_compress_encoding_hash: "+n+" assertions, 0 failures")
