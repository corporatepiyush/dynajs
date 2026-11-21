// covers: dyna:oauth2.buildAuthorizationUrl
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_oauth2_build_authorization_url");
import * as o from "dyna:oauth2";
import * as crypto from "dyna:crypto";
import { Base64URLEncode } from "dyna:encoding";
let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function assertThrows(fn, msg) { n++; let threw=false; try{fn();}catch(e){threw=true;} if(!threw) throw new Error("expected throw: "+msg); }
function assertEq(a,b,msg){ n++; if(a!==b) throw new Error(msg+": expected "+JSON.stringify(b)+" got "+JSON.stringify(a)); }

__kitS.section("read write", () => {
    let ch = o.generateCodeChallenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk","S256");
    let u = o.buildAuthorizationUrl({authorizationEndpoint:"https://auth.example.com/authorize", clientId:"s6BhdRkqt3", redirectUri:"https://client.example.com/cb", scope:"read write", state:"xyz", codeChallenge:ch, codeChallengeMethod:"S256"});
    assert(u.url.includes("response_type=code") && u.url.includes("client_id=s6BhdRkqt3") && u.url.includes("code_challenge="+ch), "buildAuthUrl");
    assert(u.state==="xyz", "state echo");
    let u2 = o.buildAuthorizationUrl({authorizationEndpoint:"https://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb"});
    assert(typeof u2.state==="string" && u2.state.length>=43, "auto state");
    let u3 = o.buildAuthorizationUrl({authorizationEndpoint:"https://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb", extraParams:{client_id:"evil"}});
    assert(!u3.url.includes("evil"), "extraParams collision rejected");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"http://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb"}), "http endpoint without allowInsecure throws");
    let u4 = o.buildAuthorizationUrl({authorizationEndpoint:"http://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb", allowInsecure:true});
    assert(u4.url.startsWith("http://"), "allowInsecure true");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb", scope:"read\nwrite"}), "scope CRLF throws");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb", responseType:"token"}), "implicit throws");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb", codeChallenge:"short", codeChallengeMethod:"S256"}), "short challenge throws");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb", codeChallenge:"a".repeat(43), codeChallengeMethod:"plain"}), "plain without allowPlain throws");
    let u5 = o.buildAuthorizationUrl({authorizationEndpoint:"https://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb", codeChallenge:"a".repeat(43), codeChallengeMethod:"plain", allowPlain:true});
    assert(u5.url.includes("code_challenge_method=plain"), "plain with allowPlain");
});

__kitS.section("extraParams CRLF value throws", () => {
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb", extraParams:{"x":"a\nb"}}), "extraParams CRLF value throws");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb", extraParams:{"a\nb":"x"}}), "extraParams CRLF key throws");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb", extraParams:{num:42}}), "extraParams non-string value throws");
    assert(o.parseBearerFromRequest({headers:{}, query:"access_token=abc", allowQuery:false})===null, "query not allowed");
});

__kitS.section("long endpoint preserved verbatim", () => {
    let ep = "https://auth.example.com/authorize/" + "x".repeat(5000);
    let u = o.buildAuthorizationUrl({authorizationEndpoint: ep, clientId:"id", redirectUri:"https://client.example.com/cb"});
    assert(u.url.startsWith(ep), "long endpoint preserved verbatim");
    assert(u.url.includes("response_type=code") && u.url.includes("client_id=id"), "long endpoint params present");
    assertEq(u.url.indexOf(ep)+ep.length, u.url.indexOf("?response_type=code"), "params directly after long endpoint");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://a/", clientId:"id", redirectUri:"https://c/d", scope:" read"}), "scope leading space throws");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://a/", clientId:"id", redirectUri:"https://c/d", scope:"a  b"}), "scope double space throws");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://a/", clientId:"id", redirectUri:"https://c/d", scope:" "}), "scope lone space throws");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://a/", clientId:"id", redirectUri:"https://c/d", scope:""}), "scope empty throws");
    let us = o.buildAuthorizationUrl({authorizationEndpoint:"https://a/", clientId:"id", redirectUri:"https://c/d", scope:"read write"});
    assert(us.url.includes("scope=read+write"), "scope two tokens ok");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"", clientId:"id", redirectUri:"https://c/d"}), "empty endpoint throws");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://a/", clientId:"", redirectUri:"https://c/d"}), "empty clientId throws");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://a/", clientId:"id", redirectUri:""}), "empty redirectUri throws");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://a/", clientId:"id", redirectUri:"https://c/d", codeChallenge:"a".repeat(44), codeChallengeMethod:"S256"}), "S256 challenge 44 throws");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://a/", clientId:"id", redirectUri:"https://c/d", codeChallenge:"a".repeat(42), codeChallengeMethod:"S256"}), "S256 challenge 42 throws");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://a/", clientId:"id", redirectUri:"https://c/d", codeChallenge:"a!b".padEnd(43,"c"), codeChallengeMethod:"S256"}), "challenge bad char throws");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://a/", clientId:"id", redirectUri:"https://c/d", codeChallenge:"a".repeat(42), codeChallengeMethod:"plain", allowPlain:true}), "plain challenge 42 throws");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://a/", clientId:"id", redirectUri:"https://c/d", codeChallenge:"a".repeat(129), codeChallengeMethod:"plain", allowPlain:true}), "plain challenge 129 throws");
    let uc = o.buildAuthorizationUrl({authorizationEndpoint:"https://a/", clientId:"id", redirectUri:"https://c/d", codeChallenge:"a".repeat(44), codeChallengeMethod:"plain", allowPlain:true});
    assert(uc.url.includes("code_challenge="+"a".repeat(44)), "plain challenge 44 ok");
});

__kitS.section("F1 no raw & injection via key", () => {
    let us1 = o.buildAuthorizationUrl({authorizationEndpoint:"https://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb", extraParams:{"a&admin=true":"1"}});
    assert(!us1.url.includes("&admin=true"), "F1 no raw & injection via key");
    assert(us1.url.includes("a%26admin%3Dtrue=1"), "F1 key form-encoded");
    let us2 = o.buildAuthorizationUrl({authorizationEndpoint:"https://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb", extraParams:{"a=b":"1"}});
    assert(!us2.url.includes("a=b=1"), "F1 no raw = param split via key");
    assert(us2.url.includes("a%3Db=1"), "F1b key = encoded");
    let us3 = o.buildAuthorizationUrl({authorizationEndpoint:"https://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb", extraParams:{"x":"a&b=c"}});
    assert(us3.url.includes("x=a%26b%3Dc"), "F1c value still encoded");

    {
        let calls = 0;
        let arr = ["read"];
        Object.defineProperty(arr, 0, { get() { if (calls++ === 0) return "read"; return "admin injected"; } });
        assertEq(o.formatScope(arr), "read", "F2 getter array single fetch");
    }
    {
        let arr = ["read", (function(){ let arr2=["ok"]; Object.defineProperty(arr2,0,{get(){return "b\tad";}}); return arr2; })()[0]];
        assertThrows(()=>o.formatScope(arr), "F2 invalid token still refused");
    }

    assertEq(o.parseBearerFromRequest({headers:{"AUTHORIZATION":"Bearer tA"}}), "tA", "F3 upper");
    assertEq(o.parseBearerFromRequest({headers:{"aUtHoRiZaTiOn":"Bearer tB"}}), "tB", "F3 mixed");
    assertEq(o.parseBearerFromRequest({headers:{authorization:"Bearer tC"}}), "tC", "F3 lower still works");
    assertEq(o.parseBearerFromRequest({headers:{"x-authorization":"Bearer tD"}}), null, "F3 near-name ignored");

    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb", state:""}), "F4 empty state throws");
    let us4 = o.buildAuthorizationUrl({authorizationEndpoint:"https://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb"});
    assert(typeof us4.state==="string" && us4.state.length===43, "F4 auto state still works");

    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb", extraParams:{n:42}}), "F5 non-string throws");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb", extraParams:{"k\n":"v"}}), "F5 CTL key throws");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb", extraParams:{k:"v\r"}}), "F5 CTL value throws");
    let us5 = o.buildAuthorizationUrl({authorizationEndpoint:"https://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb", extraParams:{client_id:"evil"}});
    assert(!us5.url.includes("evil"), "F5/P6 collision still dropped");
});
__kitS.section("extraParams key holding NUL is a CTL and throws (fuzz_oauth2 2026-11-27)", () => {
    const base = { authorizationEndpoint: "https://auth.example.com/a", clientId: "c", redirectUri: "https://client.example.com/cb", state: "s" };
    assertThrows(() => o.buildAuthorizationUrl(Object.assign({}, base, { extraParams: { ["evil\0"]: "v" } })), "NUL key");
    assertThrows(() => o.buildAuthorizationUrl(Object.assign({}, base, { extraParams: { ["state\0x"]: "v" } })), "NUL key shadowing a reserved name");
});
__kitS.end();

print("test_oauth2: all "+n+" tests passed");
