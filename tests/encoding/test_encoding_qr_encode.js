// covers: dyna:encoding.QREncode
import { QREncode, QRToString } from "dyna:encoding";
import { QR_CASES } from "../lib/encoding_qr_table.js";
let n = 0, bad = 0;
function chk(txt, ecl, mask, wantVer, want) {
  n++; const q = QREncode(txt, { ecc: ecl, mask: mask });
  let got = ""; for (let i = 0; i < q.modules.length; i++) got += q.modules[i] ? "1" : "0";
  if (q.version !== wantVer) { bad++; if (bad<5) print("FAIL ver " + txt.slice(0,10) + " " + ecl + " m" + mask + ": v" + q.version + " want v" + wantVer); return; }
  if (got !== want) { bad++; if (bad<5) print("FAIL " + txt.slice(0,10) + " " + ecl + " m" + mask + " v" + wantVer); }
}
for (const c of QR_CASES) chk(...c);

function threw(fn, re, m) {
    n++;
    try { fn(); bad++; print("FAIL " + m + ": did not throw"); }
    catch (e) { if (re && !re.test(e.message)) { bad++; print("FAIL " + m + ": " + e.message); } }
}
threw(() => QREncode(), /required/, "no text");
threw(() => QREncode("x", { ecc: "Z" }), /ecc is/, "a bad ecc level");
threw(() => QREncode("x", { version: 0 }), /version is 1 to 40/, "version 0");
threw(() => QREncode("x", { version: 41 }), /version is 1 to 40/, "version 41");
threw(() => QREncode("x", { mask: 8 }), /mask is 0 to 7/, "mask 8");
threw(() => QREncode("z".repeat(3000)), /do not fit/, "past the largest symbol");
threw(() => QREncode("z".repeat(100), { version: 1 }), /do not fit/,
      "too much data for a pinned version");

{   n++;
    const q = QREncode("dynascript");
    if (!(q.size === 17 + q.version * 4 && q.modules.length === q.size * q.size))
        { bad++; print("FAIL auto: size does not match version"); }
}

if (bad) { print("test_qr: " + bad + " FAILED of " + n); throw new Error("test_qr failed"); }
print("test_qr: " + n + " assertions, 0 failures");
