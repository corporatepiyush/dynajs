// covers: dyna:oauth2.buildWWWAuthenticate
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_oauth2_build_www_authenticate");
import * as o from "dyna:oauth2";
import * as crypto from "dyna:crypto";
import { Base64URLEncode } from "dyna:encoding";
let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function assertThrows(fn, msg) { n++; let threw=false; try{fn();}catch(e){threw=true;} if(!threw) throw new Error("expected throw: "+msg); }
function assertEq(a,b,msg){ n++; if(a!==b) throw new Error(msg+": expected "+JSON.stringify(b)+" got "+JSON.stringify(a)); }

__kitS.section("Bearer realm=\"example\", error=\"invalid_token\"", () => {
    let w = o.buildWWWAuthenticate({realm:"example", error:"invalid_token"});
    assertEq(w, 'Bearer realm="example", error="invalid_token"', "www realm+error");
    let w2 = o.buildWWWAuthenticate({realm:"example", error:"invalid_token", errorDescription:"expired", scope:"read"});
    assert(w2.includes('error_description="expired"') && w2.includes('scope="read"'), "www desc+scope");
    assertThrows(()=>o.buildWWWAuthenticate({realm:"a\nb", error:"invalid_token"}), "realm CRLF throws");
    assertEq(o.buildWWWAuthenticate({error:"invalid token"}), 'Bearer error="invalid token"', "error space allowed: RFC 6749 A.7 NQSCHAR");
    assertThrows(()=>o.buildWWWAuthenticate({error:"invalid token", strict:true}), "error space throws under strict");
    assertEq(o.buildWWWAuthenticate({error:"! #[]^~"}), 'Bearer error="! #[]^~"', "NQSCHAR boundaries");
    assertEq(o.buildWWWAuthenticate({error:"e".repeat(65)}), 'Bearer error="'+"e".repeat(65)+'"', "65-char error allowed by default");
    assertThrows(()=>o.buildWWWAuthenticate({error:"e".repeat(65), strict:true}), "65-char error throws under strict");
    assertEq(o.buildWWWAuthenticate({error:"e".repeat(64), strict:true}), 'Bearer error="'+"e".repeat(64)+'"', "64-char error ok under strict");
    assertThrows(()=>o.buildWWWAuthenticate({error:"a!b", strict:true}), "non-token68 error throws under strict");
    assertThrows(()=>o.buildWWWAuthenticate({error:'a"b'}), "error quote throws");
    assertThrows(()=>o.buildWWWAuthenticate({error:"a\\b"}), "error backslash throws");
    assertThrows(()=>o.buildWWWAuthenticate({error:"a\x7f"}), "error DEL throws");
    assertThrows(()=>o.buildWWWAuthenticate({error:"a\x1f"}), "error CTL throws");
    assertThrows(()=>o.buildWWWAuthenticate({error:""}), "empty error throws");
    assertThrows(()=>o.buildWWWAuthenticate({error:"invalid_token", errorUri:"https://e.example/a b"}), "error_uri space throws: RFC 6749 A.8");
    assertEq(o.buildWWWAuthenticate({error:"x", errorUri:"https://e.example/!#[]^~"}), 'Bearer error="x", error_uri="https://e.example/!#[]^~"', "error_uri A.8 boundaries");
    assertEq(o.buildWWWAuthenticate({error:"x", errorDescription:" a b "}), 'Bearer error="x", error_description=" a b "', "error_description keeps spaces");
    assertThrows(()=>o.buildWWWAuthenticate({error:"x", strict:"yes"}), "strict non-boolean throws");
    assertThrows(()=>o.buildWWWAuthenticate({error:"invalid_token", errorDescription:'a"b'}), "quote throws");
    assertThrows(()=>o.buildWWWAuthenticate({realm:"example"}), "no error throws");
    assertThrows(()=>o.buildWWWAuthenticate({realm:"example", errorDescription:"x"}), "desc without error throws");
    assertThrows(()=>o.buildWWWAuthenticate({scope:"read"}), "scope without error throws");
});

__kitS.section("a\", error=\"fake", () => {
    assertThrows(()=>o.buildWWWAuthenticate({realm:'a", error="fake', error:"invalid_token"}), "realm quote breakout throws");
    assertThrows(()=>o.buildWWWAuthenticate({error:"invalid_token", errorUri:'x", scope="admin'}), "errorUri quote breakout throws");
    assertThrows(()=>o.buildWWWAuthenticate({realm:"a\\b", error:"invalid_token"}), "realm backslash throws");
    assert(o.buildWWWAuthenticate({realm:"api.example.com", error:"invalid_token"}).includes('realm="api.example.com"'), "normal realm works");
});

__kitS.section("Bearer realm", () => {
    let w = o.buildWWWAuthenticate({realm:"r".repeat(4000), error:"invalid_token"});
    assert(w.startsWith('Bearer realm="' + "r".repeat(4000) + '"'), "4000-char realm verbatim");
    let w2 = o.buildWWWAuthenticate({error:"invalid_token", errorDescription:"d".repeat(3000), errorUri:"https://e.example.com/"+"u".repeat(2979), scope:"s".repeat(500)});
    assert(w2.includes('error_description="' + "d".repeat(3000) + '"'), "long desc verbatim");
    assert(w2.endsWith(', scope="' + "s".repeat(500) + '"'), "long scope terminates the header");
});
__kitS.end();

print("test_oauth2: all "+n+" tests passed");
