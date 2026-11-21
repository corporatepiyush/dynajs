// covers: dyna:oauth2.buildClientAuthHeader
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_oauth2_build_client_auth_header");
import * as o from "dyna:oauth2";
import * as crypto from "dyna:crypto";
import { Base64URLEncode } from "dyna:encoding";
let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function assertThrows(fn, msg) { n++; let threw=false; try{fn();}catch(e){threw=true;} if(!threw) throw new Error("expected throw: "+msg); }
function assertEq(a,b,msg){ n++; if(a!==b) throw new Error(msg+": expected "+JSON.stringify(b)+" got "+JSON.stringify(a)); }

__kitS.section("Basic", () => {
    let h = o.buildClientAuthHeader("s6BhdRkqt3", "7Fjfp0ZBr1KtDRbnfVdmIw");
    assert(h.startsWith("Basic "), "client auth Basic");
    let b64 = h.slice(6);
    assert(b64.length>10, "client auth b64 length");
    assertThrows(()=>o.buildClientAuthHeader("a\nb"), "CTL throws");
});
__kitS.end();

print("test_oauth2: all "+n+" tests passed");
