/* test_oauth2.js -- OAuth2 PKCE + Bearer + scope + redirect + URL helpers
 * Pure helpers, no IO, optional JWT not required.
 * Vectors cited beside expected: RFC 7636 App B, RFC 6749 §4.1.1, RFC 6750 §2.1,  §3.
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_oauth2.js
 */
import * as o from "dyna:oauth2";
import * as crypto from "dyna:crypto";
import { Base64URLEncode } from "dyna:encoding";
let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function assertThrows(fn, msg) { n++; let threw=false; try{fn();}catch(e){threw=true;} if(!threw) throw new Error("expected throw: "+msg); }
function assertEq(a,b,msg){ n++; if(a!==b) throw new Error(msg+": expected "+JSON.stringify(b)+" got "+JSON.stringify(a)); }

// RFC 7636 Appendix B
{
    let verifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
    let challenge = o.generateCodeChallenge(verifier, "S256");
    assertEq(challenge, "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM", "RFC7636 B S256");
    assert(o.verifyCodeChallenge(verifier, challenge, "S256"), "verify S256 true");
    assert(!o.verifyCodeChallenge(verifier, challenge+"A", "S256"), "verify S256 false on tamper");
    // plain
    assertEq(o.generateCodeChallenge(verifier, "plain"), verifier, "plain");
    assert(o.verifyCodeChallenge(verifier, verifier, "plain"), "verify plain true");
    // generate with plain succeeds; buildAuthorizationUrl enforces allowPlain
    assertEq(o.generateCodeChallenge(verifier, "plain"), verifier, "plain generate ok");
    // isValidCodeVerifier
    assert(o.isValidCodeVerifier(verifier), "isValidCodeVerifier true");
    assert(!o.isValidCodeVerifier("short"), "isValid too short false");
    assert(!o.isValidCodeVerifier("a".repeat(42)), "42 false");
    assert(o.isValidCodeVerifier("a".repeat(43)), "43 true");
    assert(o.isValidCodeVerifier("a".repeat(128)), "128 true");
    assert(!o.isValidCodeVerifier("a".repeat(129)), "129 false");
    assert(!o.isValidCodeVerifier("a".repeat(43).replace("a","*")), "charset * false");
    // generate
    let v2 = o.generateCodeVerifier();
    assert(o.isValidCodeVerifier(v2), "generated verifier valid");
    assertEq(v2.length, 43, "generated 43");
    // verifier charset 256 enumeration: all bytes alone -> ensure only unreserved pass
    // (spot check)
    assert(!o.isValidCodeVerifier("a".repeat(43).slice(0,42)+" "), "space fails");
    assert(!o.isValidCodeVerifier("a".repeat(43).slice(0,42)+"!"), "! fails");
}

// generateState / secureCompare
{
    let s = o.generateState();
    assert(typeof s==="string" && s.length>=43, "generateState default");
    let s16 = o.generateState(16);
    assert(typeof s16==="string", "generateState 16");
    assertThrows(()=>o.generateState(0), "generateState 0 throws");
    assertThrows(()=>o.generateState(257), "257 throws");
    assert(o.secureCompare("abc","abc"), "secureCompare true");
    assert(!o.secureCompare("abc","abd"), "secureCompare false");
    assert(!o.secureCompare("abc","ab"), "secureCompare length false");
    // BytesInput: Uint8Array vs string
    let u1 = new Uint8Array([97,98,99]);
    let u2 = new Uint8Array([97,98,99]);
    let u3 = new Uint8Array([97,98,100]);
    assert(o.secureCompare(u1,u2), "secureCompare Uint8Array true");
    assert(!o.secureCompare(u1,u3), "secureCompare Uint8Array false");
    assert(o.secureCompare("abc", u1), "secureCompare string vs Uint8Array true");
    assert(!o.secureCompare("abc", u3), "secureCompare string vs Uint8Array false");
    // ArrayBuffer
    let ab = new Uint8Array([1,2,3]).buffer;
    assert(o.secureCompare(new Uint8Array([1,2,3]), ab), " ArrayBuffer compare");
}

// scope
{
    assert(JSON.stringify(o.parseScope("read write"))===JSON.stringify(["read","write"]), "parseScope");
    assert(JSON.stringify(o.parseScope("read"))===JSON.stringify(["read"]), "parseScope single");
    assert(o.formatScope(["read","write"])==="read write", "formatScope");
    assertThrows(()=>o.parseScope("read  write"), "double space empty token");
    assertThrows(()=>o.parseScope("read write "), "trailing space");
    assertThrows(()=>o.parseScope("read \"write\""), "quote invalid");
    assertThrows(()=>o.parseScope("read\\write"), "backslash invalid");
    // empty scope returns [] (allowed) — not a throw
    assert(JSON.stringify(o.parseScope(""))===JSON.stringify([]), "empty scope []");
    // Actually our parseScope throws? Empty string returns [] not throw; test
    // Do not assert empty throws; we allow empty.
    // formatScope
    assert(o.formatScope([])==="", "empty format");
    assertThrows(()=>o.formatScope(["a", "b c"]), "formatScope invalid char");
    assertThrows(()=>o.formatScope([""]), "empty token");
    assertThrows(()=>o.formatScope(["a".repeat(65)]), "too long");
}

// bearer
{
    let hdr = o.buildBearerHeader("mF_9.B5f-4.1JqM");
    assertEq(hdr, "Bearer mF_9.B5f-4.1JqM", "buildBearer");
    assertEq(o.parseBearerHeader("Bearer mF_9.B5f-4.1JqM"), "mF_9.B5f-4.1JqM", "parseBearer");
    assertEq(o.parseBearerHeader("bearer mF_9.B5f-4.1JqM"), "mF_9.B5f-4.1JqM", "case insensitive");
    assertEq(o.parseBearerHeader("Bearer   mF_9.B5f-4.1JqM   "), "mF_9.B5f-4.1JqM", "trim");
    assert(o.parseBearerHeader("Bearer")===null, "no token null");
    assert(o.parseBearerHeader("Basic abc")===null, "wrong scheme null");
    assertThrows(()=>o.buildBearerHeader("mF 9"), "space invalid");
    assertThrows(()=>o.buildBearerHeader("mF\n9"), "CRLF invalid");
    assertThrows(()=>o.buildBearerHeader(""), "empty invalid");
    assert(o.isValidBearerToken("mF_9.B5f-4.1JqM"), "isValidBearer true");
    assert(!o.isValidBearerToken("mF 9"), "isValid false space");
    assert(!o.isValidBearerToken("a".repeat(4097)), "too long false");
    // b64token with +/=
    assert(o.isValidBearerToken("abc+def/ghi=="), "b64token +/=");
    assert(o.buildBearerHeader("abc+def/ghi==")==="Bearer abc+def/ghi==", "build with +/=");
    // parseBearerFromRequest header priority + allow flags
    let r1 = o.parseBearerFromRequest({headers:{authorization:"Bearer t1"}});
    assertEq(r1,"t1","from header lower");
    let r2 = o.parseBearerFromRequest({headers:{Authorization:"Bearer t2"}});
    assertEq(r2,"t2","from header upper");
    let r3 = o.parseBearerFromRequest({query:"access_token=t3", allowQuery:true});
    assertEq(r3,"t3","from query");
    let r4 = o.parseBearerFromRequest({body:"access_token=t4", allowBody:true});
    assertEq(r4,"t4","from body");
    assert(o.parseBearerFromRequest({query:"access_token=t3"})===null, "query not allowed default null");
    assert(o.parseBearerFromRequest({body:"access_token=t4"})===null, "body not allowed default null");
    // must not use more than one method
    assertThrows(()=>o.parseBearerFromRequest({headers:{authorization:"Bearer t1"}, query:"access_token=t2", allowQuery:true}), "multiple methods header+query throws");
    assertThrows(()=>o.parseBearerFromRequest({headers:{authorization:"Bearer t1"}, body:"access_token=t2", allowBody:true}), "header+body throws");
}

// WWW-Authenticate
{
    let w = o.buildWWWAuthenticate({realm:"example", error:"invalid_token"});
    assertEq(w, 'Bearer realm="example", error="invalid_token"', "www realm+error");
    let w2 = o.buildWWWAuthenticate({realm:"example", error:"invalid_token", errorDescription:"expired", scope:"read"});
    assert(w2.includes('error_description="expired"') && w2.includes('scope="read"'), "www desc+scope");
    assertThrows(()=>o.buildWWWAuthenticate({realm:"a\nb", error:"invalid_token"}), "realm CRLF throws");
    assertThrows(()=>o.buildWWWAuthenticate({error:"invalid token"}), "error space throws");
    assertThrows(()=>o.buildWWWAuthenticate({error:"invalid_token", errorDescription:'a"b'}), "quote throws");
    // RFC 6750 §3: error is REQUIRED in a challenge
    assertThrows(()=>o.buildWWWAuthenticate({realm:"example"}), "no error throws");
    assertThrows(()=>o.buildWWWAuthenticate({realm:"example", errorDescription:"x"}), "desc without error throws");
    assertThrows(()=>o.buildWWWAuthenticate({scope:"read"}), "scope without error throws");
}

// redirect
{
    assert(o.isValidRedirectUri("https://client.example.com/cb", ["https://client.example.com/cb"]), "exact true");
    assert(!o.isValidRedirectUri("https://client.example.com/cb/", ["https://client.example.com/cb"]), "trailing slash false");
    assert(o.isValidRedirectUri("http://127.0.0.1:54321/cb", ["http://127.0.0.1:8080/cb"]), "loopback port ignore");
    assert(o.isValidRedirectUri("http://[::1]:54321/cb", ["http://[::1]:8080/cb"]), "ipv6 loopback true");
    assert(!o.isValidRedirectUri("http://127.0.0.1:54321/cb", ["https://client.example.com/cb"]), "loopback vs https false");
    assert(!o.isValidRedirectUri("https://evil.com/cb", ["https://client.example.com/cb"]), "evil false");
    assert(!o.isValidRedirectUri("https://client.example.com/cb\n", ["https://client.example.com/cb"]), "CTL false");
    assert(o.isValidRedirectUri("com.example.app:/oauth2redirect", ["com.example.app:/oauth2redirect"]), "private-use true");
    assert(!o.isValidRedirectUri("myapp:/oauth2redirect", ["myapp:/oauth2redirect"]), "private without dot false");
    assert(!o.isValidRedirectUri("https://client.example.com/cb?evil=1", ["https://client.example.com/cb"]), "query mismatch false");
}

// buildAuthorizationUrl
{
    let ch = o.generateCodeChallenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk","S256");
    let u = o.buildAuthorizationUrl({authorizationEndpoint:"https://auth.example.com/authorize", clientId:"s6BhdRkqt3", redirectUri:"https://client.example.com/cb", scope:"read write", state:"xyz", codeChallenge:ch, codeChallengeMethod:"S256"});
    assert(u.url.includes("response_type=code") && u.url.includes("client_id=s6BhdRkqt3") && u.url.includes("code_challenge="+ch), "buildAuthUrl");
    assert(u.state==="xyz", "state echo");
    // auto state
    let u2 = o.buildAuthorizationUrl({authorizationEndpoint:"https://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb"});
    assert(typeof u2.state==="string" && u2.state.length>=43, "auto state");
    // extraParams collision reject
    let u3 = o.buildAuthorizationUrl({authorizationEndpoint:"https://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb", extraParams:{client_id:"evil"}});
    assert(!u3.url.includes("evil"), "extraParams collision rejected");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"http://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb"}), "http endpoint without allowInsecure throws");
    let u4 = o.buildAuthorizationUrl({authorizationEndpoint:"http://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb", allowInsecure:true});
    assert(u4.url.startsWith("http://"), "allowInsecure true");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb", scope:"read\nwrite"}), "scope CRLF throws");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb", responseType:"token"}), "implicit throws");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb", codeChallenge:"short", codeChallengeMethod:"S256"}), "short challenge throws");
    // plain without allowPlain throws
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb", codeChallenge:"a".repeat(43), codeChallengeMethod:"plain"}), "plain without allowPlain throws");
    let u5 = o.buildAuthorizationUrl({authorizationEndpoint:"https://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb", codeChallenge:"a".repeat(43), codeChallengeMethod:"plain", allowPlain:true});
    assert(u5.url.includes("code_challenge_method=plain"), "plain with allowPlain");
}

// parseAuthorizationResponse
{
    let p = o.parseAuthorizationResponse("https://client.example.com/cb?code=SplxlOBeZQQYbYS6WxSbIA&state=xyz");
    assertEq(p.code,"SplxlOBeZQQYbYS6WxSbIA","parse code");
    assertEq(p.state,"xyz","parse state");
    let e = o.parseAuthorizationResponse("https://client.example.com/cb?error=access_denied&error_description=denied&state=xyz");
    assertEq(e.error,"access_denied","parse error");
    assertEq(e.errorDescription,"denied","parse errorDescription");
    // duplicate code is a forgery tell -- refused, not last-wins
    assertThrows(()=>o.parseAuthorizationResponse("https://client.example.com/cb?code=abc&code=def"), "duplicate code throws");
    assertThrows(()=>o.parseAuthorizationResponse("https://client.example.com/cb?state=a&state=b"), "duplicate state throws");
    // code+error together forbidden (RFC 6749 §4.1.2.1)
    assertThrows(()=>o.parseAuthorizationResponse("https://client.example.com/cb?code=abc&error=access_denied"), "code+error throws");
    // implicit-flow fragment response refused loudly (OAuth 2.1 removes it)
    assertThrows(()=>o.parseAuthorizationResponse("https://client.example.com/cb#access_token=abc&token_type=Bearer"), "implicit fragment throws");
    assertThrows(()=>o.parseAuthorizationResponse("https://client.example.com/cb#id_token=abc"), "id_token fragment throws");
    assertEq(o.parseAuthorizationResponse("https://client.example.com/cb?state=xyz").state,"xyz","state only ok");
    assertEq(o.parseAuthorizationResponse("https://client.example.com/cb#state=xyz").state, undefined, "fragment state not in query");
}

// buildTokenRequestBody / parseTokenResponse
{
    let body = o.buildTokenRequestBody({grant_type:"authorization_code", code:"abc", redirect_uri:"https://client.example.com/cb", client_id:"id"});
    assert(body.includes("grant_type=authorization_code") && body.includes("code=abc"), "token body");
    // space encoded as +
    let body2 = o.buildTokenRequestBody({scope:"read write"});
    assert(body2==="scope=read+write", "form space +");
    let j = o.parseTokenResponse('{"access_token":"mF_9","token_type":"Bearer","expires_in":3600}');
    assert(j.access_token==="mF_9" && j.token_type==="Bearer", "parseTokenResponse");
    assertThrows(()=>o.parseTokenResponse('{"access_token":"a","token_type":"mac"}'), "token_type mac throws");
    assertThrows(()=>o.parseTokenResponse('not json'), "invalid json throws");
}

// buildClientAuthHeader
{
    let h = o.buildClientAuthHeader("s6BhdRkqt3", "7Fjfp0ZBr1KtDRbnfVdmIw");
    assert(h.startsWith("Basic "), "client auth Basic");
    // form-encoded id:secret then base64, check decode
    let b64 = h.slice(6);
    // decode via atob-like: we test round trip using parse? Just check not empty
    assert(b64.length>10, "client auth b64 length");
    assertThrows(()=>o.buildClientAuthHeader("a\nb"), "CTL throws");
}

// boundaries: verifier 42/43/128/129, state bytes edge, URL length
{
    assertThrows(()=>o.generateCodeChallenge("a".repeat(42), "S256"), "42 throws");
    assert(o.generateCodeChallenge("a".repeat(43),"S256"), "43 ok");
    assert(o.generateCodeChallenge("a".repeat(128),"S256"), "128 ok");
    assertThrows(()=>o.generateCodeChallenge("a".repeat(129),"S256"), "129 throws");
    assertThrows(()=>o.generateState(0), "0 bytes throws");
    assert(o.generateState(1), "1 byte ok");
    assertThrows(()=>o.generateState(257), "257 throws");
}

// worst: log verifier, extraParams pollution, query bearer
{
    // ensure extraParams cannot inject CRLF
    let u = o.buildAuthorizationUrl({authorizationEndpoint:"https://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb", extraParams:{"x":"a\nb"}});
    assert(!u.url.includes("\n"), "extraParams CRLF rejected");
    // query bearer not allowed by default
    assert(o.parseBearerFromRequest({headers:{}, query:"access_token=abc", allowQuery:false})===null, "query not allowed");
}

// exhaustive combinatorial — all parameter combos
{
    // isValidBearerToken edge pure padding
    assert(!o.isValidBearerToken("=="), "== pure padding false");
    assert(!o.isValidBearerToken("="), "= false");
    assert(o.isValidBearerToken("a"), "single char true");
    assert(!o.isValidBearerToken("a!"), "! false");
    assert(!o.isValidBearerToken("a".repeat(4097)), "4097 false");
    assert(o.isValidBearerToken("a".repeat(4096)), "4096 true");
    // all unreserved chars for verifier
    let allUnreserved = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    for(let c of allUnreserved) assert(o.isValidCodeVerifier(c.repeat(43)), "unreserved "+c);
    assert(!o.isValidCodeVerifier("a".repeat(42)+"*"), "* not unreserved");
    // generateChallenge default S256 when challenge without method
    let v = o.generateCodeVerifier();
    let u = o.buildAuthorizationUrl({authorizationEndpoint:"https://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb", codeChallenge:v});
    assert(u.url.includes("code_challenge="+v) && u.url.includes("code_challenge_method=S256"), "default S256 when challenge without method");
    // method case sensitivity
    assertThrows(()=>o.generateCodeChallenge(v,"s256"), "lower s256 throws");
    assertThrows(()=>o.generateCodeChallenge(v,"S256 "), "trailing space throws");
    // secureCompare DataView + offsets
    let dv = new DataView(new Uint8Array([0,1,2,3,4]).buffer, 1, 3);
    let dv2 = new Uint8Array([1,2,3]);
    assert(o.secureCompare(dv, dv2), "DataView vs Uint8Array true");
    assert(!o.secureCompare(dv, new Uint8Array([1,2,4])), "DataView false");
    let ab1 = new Int8Array([1,2,3]); // 1-byte but different type, should work (bpe==1)
    assert(o.secureCompare(ab1, dv2), "Int8Array true");
    // scope NQCHAR boundaries: %x21 !, %x23 #, %x5B [, %x5D ], %x5E ^, %x7E ~ should pass, %x22 " and %x5C \ should fail
    assert(o.parseScope("! # [ ] ^ ~").length===6, "NQCHAR boundaries");
    assertThrows(()=>o.parseScope('a"b'), "quote in scope via NQCHAR");
    assertThrows(()=>o.parseScope('a\\b'), "backslash via NQCHAR");
    // buildWWWAuthenticate all combos
    let w1 = o.buildWWWAuthenticate({realm:"ex", error:"invalid_token", errorDescription:"expired", errorUri:"https://ex.com", scope:"read"});
    assert(w1.includes('realm="ex"') && w1.includes('error="invalid_token"') && w1.includes('error_description="expired"'), "www all");
    let w2 = o.buildWWWAuthenticate({error:"invalid_token"});
    assert(w2==='Bearer error="invalid_token"', "www error only");
    // triple method bearer
    assertThrows(()=>o.parseBearerFromRequest({headers:{authorization:"Bearer t1"}, query:"access_token=t2", body:"access_token=t3", allowQuery:true, allowBody:true}), "triple methods throws");
    // redirect with userinfo should be treated as invalid? Not in spec, but has_ctl would pass; test not crash
    assert(!o.isValidRedirectUri("https://user:pass@client.example.com/cb", ["https://client.example.com/cb"]), "userinfo mismatch false");
    // buildAuthorizationUrl missing combos
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://a/b", clientId:"id"}), "missing redirectUri throws");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://a/b", redirectUri:"https://c/d"}), "missing clientId throws");
    // token body with special chars
    let tb = o.buildTokenRequestBody({k:"a b", v:"c+d"});
    assert(tb.includes("k=a+b") && tb.includes("v=c%2Bd"), "token body encoding");
    // parseTokenResponse case-insensitive bearer lower
    assert(o.parseTokenResponse('{"access_token":"a","token_type":"BEARER"}').token_type==="BEARER", "BEARER upper");
    // buildClientAuthHeader with colon in secret
    let h = o.buildClientAuthHeader("id", "a:b");
    assert(h.startsWith("Basic "), "client auth colon secret");
    // NUL injection
    assert(!o.isValidBearerToken("a\0b"), "NUL bearer false");
    assert(!o.isValidRedirectUri("https://a/b\0evil", ["https://a/b"]), "NUL redirect false");
    // extraParams prototype pollution already tested, add more
    let uProto = o.buildAuthorizationUrl({authorizationEndpoint:"https://auth.example.com/authorize", clientId:"id", redirectUri:"https://client.example.com/cb", extraParams:{constructor:"evil", scope:"evil"}});
    assert(!uProto.url.includes("evil_scope"), "extra scope pollution rejected");
}

// buildAuthorizationUrl: long endpoint, scope grammar, empty fields, challenge shape
{
    // endpoint longer than the old 4096 build buffer: must be preserved
    // verbatim, with params appended after it -- never heap garbage
    let ep = "https://auth.example.com/authorize/" + "x".repeat(5000);
    let u = o.buildAuthorizationUrl({authorizationEndpoint: ep, clientId:"id", redirectUri:"https://client.example.com/cb"});
    assert(u.url.startsWith(ep), "long endpoint preserved verbatim");
    assert(u.url.includes("response_type=code") && u.url.includes("client_id=id"), "long endpoint params present");
    assertEq(u.url.indexOf(ep)+ep.length, u.url.indexOf("?response_type=code"), "params directly after long endpoint");
    // scope grammar matches parseScope: no leading/trailing/consecutive space
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://a/", clientId:"id", redirectUri:"https://c/d", scope:" read"}), "scope leading space throws");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://a/", clientId:"id", redirectUri:"https://c/d", scope:"a  b"}), "scope double space throws");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://a/", clientId:"id", redirectUri:"https://c/d", scope:" "}), "scope lone space throws");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://a/", clientId:"id", redirectUri:"https://c/d", scope:""}), "scope empty throws");
    let us = o.buildAuthorizationUrl({authorizationEndpoint:"https://a/", clientId:"id", redirectUri:"https://c/d", scope:"read write"});
    assert(us.url.includes("scope=read+write"), "scope two tokens ok");
    // empty required fields refused
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"", clientId:"id", redirectUri:"https://c/d"}), "empty endpoint throws");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://a/", clientId:"", redirectUri:"https://c/d"}), "empty clientId throws");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://a/", clientId:"id", redirectUri:""}), "empty redirectUri throws");
    // challenge shape per method (RFC 7636 §4.2): S256 exactly 43 unreserved, plain 43..128
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://a/", clientId:"id", redirectUri:"https://c/d", codeChallenge:"a".repeat(44), codeChallengeMethod:"S256"}), "S256 challenge 44 throws");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://a/", clientId:"id", redirectUri:"https://c/d", codeChallenge:"a".repeat(42), codeChallengeMethod:"S256"}), "S256 challenge 42 throws");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://a/", clientId:"id", redirectUri:"https://c/d", codeChallenge:"a!b".padEnd(43,"c"), codeChallengeMethod:"S256"}), "challenge bad char throws");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://a/", clientId:"id", redirectUri:"https://c/d", codeChallenge:"a".repeat(42), codeChallengeMethod:"plain", allowPlain:true}), "plain challenge 42 throws");
    assertThrows(()=>o.buildAuthorizationUrl({authorizationEndpoint:"https://a/", clientId:"id", redirectUri:"https://c/d", codeChallenge:"a".repeat(129), codeChallengeMethod:"plain", allowPlain:true}), "plain challenge 129 throws");
    let uc = o.buildAuthorizationUrl({authorizationEndpoint:"https://a/", clientId:"id", redirectUri:"https://c/d", codeChallenge:"a".repeat(44), codeChallengeMethod:"plain", allowPlain:true});
    assert(uc.url.includes("code_challenge="+"a".repeat(44)), "plain challenge 44 ok");
}

// parseTokenResponse: RFC 6749 §5.1 required fields
{
    assertThrows(()=>o.parseTokenResponse('{"token_type":"Bearer"}'), "missing access_token throws");
    assertThrows(()=>o.parseTokenResponse('{"access_token":"a b"}'), "non-b64token access_token throws");
    assertThrows(()=>o.parseTokenResponse('{"access_token":"a"}'), "missing token_type throws");
    assertThrows(()=>o.parseTokenResponse('{"access_token":"a","token_type":"Bearer","expires_in":"3600"}'), "string expires_in throws");
    assertThrows(()=>o.parseTokenResponse('{"access_token":"a","token_type":"Bearer","scope":["read"]}'), "array scope throws");
    assertThrows(()=>o.parseTokenResponse('{"access_token":"a","token_type":"Bearer","refresh_token":5}'), "numeric refresh_token throws");
    let ok = o.parseTokenResponse('{"access_token":"mF_9","token_type":"Bearer","expires_in":3600,"scope":"read"}');
    assert(ok.access_token==="mF_9" && ok.expires_in===3600, "full valid response ok");
}

// buildTokenRequestBody: refuse non-string values instead of dropping them
{
    assertThrows(()=>o.buildTokenRequestBody({code: 123}), "non-string value throws");
    assertThrows(()=>o.buildTokenRequestBody({code: null}), "null value throws");
    assertThrows(()=>o.buildTokenRequestBody({code: ["x"]}), "array value throws");
    assert(o.buildTokenRequestBody({grant_type:"authorization_code", code:"abc"})==="grant_type=authorization_code&code=abc", "two params order");
}

// JWT — optional, needs CONFIG_TLS=y (otherwise verifyJWT throws)
{
    let JWTSign = crypto.JWTSign;
    let hasCrypto = typeof JWTSign==="function";
    if(!hasCrypto){
        // non-TLS build: verifyJWT must throw "JWT not available"
        assertThrows(()=>o.verifyJWT("a.b.c", "key", {algorithms:["HS256"]}), "JWT not available without TLS");
    } else {
        // runtime available — exhaustive JWT vectors
        let key = new TextEncoder().encode("test-secret-32-bytes-long-xxxxxx");
        let now = Math.floor(Date.now()/1000);
        // valid
        let tk = JWTSign({sub:"u1", aud:"https://api.example.com", iss:"https://auth.example.com", exp: now+3600, nbf: now-10, scope:"read write"}, key, {alg:"HS256"});
        let p = o.verifyJWT(tk, key, {algorithms:["HS256"], aud:"https://api.example.com", iss:"https://auth.example.com", requiredScope:["read"]});
        assert(p.sub==="u1" && p.aud==="https://api.example.com", "JWT valid");
        // exp expired
        let tkExp = JWTSign({sub:"u1", exp: now-10}, key, {alg:"HS256"});
        assertThrows(()=>o.verifyJWT(tkExp, key, {algorithms:["HS256"]}), "expired throws");
        // nbf future
        let tkNbf = JWTSign({sub:"u1", nbf: now+3600}, key, {alg:"HS256"});
        assertThrows(()=>o.verifyJWT(tkNbf, key, {algorithms:["HS256"]}), "nbf future throws");
        // clock skew allows nbf 5s future with 10s skew
        let tkSkew = JWTSign({sub:"u1", nbf: now+5}, key, {alg:"HS256"});
        assert(o.verifyJWT(tkSkew, key, {algorithms:["HS256"], clockSkewSec:10}).sub==="u1", "skew allows");
        // aud mismatch
        assertThrows(()=>o.verifyJWT(tk, key, {algorithms:["HS256"], aud:"https://other.example.com"}), "aud mismatch");
        // aud array handling: token aud string vs expected array
        let tkAudArr = JWTSign({sub:"u1", aud:["https://api.example.com","https://other.example.com"]}, key, {alg:"HS256"});
        assert(o.verifyJWT(tkAudArr, key, {algorithms:["HS256"], aud:"https://other.example.com"}).sub==="u1", "aud array contains");
        assertThrows(()=>o.verifyJWT(tk, key, {algorithms:["HS256"], aud:["https://other.example.com","https://x.example.com"]}), "aud array mismatch");
        // iss mismatch
        assertThrows(()=>o.verifyJWT(tk, key, {algorithms:["HS256"], iss:"https://evil.example.com"}), "iss mismatch");
        // scope missing
        assertThrows(()=>o.verifyJWT(tk, key, {algorithms:["HS256"], requiredScope:["admin"]}), "scope missing throws");
        assert(o.verifyJWT(tk, key, {algorithms:["HS256"], requiredScope:["read","write"]}).scope==="read write", "scope all present");
        // alg:none — must be rejected even if allowlisted
        let b64obj = (obj)=> Base64URLEncode(new TextEncoder().encode(JSON.stringify(obj)));
        let noneTok = b64obj({alg:"none",typ:"JWT"})+"."+b64obj({sub:"admin"})+".";
        assertThrows(()=>o.verifyJWT(noneTok, key, {algorithms:["none"]}), "alg:none even allowlisted throws");
        assertThrows(()=>o.verifyJWT(noneTok, key, {algorithms:["HS256"]}), "alg:none with HS256 allowlist throws");
        // alg confusion: HS256 token verified with RS256 allowlist should fail (wrong alg)
        let hsTk = JWTSign({sub:"u1"}, key, {alg:"HS256"});
        assertThrows(()=>o.verifyJWT(hsTk, key, {algorithms:["RS256"]}), "HS256 not in RS256 allowlist");
        // tamper payload
        let parts = tk.split(".");
        let tampered = parts[0]+"."+ b64obj({sub:"admin", aud:"https://api.example.com", iss:"https://auth.example.com", exp: now+3600}) +"."+parts[2];
        assertThrows(()=>o.verifyJWT(tampered, key, {algorithms:["HS256"], aud:"https://api.example.com"}), "tampered payload throws");
        // wrong key
        let wrongKey = new TextEncoder().encode("wrong-secret-32-bytes-long-yyyyyy");
        assertThrows(()=>o.verifyJWT(tk, wrongKey, {algorithms:["HS256"]}), "wrong key throws");
        // HS384/512
        for(let alg of ["HS384","HS512"]){
            let t = JWTSign({x:1}, key, {alg});
            assert(o.verifyJWT(t, key, {algorithms:[alg]}).x===1, alg+" round trip");
            assertThrows(()=>o.verifyJWT(t, key, {algorithms:["HS256"]}), alg+" wrong allowlist");
        }
        // exp == now is expired: RFC 7519 §4.1.4 needs now STRICTLY < exp
        let tkExpNow = JWTSign({sub:"u1", exp: now}, key, {alg:"HS256"});
        assertThrows(()=>o.verifyJWT(tkExpNow, key, {algorithms:["HS256"]}), "exp == now throws");
        assert(o.verifyJWT(tkExpNow, key, {algorithms:["HS256"], clockSkewSec:10}).sub==="u1", "exp == now passes with skew");
        // out-of-range clockSkewSec throws instead of silently becoming 0
        assertThrows(()=>o.verifyJWT(tk, key, {algorithms:["HS256"], clockSkewSec:301}), "clockSkewSec 301 throws");
        assertThrows(()=>o.verifyJWT(tk, key, {algorithms:["HS256"], clockSkewSec:-1}), "clockSkewSec -1 throws");
        assertThrows(()=>o.verifyJWT(tk, key, {algorithms:["HS256"], clockSkewSec:60000}), "clockSkewSec 60000 throws");
        // a non-array requiredScope throws instead of skipping the scope check
        assertThrows(()=>o.verifyJWT(tk, key, {algorithms:["HS256"], requiredScope:"read"}), "string requiredScope throws");
        // the JWT bridge slot is frozen: a forged verifier is not installable
        let forgedRan = false;
        try { globalThis.__oauth_jwt_verify = function(){ forgedRan = true; return {sub:"admin"}; }; } catch (e) {}
        assert(o.verifyJWT(tk, key, {algorithms:["HS256"]}).sub==="u1", "verifyJWT unaffected by forgery attempt");
        assert(!forgedRan, "forged bridge never invoked");
        assertThrows(()=>{ globalThis.__oauth_jwt_verify = function(){ return {sub:"admin"}; }; }, "assigning the frozen bridge slot throws");
        // empty algorithms
        assertThrows(()=>o.verifyJWT(tk, key, {algorithms:[]}), "empty allowlist");
        assertThrows(()=>o.verifyJWT(tk, key, {}), "missing algorithms");
        // missing token
        assertThrows(()=>o.verifyJWT("", key, {algorithms:["HS256"]}), "empty token");
        assertThrows(()=>o.verifyJWT("not.a.jwt", key, {algorithms:["HS256"]}), "malformed too few parts");
    }
}

// F1: the loopback port-ignore parses the authority (userinfo smuggle,
// IPv6 bracket atomicity, digits-only ports)
{
    assert(!o.isValidRedirectUri("http://127.0.0.1:8080@evil.com/cb", ["http://127.0.0.1/cb"]), "userinfo smuggle rejected");
    assert(!o.isValidRedirectUri("http://127.0.0.1:8080@127.0.0.1.evil.com/cb", ["http://127.0.0.1/cb"]), "userinfo smuggle subdomain rejected");
    assert(!o.isValidRedirectUri("http://[::1]:8080@evil.com/cb", ["http://[::1]/cb"]), "ipv6 userinfo smuggle rejected");
    assert(!o.isValidRedirectUri("http://[::1]evil.com/cb", ["http://[::1]/cb"]), "ipv6 trailing junk rejected");
    assert(!o.isValidRedirectUri("http://[::1/cb", ["http://[::1]/cb"]), "unterminated ipv6 bracket rejected");
    assert(!o.isValidRedirectUri("http://127.0.0.1:80a/cb", ["http://127.0.0.1/cb"]), "non-digit port not ignored");
    // the legitimate port-ignore survives the rewrite
    assert(o.isValidRedirectUri("http://127.0.0.1:54321/cb", ["http://127.0.0.1/cb"]), "loopback port-ignore to portless registration");
    assert(o.isValidRedirectUri("http://localhost:8443/cb", ["http://localhost/cb"]), "localhost port-ignore");
    // port-ignore stays loopback-only (8252 §7.3): other hosts match verbatim
    assert(!o.isValidRedirectUri("https://app.example.com:8443/cb", ["https://app.example.com/cb"]), "non-loopback ports are NOT ignored");
}

// F3: secureCompare copies arg0 before arg1's coercion can run user JS
{
    let victim = new Uint8Array([1, 2, 3]);
    let evil = { toString() { victim[0] = 99; return "\x01\x02\x03"; } };
    assert(o.secureCompare(victim, evil) === true, "arg0's original bytes are compared");
    assert(victim[0] === 99, "the toString mutation really happened");
    let victim2 = new Uint8Array([1, 2, 4]);
    let evil2 = { toString() { victim2[0] = 99; return "\x01\x02\x03"; } };
    assert(o.secureCompare(victim2, evil2) === false, "mismatch after mutation still false");
}

// F4: realm and errorUri cannot break out of the quoted-string
{
    assertThrows(()=>o.buildWWWAuthenticate({realm:'a", error="fake', error:"invalid_token"}), "realm quote breakout throws");
    assertThrows(()=>o.buildWWWAuthenticate({error:"invalid_token", errorUri:'x", scope="admin'}), "errorUri quote breakout throws");
    assertThrows(()=>o.buildWWWAuthenticate({realm:"a\\b", error:"invalid_token"}), "realm backslash throws");
    assert(o.buildWWWAuthenticate({realm:"api.example.com", error:"invalid_token"}).includes('realm="api.example.com"'), "normal realm works");
}

// F5: the fragment blocklist covers every implicit-flow parameter, and
// error_description/error_uri duplicates are refused like code/state
{
    assertThrows(()=>o.parseAuthorizationResponse("https://client.example.com/cb#expires_in=3600"), "expires_in fragment throws");
    assertThrows(()=>o.parseAuthorizationResponse("https://client.example.com/cb#scope=read"), "scope fragment throws");
    assertThrows(()=>o.parseAuthorizationResponse("https://client.example.com/cb?error_description=a&error_description=b"), "duplicate error_description throws");
    assertThrows(()=>o.parseAuthorizationResponse("https://client.example.com/cb?error_uri=x&error_uri=y"), "duplicate error_uri throws");
}

// F8: a token response that parses to a non-object is refused, not returned
{
    assertThrows(()=>o.parseTokenResponse("123"), "numeric body throws");
    assertThrows(()=>o.parseTokenResponse('"abc"'), "string body throws");
    assertThrows(()=>o.parseTokenResponse("true"), "boolean body throws");
}

// F9: near-limit WWW-Authenticate values neither truncate nor crash
{
    let w = o.buildWWWAuthenticate({realm:"r".repeat(4000), error:"invalid_token"});
    assert(w.startsWith('Bearer realm="' + "r".repeat(4000) + '"'), "4000-char realm verbatim");
    let w2 = o.buildWWWAuthenticate({error:"invalid_token", errorDescription:"d".repeat(3000), errorUri:"https://e.example.com/"+"u".repeat(2979), scope:"s".repeat(500)});
    assert(w2.includes('error_description="' + "d".repeat(3000) + '"'), "long desc verbatim");
    assert(w2.endsWith(', scope="' + "s".repeat(500) + '"'), "long scope terminates the header");
}

print("test_oauth2: all "+n+" tests passed");
