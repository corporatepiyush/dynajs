// covers: dyna:oauth2.generateCodeChallenge
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_oauth2_generate_code_challenge");
import * as o from "dyna:oauth2";
import * as crypto from "dyna:crypto";
import { Base64URLEncode } from "dyna:encoding";
let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function assertThrows(fn, msg) { n++; let threw=false; try{fn();}catch(e){threw=true;} if(!threw) throw new Error("expected throw: "+msg); }
function assertEq(a,b,msg){ n++; if(a!==b) throw new Error(msg+": expected "+JSON.stringify(b)+" got "+JSON.stringify(a)); }

__kitS.section("42 throws", () => {
    assertThrows(()=>o.generateCodeChallenge("a".repeat(42), "S256"), "42 throws");
    assert(o.generateCodeChallenge("a".repeat(43),"S256"), "43 ok");
    assert(o.generateCodeChallenge("a".repeat(128),"S256"), "128 ok");
    assertThrows(()=>o.generateCodeChallenge("a".repeat(129),"S256"), "129 throws");
    assertThrows(()=>o.generateState(0), "0 bytes throws");
    assert(o.generateState(1), "1 byte ok");
    assertThrows(()=>o.generateState(257), "257 throws");
});
__kitS.end();

print("test_oauth2: all "+n+" tests passed");
