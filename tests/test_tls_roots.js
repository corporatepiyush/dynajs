/* test_tls_roots.js -- plan 3.2: pin the CA trust store on macOS and Linux.
 *
 * dyna:tls is a C-only adapter (src/dyna-tls.c) with no JS module surface,
 * so this test pins the DATA the C loader consumes, identically on both
 * platforms, with NO network and NO CONFIG_TLS needed (it runs in the plain
 * native build):
 *   1. the VENDORED Mozilla bundle tests/corpus/ca-bundle.pem -- the
 *      deterministic fallback everywhere (NEVER SKIPPED);
 *   2. the per-OS platform store: macOS Security framework system keychain,
 *      Linux distro bundle (skip loudly only if genuinely absent).
 * Pinned root: ISRG Root X1. SHA-256 fingerprint lives in
 * tests/corpus/isrg_root_x1.pin -- the ONE source of truth, read by both
 * this test and tests/gen_ca_bundle.sh (a stale pin cannot diverge).
 * The bundle floor (>100 roots) guards a truncated or stale generation.
 *
 * Run: make CONFIG_NATIVE_MODULES=y && ./dynajs tests/test_tls_roots.js
 * No network. A skip is loud, and fatal under DYNAJS_REQUIRE_TOOLS=1.
 */
import { SHA256Hex } from "dyna:hash";
import { Base64Decode } from "dyna:encoding";
import { Exec, platform, getEnv } from "dyna:sys";
import * as std from "std";

const PIN_FILE = std.loadFile("tests/corpus/isrg_root_x1.pin");
const PIN = PIN_FILE === null ? "" : PIN_FILE.trim();
const FLOOR = 100;   /* Mozilla bundle sanity floor: see header */
const REQUIRE = getEnv("DYNAJS_REQUIRE_TOOLS") === "1";
let pass = 0, fail = 0, skip = 0;

if (PIN.length !== 64) {
    fail++;
    print("  FAIL  tests/corpus/isrg_root_x1.pin is missing or malformed");
}

function ok(cond, what, detail) {
    if (cond) { pass++; print("  ok    " + what); }
    else { fail++; print("  FAIL  " + what + (detail ? "  [" + detail + "]" : "")); }
}
function skipped(what) {
    if (REQUIRE) { fail++; print("  FAIL  REQUIRED: " + what); return; }
    skip++; print("  SKIP  " + what);
}

/* SHA-256 fingerprints (lowercase hex) of every cert in a PEM text. */
function fingerprints(pem) {
    const out = [];
    const re = /-----BEGIN CERTIFICATE-----([\s\S]*?)-----END CERTIFICATE-----/g;
    let m;
    while ((m = re.exec(pem)) !== null) {
        const b64 = m[1].replace(/\s+/g, "");
        out.push(SHA256Hex(Base64Decode(b64)));
    }
    return out;
}

print("--- the vendored Mozilla bundle is the deterministic fallback (NEVER SKIPPED) ---");
{
    const pem = std.loadFile("tests/corpus/ca-bundle.pem");
    if (pem === null) {
        fail++;
        print("  FAIL  tests/corpus/ca-bundle.pem is missing. Run tests/gen_ca_bundle.sh");
        print("        once and commit the output -- the C loader reads the same file.");
    } else {
        const fps = fingerprints(pem);
        ok(fps.length > FLOOR, "vendored bundle holds >" + FLOOR + " roots (" + fps.length + ")");
        ok(fps.indexOf(PIN) >= 0, "pinned root ISRG Root X1 is in the vendored bundle");
    }
}

print("--- the per-OS platform store carries the same pinned root ---");
{
    const os = platform();
    if (os === "darwin") {
        let r;
        try {
            r = Exec("security", ["find-certificate", "-a", "-p",
                "/System/Library/Keychains/SystemRootCertificates.keychain"]);
        } catch (e) {
            r = null;
            skipped("macOS security CLI unavailable (" + (e.message || e) + ")");
        }
        if (r) {
            if (r.code !== 0) skipped("macOS keychain export failed (code " + r.code + ")");
            else {
                const fps = fingerprints(String(r.stdout));
                ok(fps.length > 50, "macOS keychain anchors export (" + fps.length + ")");
                ok(fps.indexOf(PIN) >= 0, "ISRG Root X1 is in the macOS system keychain");
            }
        }
    } else if (os === "linux") {
        const paths = ["/etc/ssl/certs/ca-certificates.crt",
                       "/etc/pki/tls/certs/ca-bundle.crt"];
        let pem = null, used = "";
        for (const p of paths) {
            const s = std.loadFile(p);
            if (s !== null) { pem = s; used = p; break; }
        }
        if (pem === null) skipped("no Linux distro bundle (" + paths.join(", ") + ")");
        else {
            const fps = fingerprints(pem);
            ok(fps.length > FLOOR, "distro bundle holds >" + FLOOR + " roots (" + fps.length + ")");
            ok(fps.indexOf(PIN) >= 0, "ISRG Root X1 is in the Linux distro bundle (" + used + ")");
        }
    } else {
        skipped("unknown platform '" + os + "' -- no per-OS store to probe");
    }
}

print("test_tls_roots: " + pass + " passed, " + fail + " failed, " + skip + " skipped");
if (fail) throw new Error("test_tls_roots: " + fail + " failures");
