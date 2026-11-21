// covers: dyna:oauth2.parseScope
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_oauth2_parse_scope");
import * as o from "dyna:oauth2";
import * as crypto from "dyna:crypto";
import { Base64URLEncode } from "dyna:encoding";
let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function assertThrows(fn, msg) { n++; let threw=false; try{fn();}catch(e){threw=true;} if(!threw) throw new Error("expected throw: "+msg); }
function assertEq(a,b,msg){ n++; if(a!==b) throw new Error(msg+": expected "+JSON.stringify(b)+" got "+JSON.stringify(a)); }

__kitS.section("read write", () => {
    assert(JSON.stringify(o.parseScope("read write"))===JSON.stringify(["read","write"]), "parseScope");
    assert(JSON.stringify(o.parseScope("read"))===JSON.stringify(["read"]), "parseScope single");
    assert(o.formatScope(["read","write"])==="read write", "formatScope");
    assertThrows(()=>o.parseScope("read  write"), "double space empty token");
    assertThrows(()=>o.parseScope("read write "), "trailing space");
    assertThrows(()=>o.parseScope("read \"write\""), "quote invalid");
    assertThrows(()=>o.parseScope("read\\write"), "backslash invalid");
    assert(JSON.stringify(o.parseScope(""))===JSON.stringify([]), "empty scope []");
    assert(o.formatScope([])==="", "empty format");
    assertThrows(()=>o.formatScope(["a", "b c"]), "formatScope invalid char");
    assertThrows(()=>o.formatScope([""]), "empty token");
    assertThrows(()=>o.formatScope(["a".repeat(65)]), "too long");
});
__kitS.end();

print("test_oauth2: all "+n+" tests passed");
