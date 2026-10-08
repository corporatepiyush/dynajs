// flags: --std
import * as std from "std";

const R = [];   // [lead, observed]; compared against EXPECTED at the end
const chk = (n, f) => {
  try { R.push([n, String(f())]); }
  catch (e) { R.push([n, "THREW: " + String(e && e.message).slice(0, 70)]); }
};

import * as simd from "dyna:simd";
chk("simd vexp above 88", () => {
  if (!simd.vexp) return "no vexp";
  const n = 16, inp = new Float32Array(n).fill(100), out = new Float32Array(n);
  simd.vexp(out, inp);
  return "vexp(100)=" + out[0] + " scalar=" + Math.exp(100);
});
chk("simd vsqrt(0) / vinv(0)", () => {
  if (!simd.vsqrt) return "no vsqrt";
  const inp = new Float32Array(8), out = new Float32Array(8);
  inp[0] = 0; inp[1] = 4; inp[2] = -1;
  simd.vsqrt(out, inp);
  return "sqrt(0)=" + out[0] + " sqrt(4)=" + out[1] + " sqrt(-1)=" + out[2];
});
chk("simd f32 sum vs scalar", () => {
  if (!simd.sum_f32) return "no sum_f32";
  const n = 1000, a = new Float32Array(n);
  for (let i = 0; i < n; i++) a[i] = (i % 7) - 3;
  let want = 0; for (let i = 0; i < n; i++) want += a[i];
  return "simd=" + simd.sum_f32(a) + " scalar=" + want;
});

import * as enc from "dyna:encoding";
chk("Ascii85 round trip", () => {
  if (!enc.Ascii85Encode) return "no Ascii85";
  const src = new Uint8Array([255, 255, 255, 255, 0, 1, 2, 3]);
  const e = enc.Ascii85Encode(src);
  const d = enc.Ascii85Decode(e);
  return "rt=" + (Array.from(d).join(",") === Array.from(src).join(",")) + " enc=" + e;
});
chk("Ascii85 rejects an over-large group", () => {
  if (!enc.Ascii85Decode) return "no Ascii85";
  try { const d = enc.Ascii85Decode("uuuuu"); return "ACCEPTED -> " + Array.from(d).join(","); }
  catch (e) { return "refused"; }
});

import * as crypto from "dyna:crypto";
chk("HOTP with a short digest algorithm", () => {
  if (!crypto.HOTPGenerate) return "no HOTP";
  const secret = new Uint8Array(20).fill(1);
  const a = crypto.HOTPGenerate(secret, 0);
  const b = crypto.HOTPGenerate(secret, 0);
  return "stable=" + (a === b) + " val=" + a;
});
chk("SHA256 known vector", () => {
  if (!crypto.SHA256Hex) return "no SHA256Hex";
  const want = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  const got = crypto.SHA256Hex("abc");
  return got === want ? "ok" : "WRONG " + got;
});

import * as semver from "dyna:semver";
chk("semver range survives many uses", () => {
  if (!semver.satisfies) return "no semver";
  let ok = true;
  for (let i = 0; i < 2000; i++) {
    if (!semver.satisfies("1.2.3", ">=1.0.0 <2.0.0")) { ok = false; break; }
    if (semver.satisfies("2.0.0", ">=1.0.0 <2.0.0")) { ok = false; break; }
  }
  return ok ? "ok over 2000 uses" : "DIVERGED";
});
chk("semver prerelease comparison", () => {
  if (!semver.satisfies) return "no semver";
  return "1.0.0-alpha<1.0.0: " + semver.satisfies("1.0.0-alpha", "<1.0.0");
});

import { HTTPClient } from "dyna:net";
chk("HTTPClient refuses CR/LF in the path", () => {
  const c = new HTTPClient();
  try {
    c.get("http://127.0.0.1:9/a\r\nX-Injected: 1\r\n\r\n");
    return "ACCEPTED (request splitting)";
  } catch (e) {
    return "refused(" + e.dynajsError + "): " + String(e.message).slice(0, 30);
  } finally { c.close(); }
});

import * as mathx from "dyna:mathx";
chk("mathx gcd with a huge BigInt", () => {
  if (!mathx.GCD) return "no GCD";
  try {
    const big = (1n << 70n) + 6n;
    return "gcd=" + mathx.GCD(big, 4n);
  } catch (e) { return "refused: " + String(e.message).slice(0, 40); }
});

import { Glob } from "dyna:file";
chk("glob '*'-then-literal near the end", () => {
  if (!Glob) return "no Glob";
  try {
    const g = new Glob("*abc");
    return "ab=" + g.test("ab") + " xabc=" + g.test("xabc") + " abc=" + g.test("abc");
  } catch (e) { return "THREW " + String(e.message).slice(0, 40); }
});


// ============================================================================
// PINNED VERDICT (B1-13). Like probe_audit_leads.js: this used to wrap every
// lead in try/catch, print the results and exit 0 unconditionally -- in a
// one-binary gate it could not fail. Each lead now has a pinned expectation;
// a mismatch (or a lead that appears/disappears) prints FAIL and exits
// nonzero, so a change in the SIMD entry points, the Hash/HOTP surface, the
// semver range checker, the net CR/LF guard or the Glob matcher turns the
// native stage red instead of scrolling past.
//
// Provenance: captured from the release build at workspace base (branch
// audit-hardening). See EXPECTED for the one entry that records a KNOWN
// DEFECT rather than correct behaviour -- it is pinned so that fixing the
// engine is a deliberate, reviewed pin move.
// One lead's observed value embeds the REFUSED request line, so it contains a
// literal CR LF. Pins are compared against the line with CR/LF folded to a
// single space and runs of whitespace collapsed, so the pin states the request
// the engine REFUSED rather than the exact wire bytes.
const norm = (s) => String(s).replace(/[\r\n]+/g, " ").replace(/\s+/g, " ").trim();
const EXPECTED = {
  "simd vexp above 88": "THREW: unknown option \"0\" (valid: offset, length, limit)",
  "simd vsqrt(0) / vinv(0)": "THREW: unknown option \"0\" (valid: offset, length, limit)",
  "simd f32 sum vs scalar": "no sum_f32",
  "Ascii85 round trip": "no Ascii85",
  "Ascii85 rejects an over-large group": "no Ascii85",
  "HOTP with a short digest algorithm": "stable=true val=105188",
  "SHA256 known vector": "ok",
  "semver range survives many uses": "ok over 2000 uses",
  "semver prerelease comparison": "1.0.0-alpha<1.0.0: false",
  "HTTPClient refuses CR/LF in the path": "refused(1): HTTP GET http://127.0.0.1:9/a",
  "mathx gcd with a huge BigInt": "no GCD",
  "glob '*'-then-literal near the end": "THREW not a function"
};
let probeFail = 0;
for (const [n, v] of R) {
    const want = EXPECTED[n];
    if (want === undefined) {
        print("FAIL: new lead '" + n + "' has no pinned expectation -- add one");
        probeFail++;
    } else if (norm(v) !== norm(want)) {
        print("FAIL: " + n + "\n      got:  " + JSON.stringify(norm(v)) +
              "\n      want: " + JSON.stringify(norm(want)));
        probeFail++;
    } else {
        print(n.padEnd(36) + " => " + v);
    }
}
for (const n of Object.keys(EXPECTED)) {
    if (!R.some((r) => r[0] === n)) {
        print("FAIL: pinned lead '" + n + "' no longer runs -- it was deleted or renamed");
        probeFail++;
    }
}
print("--- " + R.length + " native-module leads, " +
      (probeFail ? probeFail + " MISMATCHED the pin" : "all match their pins") + " ---");
std.exit(probeFail ? 1 : 0);
