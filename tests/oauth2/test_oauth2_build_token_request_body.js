// covers: dyna:oauth2.buildTokenRequestBody
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_oauth2_build_token_request_body");
import * as o from "dyna:oauth2";
import * as crypto from "dyna:crypto";
import { Base64URLEncode } from "dyna:encoding";
let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function assertThrows(fn, msg) { n++; let threw=false; try{fn();}catch(e){threw=true;} if(!threw) throw new Error("expected throw: "+msg); }
function assertEq(a,b,msg){ n++; if(a!==b) throw new Error(msg+": expected "+JSON.stringify(b)+" got "+JSON.stringify(a)); }

__kitS.section("non-string value throws", () => {
    assertThrows(()=>o.buildTokenRequestBody({code: 123}), "non-string value throws");
    assertThrows(()=>o.buildTokenRequestBody({code: null}), "null value throws");
    assertThrows(()=>o.buildTokenRequestBody({code: ["x"]}), "array value throws");
    assert(o.buildTokenRequestBody({grant_type:"authorization_code", code:"abc"})==="grant_type=authorization_code&code=abc", "two params order");
});
__kitS.section("a key holding NUL is encoded whole, not cut at the NUL (fuzz_oauth2 2026-11-27)", () => {
    assertEq(o.buildTokenRequestBody({ ["x\0y"]: "v" }), "x%00y=v", "NUL key");
    assertEq(o.buildTokenRequestBody({ ["code\0"]: "v", code: "c" }), "code%00=v&code=c", "NUL suffix does not alias another key");
});
__kitS.end();

print("test_oauth2: all "+n+" tests passed");
