// covers: dyna:oauth2.secureCompare
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_oauth2_secure_compare");
import * as o from "dyna:oauth2";
import * as crypto from "dyna:crypto";
import { Base64URLEncode } from "dyna:encoding";
let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function assertThrows(fn, msg) { n++; let threw=false; try{fn();}catch(e){threw=true;} if(!threw) throw new Error("expected throw: "+msg); }
function assertEq(a,b,msg){ n++; if(a!==b) throw new Error(msg+": expected "+JSON.stringify(b)+" got "+JSON.stringify(a)); }

__kitS.section("generateState default", () => {
    let s = o.generateState();
    assert(typeof s==="string" && s.length>=43, "generateState default");
    let s16 = o.generateState(16);
    assert(typeof s16==="string", "generateState 16");
    assertThrows(()=>o.generateState(0), "generateState 0 throws");
    assertThrows(()=>o.generateState(257), "257 throws");
    assert(o.secureCompare("abc","abc"), "secureCompare true");
    assert(!o.secureCompare("abc","abd"), "secureCompare false");
    assert(!o.secureCompare("abc","ab"), "secureCompare length false");
    let u1 = new Uint8Array([97,98,99]);
    let u2 = new Uint8Array([97,98,99]);
    let u3 = new Uint8Array([97,98,100]);
    assert(o.secureCompare(u1,u2), "secureCompare Uint8Array true");
    assert(!o.secureCompare(u1,u3), "secureCompare Uint8Array false");
    assert(o.secureCompare("abc", u1), "secureCompare string vs Uint8Array true");
    assert(!o.secureCompare("abc", u3), "secureCompare string vs Uint8Array false");
    let ab = new Uint8Array([1,2,3]).buffer;
    assert(o.secureCompare(new Uint8Array([1,2,3]), ab), " ArrayBuffer compare");
});

__kitS.section("arg0's original bytes are compared", () => {
    let victim = new Uint8Array([1, 2, 3]);
    let evil = { toString() { victim[0] = 99; return "\x01\x02\x03"; } };
    assert(o.secureCompare(victim, evil) === true, "arg0's original bytes are compared");
    assert(victim[0] === 99, "the toString mutation really happened");
    let victim2 = new Uint8Array([1, 2, 4]);
    let evil2 = { toString() { victim2[0] = 99; return "\x01\x02\x03"; } };
    assert(o.secureCompare(victim2, evil2) === false, "mismatch after mutation still false");
});
__kitS.end();

print("test_oauth2: all "+n+" tests passed");
