// covers: dyna:oauth2.parseTokenResponse
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_oauth2_parse_token_response");
import * as o from "dyna:oauth2";
import * as crypto from "dyna:crypto";
import { Base64URLEncode } from "dyna:encoding";
let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function assertThrows(fn, msg) { n++; let threw=false; try{fn();}catch(e){threw=true;} if(!threw) throw new Error("expected throw: "+msg); }
function assertEq(a,b,msg){ n++; if(a!==b) throw new Error(msg+": expected "+JSON.stringify(b)+" got "+JSON.stringify(a)); }

__kitS.section("token body", () => {
    let body = o.buildTokenRequestBody({grant_type:"authorization_code", code:"abc", redirect_uri:"https://client.example.com/cb", client_id:"id"});
    assert(body.includes("grant_type=authorization_code") && body.includes("code=abc"), "token body");
    let body2 = o.buildTokenRequestBody({scope:"read write"});
    assert(body2==="scope=read+write", "form space +");
    let j = o.parseTokenResponse('{"access_token":"mF_9","token_type":"Bearer","expires_in":3600}');
    assert(j.access_token==="mF_9" && j.token_type==="Bearer", "parseTokenResponse");
    assertThrows(()=>o.parseTokenResponse('{"access_token":"a","token_type":"mac"}'), "token_type mac throws");
    assertThrows(()=>o.parseTokenResponse('not json'), "invalid json throws");
});

__kitS.section("missing access_token throws", () => {
    assertThrows(()=>o.parseTokenResponse('{"token_type":"Bearer"}'), "missing access_token throws");
    assertThrows(()=>o.parseTokenResponse('{"access_token":"a b"}'), "non-b64token access_token throws");
    assertThrows(()=>o.parseTokenResponse('{"access_token":"a"}'), "missing token_type throws");
    assertThrows(()=>o.parseTokenResponse('{"access_token":"a","token_type":"Bearer","expires_in":"3600"}'), "string expires_in throws");
    assertThrows(()=>o.parseTokenResponse('{"access_token":"a","token_type":"Bearer","scope":["read"]}'), "array scope throws");
    assertThrows(()=>o.parseTokenResponse('{"access_token":"a","token_type":"Bearer","refresh_token":5}'), "numeric refresh_token throws");
    let ok = o.parseTokenResponse('{"access_token":"mF_9","token_type":"Bearer","expires_in":3600,"scope":"read"}');
    assert(ok.access_token==="mF_9" && ok.expires_in===3600, "full valid response ok");
});

__kitS.section("numeric body throws", () => {
    assertThrows(()=>o.parseTokenResponse("123"), "numeric body throws");
    assertThrows(()=>o.parseTokenResponse('"abc"'), "string body throws");
    assertThrows(()=>o.parseTokenResponse("true"), "boolean body throws");
});
__kitS.end();

print("test_oauth2: all "+n+" tests passed");
