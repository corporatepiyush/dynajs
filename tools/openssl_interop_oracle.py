#!/usr/bin/env python3
"""CY-8 oracle: independent DER parse of openssl PEMs vs dynajs converters."""
import json
import subprocess
import sys

D = sys.argv[1]
js = json.loads(sys.argv[2])


def der_len(b, i):
    n = b[i]
    i += 1
    if n < 0x80:
        return n, i
    k = n & 0x7F
    v = 0
    for _ in range(k):
        v = (v << 8) | b[i]
        i += 1
    return v, i


def tlv(b, i):
    tag = b[i]
    ln, j = der_len(b, i + 1)
    return tag, b[j:j + ln], j + ln


def pem_body(path):
    lines = open(path).read().splitlines()
    import base64
    return base64.b64decode("".join(l for l in lines if "---" not in l))


def priv_raw(path):
    seq = pem_body(path)
    assert seq[0] == 0x30
    _, content, _ = tlv(seq, 0)
    i = 0
    # walk: INTEGER, SEQUENCE(alg), OCTET STRING
    t, v, i = tlv(content, 0)
    assert t == 0x02, t
    t, v, i = tlv(content, i)
    assert t == 0x30, t
    t, v, i = tlv(content, i)
    assert t == 0x04, t
    # inner OCTET STRING wrap 04 20 <32>
    assert v[0] == 0x04 and v[1] == 0x20 and len(v) == 34, v.hex()
    return v[2:].hex()


def pub_raw(path):
    seq = pem_body(path)
    _, content, _ = tlv(seq, 0)
    i = 0
    t, v, i = tlv(content, 0)
    assert t == 0x30, t  # alg seq
    t, v, i = tlv(content, i)
    assert t == 0x03, t  # BIT STRING
    assert v[0] == 0x00 and len(v) == 33, v.hex()
    return v[1:].hex()


fails = []


def eq(name, a, b):
    if a != b:
        fails.append(f"{name}: dynajs={a} oracle={b}")
    print(f"{'FAIL' if a != b else 'ok'} {name}")


eq("Ed_priv", js["Ed_privRaw"], priv_raw(f"{D}/ed_priv.pem"))
eq("X_priv", js["X_privRaw"], priv_raw(f"{D}/x_priv.pem"))
eq("Ed_pub", js["Ed_pubRaw"], pub_raw(f"{D}/ed_pub.pem"))
eq("X_pub", js["X_pubRaw"], pub_raw(f"{D}/x_pub.pem"))
eq("Ed_pubOnly", js["Ed_pubOnlyRaw"], pub_raw(f"{D}/ed_pub.pem"))
eq("X_pubOnly", js["X_pubOnlyRaw"], pub_raw(f"{D}/x_pub.pem"))
print("pubOnlyNoPriv:", js["Ed_pubOnlyHasNoPriv"] and js["X_pubOnlyHasNoPriv"])

# openssl re-reads the dynajs-regenerated PEMs. Public PEMs need -pubin:
# without it `pkey -in` demands a private key and fails on ANY pubkey file,
# including openssl's own -- not an engine signal.
for name in ["Ed_rt_priv", "Ed_rt_pub", "X_rt_priv", "X_rt_pub"]:
    cmd = ["openssl", "pkey", "-in", f"{D}/{name}.pem", "-noout", "-text"]
    if name.endswith("_pub"):
        cmd.append("-pubin")
    p = subprocess.run(cmd, capture_output=True, text=True)
    ok = p.returncode == 0
    print(f"{'ok' if ok else 'FAIL'} openssl-reads-{name}")
    if not ok:
        fails.append(f"openssl rejects {name}: {p.stderr.strip()}")
    if "PRIVATE" in name.upper() and "priv:" not in p.stdout and "priv" not in p.stdout.lower():
        pass

# regenerated private PEM must carry the SAME key material (compare openssl text)
for alg, lo in [("Ed", "ed"), ("X", "x")]:
    a = subprocess.run(["openssl", "pkey", "-in", f"{D}/{lo}_priv.pem", "-text", "-noout"],
                       capture_output=True, text=True).stdout
    b = subprocess.run(["openssl", "pkey", "-in", f"{D}/{alg}_rt_priv.pem", "-text", "-noout"],
                       capture_output=True, text=True).stdout
    same = a == b
    print(f"{'ok' if same else 'FAIL'} {alg}-roundtrip-identical")
    if not same:
        fails.append(f"{alg} regenerated PEM differs")

print("CY8-ORACLE:", "FAIL" if fails else "PASS")
sys.exit(1 if fails else 0)
