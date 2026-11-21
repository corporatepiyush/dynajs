// flags: --std
// Ed25519/X25519 PEM interop against openssl, with an oracle that shares no code with dyna:crypto.
//
//   ./dynajs --std tools/openssl_interop_oracle.js [DIR]
//
// DIR gets fresh keys from `openssl genpkey` (ed_priv.pem, ed_pub.pem, x_priv.pem, x_pub.pem) unless
// they are already there. dyna:crypto's PemToRaw reads them; the raw bytes are compared with a DER
// walk written here (SEQUENCE { INTEGER, AlgorithmIdentifier, OCTET STRING { OCTET STRING 32 } } for
// the private key, SEQUENCE { AlgorithmIdentifier, BIT STRING 0x00 || 32 } for the public key).
// PemFromRaw's output is written back as *_rt_*.pem, which openssl must read, and the private
// round trip must print exactly what openssl prints for the original. Exit 1 on any difference.
import * as std from "std";
import { Exec } from "dyna:sys";
import { readFile, writeFile, Path } from "dyna:file";
import * as c from "dyna:crypto";

const dir = scriptArgs[1] || (std.getenv("TMPDIR") || "/tmp").replace(/\/$/, "") + "/openssl-oracle";
Exec("mkdir", ["-p", dir]);
const P = (n) => new Path(dir + "/" + n);
const has = (n) => Exec("test", ["-f", dir + "/" + n]).code === 0;
const fails = [];
const openssl = (args) => Exec("openssl", args);
for (const [lo, alg] of [["ed", "ED25519"], ["x", "X25519"]]) {
    if (!has(lo + "_priv.pem")) openssl(["genpkey", "-algorithm", alg, "-out", dir + "/" + lo + "_priv.pem"]);
    if (!has(lo + "_pub.pem")) openssl(["pkey", "-in", dir + "/" + lo + "_priv.pem", "-pubout", "-out", dir + "/" + lo + "_pub.pem"]);
}

const hex = (u8) => Array.from(u8, (b) => b.toString(16).padStart(2, "0")).join("");
function pemBody(name) {
    const b64 = readFile(P(name)).split("\n").filter((l) => !l.includes("---")).join("");
    const bin = atob(b64), out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
}
function tlv(b, i) {
    const tag = b[i];
    let n = b[i + 1], j = i + 2;
    if (n >= 0x80) { const k = n & 0x7f; n = 0; for (let q = 0; q < k; q++) n = n * 256 + b[j++]; }
    return { tag, v: b.subarray(j, j + n), next: j + n };
}
function expect(cond, what) { if (!cond) throw new Error("DER: " + what); }
function privRaw(name) {
    const seq = pemBody(name);
    expect(seq[0] === 0x30, "outer SEQUENCE");
    const body = tlv(seq, 0).v;
    let t = tlv(body, 0); expect(t.tag === 0x02, "version INTEGER");
    t = tlv(body, t.next); expect(t.tag === 0x30, "AlgorithmIdentifier");
    t = tlv(body, t.next); expect(t.tag === 0x04, "privateKey OCTET STRING");
    expect(t.v[0] === 0x04 && t.v[1] === 0x20 && t.v.length === 34, "inner OCTET STRING of 32 bytes");
    return hex(t.v.subarray(2));
}
function pubRaw(name) {
    const body = tlv(pemBody(name), 0).v;
    let t = tlv(body, 0); expect(t.tag === 0x30, "AlgorithmIdentifier");
    t = tlv(body, t.next); expect(t.tag === 0x03, "BIT STRING");
    expect(t.v[0] === 0x00 && t.v.length === 33, "BIT STRING 0x00 || 32 bytes");
    return hex(t.v.subarray(1));
}
function eq(name, a, b) {
    print((a === b ? "ok " : "FAIL ") + name);
    if (a !== b) fails.push(`${name}: dynajs=${a} oracle=${b}`);
}

for (const [alg, lo, pre] of [["Ed", "ed", "Ed25519"], ["X", "x", "X25519"]]) {
    const fromPriv = c[pre + "PemToRaw"](readFile(P(lo + "_priv.pem")));
    const fromPub = c[pre + "PemToRaw"](readFile(P(lo + "_pub.pem")));
    eq(alg + "_priv", hex(fromPriv.privateKey), privRaw(lo + "_priv.pem"));
    eq(alg + "_pub", hex(fromPriv.publicKey), pubRaw(lo + "_pub.pem"));
    eq(alg + "_pubOnly", hex(fromPub.publicKey), pubRaw(lo + "_pub.pem"));
    eq(alg + "_pubOnlyHasNoPriv", fromPub.privateKey, undefined);
    const back = c[pre + "PemFromRaw"]({ privateKey: fromPriv.privateKey, publicKey: fromPriv.publicKey });
    writeFile(P(alg + "_rt_priv.pem"), back.privateKey);
    writeFile(P(alg + "_rt_pub.pem"), back.publicKey);
    for (const kind of ["priv", "pub"]) {
        const args = ["pkey", "-in", dir + "/" + alg + "_rt_" + kind + ".pem", "-noout", "-text"];
        if (kind === "pub") args.push("-pubin");
        const r = openssl(args);
        eq("openssl-reads-" + alg + "_rt_" + kind, r.code, 0);
    }
    const a = openssl(["pkey", "-in", dir + "/" + lo + "_priv.pem", "-text", "-noout"]).stdout;
    const b = openssl(["pkey", "-in", dir + "/" + alg + "_rt_priv.pem", "-text", "-noout"]).stdout;
    eq(alg + "-roundtrip-identical", a, b);
}
for (const f of fails) print("  " + f);
print("openssl-interop-oracle: " + (fails.length ? "FAIL" : "PASS"));
std.exit(fails.length ? 1 : 0);
