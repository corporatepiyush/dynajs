// covers: dyna:oauth2.parseAuthorizationResponse
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_oauth2_parse_authorization_response");
import * as o from "dyna:oauth2";
import * as crypto from "dyna:crypto";
import { Base64URLEncode } from "dyna:encoding";
let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function assertThrows(fn, msg) { n++; let threw=false; try{fn();}catch(e){threw=true;} if(!threw) throw new Error("expected throw: "+msg); }
function assertEq(a,b,msg){ n++; if(a!==b) throw new Error(msg+": expected "+JSON.stringify(b)+" got "+JSON.stringify(a)); }

__kitS.section("parse code", () => {
    let p = o.parseAuthorizationResponse("https://client.example.com/cb?code=SplxlOBeZQQYbYS6WxSbIA&state=xyz");
    assertEq(p.code,"SplxlOBeZQQYbYS6WxSbIA","parse code");
    assertEq(p.state,"xyz","parse state");
    let e = o.parseAuthorizationResponse("https://client.example.com/cb?error=access_denied&error_description=denied&state=xyz");
    assertEq(e.error,"access_denied","parse error");
    assertEq(e.errorDescription,"denied","parse errorDescription");
    assertThrows(()=>o.parseAuthorizationResponse("https://client.example.com/cb?code=abc&code=def"), "duplicate code throws");
    assertThrows(()=>o.parseAuthorizationResponse("https://client.example.com/cb?state=a&state=b"), "duplicate state throws");
    assertThrows(()=>o.parseAuthorizationResponse("https://client.example.com/cb?code=abc&error=access_denied"), "code+error throws");
    assertThrows(()=>o.parseAuthorizationResponse("https://client.example.com/cb#access_token=abc&token_type=Bearer"), "implicit fragment throws");
    assertThrows(()=>o.parseAuthorizationResponse("https://client.example.com/cb#id_token=abc"), "id_token fragment throws");
    assertEq(o.parseAuthorizationResponse("https://client.example.com/cb?state=xyz").state,"xyz","state only ok");
    assertEq(o.parseAuthorizationResponse("https://client.example.com/cb#state=xyz").state, undefined, "fragment state not in query");
});

__kitS.section("expires_in fragment throws", () => {
    assertThrows(()=>o.parseAuthorizationResponse("https://client.example.com/cb#expires_in=3600"), "expires_in fragment throws");
    assertThrows(()=>o.parseAuthorizationResponse("https://client.example.com/cb#scope=read"), "scope fragment throws");
    assertThrows(()=>o.parseAuthorizationResponse("https://client.example.com/cb?error_description=a&error_description=b"), "duplicate error_description throws");
    assertThrows(()=>o.parseAuthorizationResponse("https://client.example.com/cb?error_uri=x&error_uri=y"), "duplicate error_uri throws");
});
__kitS.end();

print("test_oauth2: all "+n+" tests passed");
