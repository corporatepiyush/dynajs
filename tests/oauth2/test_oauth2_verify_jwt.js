// covers: dyna:oauth2.verifyJWT
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_oauth2_verify_jwt");
import * as o from "dyna:oauth2";
import * as crypto from "dyna:crypto";
import { Base64URLEncode } from "dyna:encoding";
let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function assertThrows(fn, msg) { n++; let threw=false; try{fn();}catch(e){threw=true;} if(!threw) throw new Error("expected throw: "+msg); }
function assertEq(a,b,msg){ n++; if(a!==b) throw new Error(msg+": expected "+JSON.stringify(b)+" got "+JSON.stringify(a)); }

__kitS.section("JWT not available without OpenSSL-backed crypto", () => {
    let JWTSign = crypto.JWTSign;
    let hasCrypto = typeof JWTSign==="function";
    if(!hasCrypto){
        assertThrows(()=>o.verifyJWT("a.b.c", "key", {algorithms:["HS256"]}), "JWT not available without OpenSSL-backed crypto");
    } else {
        let key = new TextEncoder().encode("test-secret-32-bytes-long-xxxxxx");
        let now = Math.floor(Date.now()/1000);
        let tk = JWTSign({sub:"u1", aud:"https://api.example.com", iss:"https://auth.example.com", exp: now+3600, nbf: now-10, scope:"read write"}, key, {alg:"HS256"});
        let p = o.verifyJWT(tk, key, {algorithms:["HS256"], aud:"https://api.example.com", iss:"https://auth.example.com", requiredScope:["read"]});
        assert(p.sub==="u1" && p.aud==="https://api.example.com", "JWT valid");
        let tkExp = JWTSign({sub:"u1", exp: now-10}, key, {alg:"HS256"});
        assertThrows(()=>o.verifyJWT(tkExp, key, {algorithms:["HS256"]}), "expired throws");
        let tkNbf = JWTSign({sub:"u1", nbf: now+3600}, key, {alg:"HS256"});
        assertThrows(()=>o.verifyJWT(tkNbf, key, {algorithms:["HS256"]}), "nbf future throws");
        let tkSkew = JWTSign({sub:"u1", nbf: now+5}, key, {alg:"HS256"});
        assert(o.verifyJWT(tkSkew, key, {algorithms:["HS256"], clockSkewSec:10}).sub==="u1", "skew allows");
        {
            // A non-finite exp/nbf (JSON 1e400 parses to Infinity) is refused, not compared: an Infinity exp
            // never expires and a -Infinity nbf is always valid (deep line-read wave D-D, oauth2).
            const b64 = (s) => Base64URLEncode(new TextEncoder().encode(s));
            const raw = (payload) => { const h = b64('{"alg":"HS256","typ":"JWT"}') + "." + b64(payload); return h + "." + Base64URLEncode(crypto.HMAC("sha256", key, h)); };
            let m = "";
            try { o.verifyJWT(raw('{"sub":"u1","exp":1e400}'), key, {algorithms:["HS256"]}); } catch (e) { m = e.message; }
            assert(/exp must be finite/.test(m), "an Infinity exp is refused");
            m = "";
            try { o.verifyJWT(raw('{"sub":"u1","nbf":-1e400}'), key, {algorithms:["HS256"]}); } catch (e) { m = e.message; }
            assert(/nbf must be finite/.test(m), "a -Infinity nbf is refused");
            assert(o.verifyJWT(raw('{"sub":"u1","exp":4102444800}'), key, {algorithms:["HS256"]}).sub === "u1", "CONTROL: a hand-signed token with a finite exp verifies");
        }
        assertThrows(()=>o.verifyJWT(tk, key, {algorithms:["HS256"], aud:"https://other.example.com"}), "aud mismatch");
        let tkAudArr = JWTSign({sub:"u1", aud:["https://api.example.com","https://other.example.com"]}, key, {alg:"HS256"});
        assert(o.verifyJWT(tkAudArr, key, {algorithms:["HS256"], aud:"https://other.example.com"}).sub==="u1", "aud array contains");
        assertThrows(()=>o.verifyJWT(tk, key, {algorithms:["HS256"], aud:["https://other.example.com","https://x.example.com"]}), "aud array mismatch");
        assertThrows(()=>o.verifyJWT(tk, key, {algorithms:["HS256"], iss:"https://evil.example.com"}), "iss mismatch");
        assertThrows(()=>o.verifyJWT(tk, key, {algorithms:["HS256"], requiredScope:["admin"]}), "scope missing throws");
        assert(o.verifyJWT(tk, key, {algorithms:["HS256"], requiredScope:["read","write"]}).scope==="read write", "scope all present");
        let b64obj = (obj)=> Base64URLEncode(new TextEncoder().encode(JSON.stringify(obj)));
        let noneTok = b64obj({alg:"none",typ:"JWT"})+"."+b64obj({sub:"admin"})+".";
        assertThrows(()=>o.verifyJWT(noneTok, key, {algorithms:["none"]}), "alg:none even allowlisted throws");
        assertThrows(()=>o.verifyJWT(noneTok, key, {algorithms:["HS256"]}), "alg:none with HS256 allowlist throws");
        let hsTk = JWTSign({sub:"u1"}, key, {alg:"HS256"});
        assertThrows(()=>o.verifyJWT(hsTk, key, {algorithms:["RS256"]}), "HS256 not in RS256 allowlist");
        let parts = tk.split(".");
        let tampered = parts[0]+"."+ b64obj({sub:"admin", aud:"https://api.example.com", iss:"https://auth.example.com", exp: now+3600}) +"."+parts[2];
        assertThrows(()=>o.verifyJWT(tampered, key, {algorithms:["HS256"], aud:"https://api.example.com"}), "tampered payload throws");
        let wrongKey = new TextEncoder().encode("wrong-secret-32-bytes-long-yyyyyy");
        assertThrows(()=>o.verifyJWT(tk, wrongKey, {algorithms:["HS256"]}), "wrong key throws");
        for(let alg of ["HS384","HS512"]){
            let t = JWTSign({x:1}, key, {alg});
            assert(o.verifyJWT(t, key, {algorithms:[alg]}).x===1, alg+" round trip");
            assertThrows(()=>o.verifyJWT(t, key, {algorithms:["HS256"]}), alg+" wrong allowlist");
        }
        let tkExpNow = JWTSign({sub:"u1", exp: now}, key, {alg:"HS256"});
        assertThrows(()=>o.verifyJWT(tkExpNow, key, {algorithms:["HS256"]}), "exp == now throws");
        assert(o.verifyJWT(tkExpNow, key, {algorithms:["HS256"], clockSkewSec:10}).sub==="u1", "exp == now passes with skew");
        assertThrows(()=>o.verifyJWT(tk, key, {algorithms:["HS256"], clockSkewSec:301}), "clockSkewSec 301 throws");
        assertThrows(()=>o.verifyJWT(tk, key, {algorithms:["HS256"], clockSkewSec:-1}), "clockSkewSec -1 throws");
        assertThrows(()=>o.verifyJWT(tk, key, {algorithms:["HS256"], clockSkewSec:60000}), "clockSkewSec 60000 throws");
        assertThrows(()=>o.verifyJWT(tk, key, {algorithms:["HS256"], requiredScope:"read"}), "string requiredScope throws");
        let forgedRan = false;
        try { globalThis.__oauth_jwt_verify = function(){ forgedRan = true; return {sub:"admin"}; }; } catch (e) {}
        assert(o.verifyJWT(tk, key, {algorithms:["HS256"]}).sub==="u1", "verifyJWT unaffected by forgery attempt");
        assert(!forgedRan, "forged bridge never invoked");
        assertThrows(()=>{ globalThis.__oauth_jwt_verify = function(){ return {sub:"admin"}; }; }, "assigning the frozen bridge slot throws");
        assertThrows(()=>o.verifyJWT(tk, key, {algorithms:[]}), "empty allowlist");
        assertThrows(()=>o.verifyJWT(tk, key, {}), "missing algorithms");
        assertThrows(()=>o.verifyJWT("", key, {algorithms:["HS256"]}), "empty token");
        assertThrows(()=>o.verifyJWT("not.a.jwt", key, {algorithms:["HS256"]}), "malformed too few parts");
    }
});
__kitS.end();

print("test_oauth2: all "+n+" tests passed");
