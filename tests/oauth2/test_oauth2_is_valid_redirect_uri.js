// covers: dyna:oauth2.isValidRedirectUri
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_oauth2_is_valid_redirect_uri");
import * as o from "dyna:oauth2";
import * as crypto from "dyna:crypto";
import { Base64URLEncode } from "dyna:encoding";
let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function assertThrows(fn, msg) { n++; let threw=false; try{fn();}catch(e){threw=true;} if(!threw) throw new Error("expected throw: "+msg); }
function assertEq(a,b,msg){ n++; if(a!==b) throw new Error(msg+": expected "+JSON.stringify(b)+" got "+JSON.stringify(a)); }

__kitS.section("exact true", () => {
    assert(o.isValidRedirectUri("https://client.example.com/cb", ["https://client.example.com/cb"]), "exact true");
    assert(!o.isValidRedirectUri("https://client.example.com/cb/", ["https://client.example.com/cb"]), "trailing slash false");
    assert(o.isValidRedirectUri("http://127.0.0.1:54321/cb", ["http://127.0.0.1:8080/cb"]), "loopback port ignore");
    assert(o.isValidRedirectUri("http://[::1]:54321/cb", ["http://[::1]:8080/cb"]), "ipv6 loopback true");
    assert(!o.isValidRedirectUri("http://127.0.0.1:54321/cb", ["https://client.example.com/cb"]), "loopback vs https false");
    assert(!o.isValidRedirectUri("https://evil.com/cb", ["https://client.example.com/cb"]), "evil false");
    assert(!o.isValidRedirectUri("https://client.example.com/cb\n", ["https://client.example.com/cb"]), "CTL false");
    assert(o.isValidRedirectUri("com.example.app:/oauth2redirect", ["com.example.app:/oauth2redirect"]), "private-use true");
    assert(!o.isValidRedirectUri("myapp:/oauth2redirect", ["myapp:/oauth2redirect"]), "private without dot false");
    assert(!o.isValidRedirectUri("https://client.example.com/cb?evil=1", ["https://client.example.com/cb"]), "query mismatch false");
});

__kitS.section("userinfo smuggle rejected", () => {
    assert(!o.isValidRedirectUri("http://127.0.0.1:8080@evil.com/cb", ["http://127.0.0.1/cb"]), "userinfo smuggle rejected");
    assert(!o.isValidRedirectUri("http://127.0.0.1:8080@127.0.0.1.evil.com/cb", ["http://127.0.0.1/cb"]), "userinfo smuggle subdomain rejected");
    assert(!o.isValidRedirectUri("http://[::1]:8080@evil.com/cb", ["http://[::1]/cb"]), "ipv6 userinfo smuggle rejected");
    assert(!o.isValidRedirectUri("http://[::1]evil.com/cb", ["http://[::1]/cb"]), "ipv6 trailing junk rejected");
    assert(!o.isValidRedirectUri("http://[::1/cb", ["http://[::1]/cb"]), "unterminated ipv6 bracket rejected");
    assert(!o.isValidRedirectUri("http://127.0.0.1:80a/cb", ["http://127.0.0.1/cb"]), "non-digit port not ignored");
    assert(o.isValidRedirectUri("http://127.0.0.1:54321/cb", ["http://127.0.0.1/cb"]), "loopback port-ignore to portless registration");
    assert(o.isValidRedirectUri("http://localhost:8443/cb", ["http://localhost/cb"]), "localhost port-ignore");
    assert(!o.isValidRedirectUri("https://app.example.com:8443/cb", ["https://app.example.com/cb"]), "non-loopback ports are NOT ignored");
});
__kitS.end();

print("test_oauth2: all "+n+" tests passed");
