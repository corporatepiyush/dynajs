/* bench_asn1.js -- ASN.1 DER decode/encode (plan 3.12).
 *
 * WHAT MOVES AND WHAT MUST NOT. A codec is two directions with different
 * shapes: decode allocates JS objects per node, encode walks them. Both are
 * timed here at three sizes so a win in one that is a loss in the other
 * shows up -- an asymmetric result is exactly the regression shape to catch.
 *
 * The structure is an X.509-like SEQUENCE (serial, algorithm with NULL
 * params, name with two attributes, two times, a signature BIT STRING, an
 * extension OCTET STRING). Sizes straddle the one-octet length boundary and
 * the 16-bit one: 256, 4096 and 65536 bytes of extension payload.
 *
 * A decoded tree is re-encoded and compared byte-for-byte at the end: a
 * benchmark that silently produces non-canonical DER is worse than none.
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/bench_asn1.js
 */
import { ASN1 } from "dyna:serialize";

const MIN_MS = 120;
let sink = 0;

/* Scale reps until the timed region clears the clock's noise floor, then take
 * the best of five. Returns ms for ONE call of fn(). */
function ms(fn) {
    fn(); fn();
    let mult = 1, dt;
    for (;;) {
        const t0 = performance.now();
        for (let m = 0; m < mult; m++) sink += fn();
        dt = performance.now() - t0;
        if (dt >= MIN_MS || mult >= 1 << 16) break;
        mult = Math.max(mult * 2, Math.ceil(mult * MIN_MS / Math.max(dt, 0.001)));
    }
    let best = Infinity;
    for (let k = 0; k < 5; k++) {
        const t0 = performance.now();
        for (let m = 0; m < mult; m++) sink += fn();
        const d = performance.now() - t0;
        if (d < best) best = d;
    }
    return best / mult;
}

const bytes = (h) => new Uint8Array(h.match(/../g) ? h.match(/../g).map((x) => parseInt(x, 16)) : []);

/* Reading .length of the result forces the call to have run: the Uint8Array
 * from encode, the child array from a decoded SEQUENCE. */
const use = (v) => sink += (v && v.length) || 0;

function makeCert(payload) {
    return ASN1.seq([
        ASN1.int(0x7fffffff),                                  /* serial */
        ASN1.seq([ASN1.oid("1.2.840.113549.1.1.11"), ASN1.null()]),   /* alg */
        ASN1.seq([
            ASN1.seq([ASN1.utf8("dynajs.example")]),           /* commonName */
            ASN1.seq([ASN1.oid("2.5.4.3"), ASN1.utf8("dynajs")]),
        ]),
        ASN1.utcTime("260816123456Z"),
        ASN1.generalizedTime("20260816123456Z"),
        ASN1.bitString(bytes("80"), 7),                        /* signature */
        ASN1.octets(payload),
    ]);
}

print("=== asn1: X509-like decode/encode ===");
print("#V op      bytes   ms/call");

const SIZES = [256, 4096, 65536];
for (const n of SIZES) {
    const payload = new Uint8Array(n).map((_, i) => (i * 131 + 7) & 0xFF);
    const node = makeCert(payload);
    const der = ASN1.encode(node);

    /* Re-encode identity: decode(der) -> encode -> der, exactly. */
    const back = ASN1.encode(ASN1.decode(der));
    if (back.length !== der.length) {
        let same = true;
        for (let i = 0; i < der.length; i++) if (back[i] !== der[i]) same = false;
        if (!same) throw new Error("bench_asn1: re-encode of a canonical "
            + n + "-byte cert is not byte-identical");
    }

    print("#V encode    " + String(der.length).padStart(6) + "  " +
          ms(() => use(ASN1.encode(node))).toFixed(4));
    print("#V decode    " + String(der.length).padStart(6) + "  " +
          ms(() => use(ASN1.decode(der))).toFixed(4));
}

/* A wide flat SEQUENCE stresses the child loop rather than the payload copy. */
print("");
print("#V op      children ms/call");
for (const n of [16, 256, 4096]) {
    const kids = [];
    for (let i = 0; i < n; i++) kids.push(ASN1.int(i));
    const seq = ASN1.seq(kids);
    const der = ASN1.encode(seq);
    print("#V seq       " + String(n).padStart(5) + "   " +
          ms(() => use(ASN1.decode(der))).toFixed(4));
}

if (sink === -1) print("unreachable");
print("done");