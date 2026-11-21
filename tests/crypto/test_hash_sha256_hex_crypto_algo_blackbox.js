// covers: dyna:hash.SHA256Hex
// Split from tests/test_algo_blackbox.js: the crypto sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_hash_sha256_hex_crypto_algo_blackbox");
import * as H from "dyna:hash";
import * as K from "dyna:crypto";

let pass=0, fail=0;
const ok=(c,w)=>{ if(c) pass++; else { fail++; print("  FAIL "+w); } };

__kitS.section("SHA256 abc", () => {
ok(H.SHA256Hex("abc")==="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA256 abc");
});

__kitS.section("SHA256 empty len", () => {
ok(H.SHA256Hex("")==="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" ||
   H.SHA256Hex("").length===64,"SHA256 empty len");
});

const bytes = new Uint8Array([0x61,0x62,0x63]);

__kitS.section("SHA256 Uint8Array == string", () => {
ok(H.SHA256Hex(bytes)===H.SHA256Hex("abc"),"SHA256 Uint8Array == string");
});

__kitS.section("SHA256 ArrayBuffer == string", () => {
ok(H.SHA256Hex(bytes.buffer)===H.SHA256Hex("abc"),"SHA256 ArrayBuffer == string");
print("black-box: "+pass+" passed, "+fail+" failed");
if (fail) throw new Error(fail+" black-box failures");
});
__kitS.end();
