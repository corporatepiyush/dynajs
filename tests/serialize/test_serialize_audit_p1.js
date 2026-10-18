// flags: --std
// Split from tests/test_audit_p1.js: the serialize sections.
import { Proto } from "dyna:serialize";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { n++; if (a !== b) { fails++; print("FAIL: " + m + " got " + JSON.stringify(a) + " want " + JSON.stringify(b)); } }
function throws(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL: " + m + " did not throw"); }
    catch (e) {
        if (re && !re.test(String(e.message))) { fails++; print("FAIL: " + m + " wrong message: " + e.message); }
    }
}
{
    const schema = { fields: [{ name: "a", number: 1, type: "int32" }] };
    const tag = ( (1n<<32n | 1n) <<3n );
    const tb = (()=>{ let nn=tag, b=[]; while(nn>0x7Fn){b.push(Number((nn&0x7Fn)|0x80n)); nn>>=7n} b.push(Number(nn)); return Uint8Array.from(b)})();
    const payload = new Uint8Array([...tb, 42]);
    throws(() => Proto.decode(payload, schema), /exceeds.*2\^29/, "protobuf high tag alias rejected");
    const normal = new Uint8Array([ (1<<3)|0, 99 ]);
    eq(Proto.decode(normal, schema).a, 99, "protobuf normal tag still decodes");
}
if (fails) { print("test_serialize_audit_p1: " + fails + " FAILED of " + n + " assertions"); throw new Error("test_serialize_audit_p1 failed"); }
print("test_serialize_audit_p1: " + n + " assertions, 0 failures");
