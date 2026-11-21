// covers: dyna:oauth2.isValidCodeVerifier
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_oauth2_is_valid_code_verifier");
import * as o from "dyna:oauth2";
import * as crypto from "dyna:crypto";
import { Base64URLEncode } from "dyna:encoding";
let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function assertThrows(fn, msg) { n++; let threw=false; try{fn();}catch(e){threw=true;} if(!threw) throw new Error("expected throw: "+msg); }
function assertEq(a,b,msg){ n++; if(a!==b) throw new Error(msg+": expected "+JSON.stringify(b)+" got "+JSON.stringify(a)); }

__kitS.section("RFC7636 B S256", () => {
    let verifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
    let challenge = o.generateCodeChallenge(verifier, "S256");
    assertEq(challenge, "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM", "RFC7636 B S256");
    assert(o.verifyCodeChallenge(verifier, challenge, "S256"), "verify S256 true");
    assert(!o.verifyCodeChallenge(verifier, challenge+"A", "S256"), "verify S256 false on tamper");
    assertEq(o.generateCodeChallenge(verifier, "plain"), verifier, "plain");
    assert(o.verifyCodeChallenge(verifier, verifier, "plain"), "verify plain true");
    assertEq(o.generateCodeChallenge(verifier, "plain"), verifier, "plain generate ok");
    assert(o.isValidCodeVerifier(verifier), "isValidCodeVerifier true");
    assert(!o.isValidCodeVerifier("short"), "isValid too short false");
    assert(!o.isValidCodeVerifier("a".repeat(42)), "42 false");
    assert(o.isValidCodeVerifier("a".repeat(43)), "43 true");
    assert(o.isValidCodeVerifier("a".repeat(128)), "128 true");
    assert(!o.isValidCodeVerifier("a".repeat(129)), "129 false");
    assert(!o.isValidCodeVerifier("a".repeat(43).replace("a","*")), "charset * false");
    let v2 = o.generateCodeVerifier();
    assert(o.isValidCodeVerifier(v2), "generated verifier valid");
    assertEq(v2.length, 43, "generated 43");
    assert(!o.isValidCodeVerifier("a".repeat(43).slice(0,42)+" "), "space fails");
    assert(!o.isValidCodeVerifier("a".repeat(43).slice(0,42)+"!"), "! fails");
});
__kitS.end();

print("test_oauth2: all "+n+" tests passed");
