// covers: dyna:oauth2.parseBearerFromRequest
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_oauth2_parse_bearer_from_request");
import * as o from "dyna:oauth2";
import * as crypto from "dyna:crypto";
import { Base64URLEncode } from "dyna:encoding";
let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function assertThrows(fn, msg) { n++; let threw=false; try{fn();}catch(e){threw=true;} if(!threw) throw new Error("expected throw: "+msg); }
function assertEq(a,b,msg){ n++; if(a!==b) throw new Error(msg+": expected "+JSON.stringify(b)+" got "+JSON.stringify(a)); }

__kitS.section("Bearer mF_9.B5f-4.1JqM", () => {
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
    assert(!o.isValidBearerToken("a".repeat(4097), {strict:true}), "too long false under strict");
    assert(o.parseBearerHeader("Bearer "+"a".repeat(5000)+"===")==="a".repeat(5000)+"===", "RFC 6750: parse a 5000-char token with three =");
    assert(o.buildBearerHeader("a".repeat(5000)+"===")==="Bearer "+"a".repeat(5000)+"===", "RFC 6750: build a 5000-char token with three =");
    assert(o.isValidBearerToken("abc+def/ghi=="), "b64token +/=");
    assert(o.buildBearerHeader("abc+def/ghi==")==="Bearer abc+def/ghi==", "build with +/=");
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
    assertThrows(()=>o.parseBearerFromRequest({headers:{authorization:"Bearer t1"}, query:"access_token=t2", allowQuery:true}), "multiple methods header+query throws");
    assertThrows(()=>o.parseBearerFromRequest({headers:{authorization:"Bearer t1"}, body:"access_token=t2", allowBody:true}), "header+body throws");
});
__kitS.section("a header named authorization plus a NUL suffix is not Authorization (fuzz_oauth2 2026-11-27)", () => {
    assertEq(o.parseBearerFromRequest({ headers: { ["authorization\0x"]: "Bearer abc" } }), null, "NUL-suffixed name");
    assertEq(o.parseBearerFromRequest({ headers: { ["Authorization"]: "Bearer abc" } }), "abc", "plain name");
});
__kitS.end();

print("test_oauth2: all "+n+" tests passed");
