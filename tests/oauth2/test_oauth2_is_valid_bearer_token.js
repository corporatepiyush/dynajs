// covers: dyna:oauth2.isValidBearerToken
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_oauth2_is_valid_bearer_token");
import * as o from "dyna:oauth2";
import * as crypto from "dyna:crypto";
import { Base64URLEncode } from "dyna:encoding";
let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function assertThrows(fn, msg) { n++; let threw=false; try{fn();}catch(e){threw=true;} if(!threw) throw new Error("expected throw: "+msg); }
function assertEq(a,b,msg){ n++; if(a!==b) throw new Error(msg+": expected "+JSON.stringify(b)+" got "+JSON.stringify(a)); }

__kitS.section("== pure padding false", () => {
    assert(!o.isValidBearerToken("=="), "== pure padding false");
    assert(!o.isValidBearerToken("="), "= false");
    assert(o.isValidBearerToken("a"), "single char true");
    assert(!o.isValidBearerToken("a!"), "! false");
    assert(o.isValidBearerToken("a".repeat(4097)), "4097 true: RFC 6750 b64token has no length cap");
    assert(!o.isValidBearerToken("a".repeat(4097), {strict:true}), "4097 false under strict");
    assert(o.isValidBearerToken("a".repeat(4096), {strict:true}), "4096 true under strict");
    assert(o.isValidBearerToken("a".repeat(4097), {strict:false}), "strict:false is the RFC default");
    assert(o.isValidBearerToken("a==="), "three trailing = true: RFC 6750 *\"=\"");
    assert(!o.isValidBearerToken("a===", {strict:true}), "three trailing = false under strict");
    assert(o.isValidBearerToken("a==", {strict:true}), "two trailing = true under strict");
    assert(!o.isValidBearerToken("===="), "pure padding false");
    assert(!o.isValidBearerToken("a=b="), "interior = false");
    assertThrows(()=>o.isValidBearerToken("a", {strict:1}), "strict non-boolean throws");
    assertThrows(()=>o.isValidBearerToken("a", {strcit:true}), "unknown option throws");
    let allUnreserved = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    for(let c of allUnreserved) assert(o.isValidCodeVerifier(c.repeat(43)), "unreserved "+c);
    assert(!o.isValidCodeVerifier("a".repeat(42)+"*"), "* not unreserved");
    let v = o.generateCodeVerifier();
    let u = o.buildAuthorizationUrl({authorizationEndpoint:"https://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb", codeChallenge:v});
    assert(u.url.includes("code_challenge="+v) && u.url.includes("code_challenge_method=S256"), "default S256 when challenge without method");
    assertThrows(()=>o.generateCodeChallenge(v,"s256"), "lower s256 throws");
    assertThrows(()=>o.generateCodeChallenge(v,"S256 "), "trailing space throws");
    let dv = new DataView(new Uint8Array([0,1,2,3,4]).buffer, 1, 3);
    let dv2 = new Uint8Array([1,2,3]);
    assert(o.secureCompare(dv, dv2), "DataView vs Uint8Array true");
    assert(!o.secureCompare(dv, new Uint8Array([1,2,4])), "DataView false");
    let ab1 = new Int8Array([1,2,3]);
    assert(o.secureCompare(ab1, dv2), "Int8Array true");
    assert(o.parseScope("! # [ ] ^ ~").length===6, "NQCHAR boundaries");
    assertThrows(()=>o.parseScope('a"b'), "quote in scope via NQCHAR");
    assertThrows(()=>o.parseScope('a\\b'), "backslash via NQCHAR");
    let w1 = o.buildWWWAuthenticate({realm:"ex", error:"invalid_token", errorDescription:"expired", errorUri:"https://ex.com", scope:"read"});
    assert(w1.includes('realm="ex"') && w1.includes('error="invalid_token"') && w1.includes('error_description="expired"'), "www all");
    let w2 = o.buildWWWAuthenticate({error:"invalid_token"});
    assert(w2==='Bearer error="invalid_token"', "www error only");
    assertThrows(()=>o.parseBearerFromRequest({headers:{authorization:"Bearer t1"}, query:"access_token=t2", body:"access_token=t3", allowQuery:true, allowBody:true}), "triple methods throws");
    assert(!o.isValidRedirectUri("https://user:pass@client.example.com/cb", ["https://client.example.com/cb"]), "userinfo mismatch false");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://a/b", clientId:"id"}), "missing redirectUri throws");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://a/b", redirectUri:"https://c/d"}), "missing clientId throws");
    let tb = o.buildTokenRequestBody({k:"a b", v:"c+d"});
    assert(tb.includes("k=a+b") && tb.includes("v=c%2Bd"), "token body encoding");
    assert(o.parseTokenResponse('{"access_token":"a","token_type":"BEARER"}').token_type==="BEARER", "BEARER upper");
    let h = o.buildClientAuthHeader("id", "a:b");
    assert(h.startsWith("Basic "), "client auth colon secret");
    assert(!o.isValidBearerToken("a\0b"), "NUL bearer false");
    assert(!o.isValidRedirectUri("https://a/b\0evil", ["https://a/b"]), "NUL redirect false");
    let uProto = o.buildAuthorizationUrl({authorizationEndpoint:"https://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb", extraParams:{constructor:"evil", scope:"evil"}});
    assert(!uProto.url.includes("evil_scope"), "extra scope pollution rejected");
});
__kitS.end();

print("test_oauth2: all "+n+" tests passed");
