// flags: --std
// Audit wave 3, batch 9: measured performance fixes. A speed-up is only
// admissible with a VALUE oracle beside it, so every section first pins what
// the operation must return (from a definition or a published vector), then
// states the scaling property the fix establishes.
// Cases marked [red] fail on the build before this batch.
import * as df from "dyna:dataframe";
import { AESGCM, ChaCha20Poly1305 } from "dyna:crypto";
import { args, Exec } from "dyna:sys";
import { Path, makeTempDir, writeFile, removeAll } from "dyna:file";
import { suite } from "./kit.js";

const t = suite("test_audit_w3_native_9");
const best = (f, reps = 5) => { let b = Infinity; for (let r = 0; r < reps; r++) { const t = performance.now(); f(); b = Math.min(b, performance.now() - t); } return b; };
const T = makeTempDir("w3n9_");

// ---- E2c-04: `local += x` with one int and one double --------------------
// ORACLE: ECMA-262 Number::add is IEEE-754 double addition after ToNumber;
// the expected value is computed through a different opcode path (a plain
// `a + b` on arguments), and Object.is distinguishes -0 and NaN.
// CONTROL: int += int and double += double are unchanged paths.
t.test("E2c-04 local += x with mixed int and double", ({ ok }) => {
    const add = (a, b) => a + b;
    const vals = [0, -0, 1, -1, 7, 2147483647, -2147483648, 0.5, -0.5, 1.5, 1e300, -1e300, 5e-324, NaN, Infinity, -Infinity, 2 ** 53, 123456789.987];
    let bad = 0, n = 0;
    const run = () => {
        for (const a of vals) for (const b of vals) {
            let s = a; s += b; n++;
            if (!Object.is(s, add(a, b))) bad++;
            let t = b; t += 1; n++;
            if (!Object.is(t, add(b, 1))) bad++;
            let u = 1; u += b; n++;
            if (!Object.is(u, add(1, b))) bad++;
        }
    };
    run(); run();
    ok(bad === 0, "E2c-04: `local += x` equals `a + b` for " + n + " int/double pairs (" + bad + " differ)");
    let s = 0.5; for (let i = 0; i < 1000; i++) s += i;
    ok(s === 499500.5, "control: a double accumulator of ints sums exactly (" + s + ")");
    let k = 2147483640; for (let i = 0; i < 20; i++) k += 1;
    ok(k === 2147483660, "control: int += int still widens past int32 (" + k + ")");
});

// ---- E2c-12: every loop shape is still interruptible ----------------------
// ORACLE: --timeout-ms ends a script with InternalError "interrupted" and
// status 1. Only a backward transfer can close a loop, so polling only taken
// backward branches must keep that true for every loop form; status 113 would
// be the hard-stop watchdog, meaning nothing in the loop looked at the clock.
// CONTROL: a script that finishes prints its result under the same flag.
t.test("E2c-12 every loop shape is interruptible", ({ ok }) => {
    const shapes = [
        "for(;;){}",
        "var i=0; do { i++; } while (i>=0 || i<0)",
        "var i=0; while (i < 1e12) { if (i & 1) i += 1; else i += 1; }",
        "var i=0; for(;;) { i = i ? 0 : 1; }",
        "var x=0; L: for(;;) { switch (x) { case 0: x=1; continue L; default: x=0; continue L; } }",
        "var a=[1]; for (var q of a) { a.push(1); }",
    ];
    let n = 0;
    for (const body of shapes) {
        const p = String(T) + "/loop" + (n++) + ".js";
        writeFile(new Path(p), body);
        const r = Exec(args()[0], ["--timeout-ms", "300", p], { timeoutMs: 20000, encoding: "utf8" });
        ok(r.code === 1 && /interrupted/.test(r.stderr), "E2c-12: `" + body.slice(0, 40) + "` is interrupted by the deadline (code " + r.code + ")");
    }
    const p = String(T) + "/done.js";
    writeFile(new Path(p), "var s=0; for (var i=0;i<1000;i++){ if (i&1) s+=i; } print(s);");
    const r = Exec(args()[0], ["--timeout-ms", "5000", p], { timeoutMs: 20000, encoding: "utf8" });
    ok(r.code === 0 && r.stdout.trim() === "250000", "control: a finite loop finishes under the same flag (" + r.stdout.trim() + ")");
});

// ---- E3-14: new TypedArray(array) -----------------------------------------
// ORACLE: ECMA-262 23.2.5.1.4: values = IterableToList(source) using the
// source's @@iterator, THEN each is converted and stored. So (1) the result
// equals an element-wise copy, (2) a replaced Array.prototype[@@iterator] or a
// replaced %ArrayIteratorPrototype%.next must be honoured, (3) the list is a
// snapshot: a valueOf that grows the source cannot add elements.
// SCALING: building from an array is the same work as Float64Array.from; it
// was 6.6x slower because it went through the iterator protocol per element.
// CONTROL: construction from another typed array (unchanged path).
t.test("E3-14 new TypedArray(array)", ({ ok }) => {
    const N = 400000, src = [];
    for (let i = 0; i < N; i++) src.push(i * 0.5);
    const a = new Float64Array(src);
    let same = a.length === N;
    for (let i = 0; i < N && same; i += 997) same = a[i] === src[i];
    ok(same, "new Float64Array(array) copies the elements");
    ok(Array.from(new Int8Array([1.9, -1.9, 300, "7", null, undefined])).join() === "1,-1,44,7,0,0", "conversion per element is unchanged");
    ok(Array.from(new Uint8Array(new Float64Array([1, 2, 3]))).join() === "1,2,3", "control: construction from a typed array");

    const it = Array.prototype[Symbol.iterator];
    Array.prototype[Symbol.iterator] = function* () { yield 7; yield 8; };
    let viaIter;
    try { viaIter = Array.from(new Uint8Array([1, 2, 3, 4])); } finally { Array.prototype[Symbol.iterator] = it; }
    ok(viaIter.join() === "7,8", "a replaced Array.prototype[@@iterator] is honoured (" + viaIter.join() + ")");

    const grow = [1, { valueOf() { grow.push(99); return 2; } }, 3];
    const g = new Float64Array(grow);
    ok(g.length === 3 && Array.from(g).join() === "1,2,3", "the source is snapshotted before conversion runs user code (" + Array.from(g).join() + ")");

    const tNew = best(() => new Float64Array(src)), tFrom = best(() => Float64Array.from(src));
    ok(tNew < tFrom * 3, "[red] E3-14: new Float64Array(array) costs about what Float64Array.from does (" + tNew.toFixed(2) + " ms vs " + tFrom.toFixed(2) + " ms)");
});

// ---- M1a-06 / M1a-08: DataFrame records ----------------------------------
// ORACLE: TO_RECORDS is the row view of the columns and FROM_RECORDS is its
// inverse, so a round trip returns the same numbers and strings; the column
// set is the union of the keys in first-seen order.
// SCALING: the cost per cell cannot depend on how many columns there are.
// It did: each key was compared, as a C string, against every known column.
// CONTROL: a 20-column frame.
t.test("M1a-06 M1a-08 DataFrame records", ({ ok }) => {
    const D0 = new df.DataFrame({ a: new Float64Array(1) });
    const recs = [{ x: 1, name: "a" }, { x: 2.5, name: "b", extra: 9 }, { name: "c", x: -0.25 }];
    const D = D0.FROM_RECORDS(recs);
    const back = D.TO_RECORDS();
    ok(back.length === 3 && back[0].x === 1 && back[1].x === 2.5 && back[2].x === -0.25, "FROM_RECORDS/TO_RECORDS round-trips numbers");
    ok(back.map((r) => r.name).join() === "a,b,c", "and strings (" + back.map((r) => r.name).join() + ")");
    ok(Object.keys(back[0]).join() === "x,name,extra", "columns are the union of keys in first-seen order (" + Object.keys(back[0]).join() + ")");
    ok(Number.isNaN(back[0].extra) && back[1].extra === 9, "a key missing from a row is NaN in that row");
    const csv = D.TO_CSV().split("\n");
    ok(csv[0] === "x,name,extra" && csv[2] === "2.5,b,9", "TO_CSV writes the same cells (" + csv[2] + ")");

    const cell = (rows, cols) => {
        const rs = [];
        for (let i = 0; i < rows; i++) { const o = {}; for (let c = 0; c < cols; c++) o["k" + c] = i + c; rs.push(o); }
        return best(() => D0.FROM_RECORDS(rs), 3) / (rows * cols);
    };
    cell(50, 20);
    const narrow = cell(4000, 20), wide = cell(100, 800);
    ok(wide < narrow * 4, "[red] M1a-08: FROM_RECORDS cost per cell does not grow with the column count (" + (narrow * 1e6).toFixed(0) + " ns at 20 columns, " + (wide * 1e6).toFixed(0) + " ns at 800)");
});

// ---- D3-07: AEAD objects reuse a keyed context ----------------------------
// ORACLE: McGrew & Viega, "The Galois/Counter Mode of Operation", test cases
// 13 and 14 (AES-256, zero key, zero 96-bit IV; also in NIST's GCM vectors):
// empty plaintext -> tag 530f8afbc74536b9a963b4f1c4cb738b; sixteen zero bytes
// -> cea7403d4d606b6e074ec5d3baf39d18 || d0d1c8a799996bf0265b98b5d48ab919.
// RFC 8439 2.8.2 is the ChaCha20-Poly1305 vector. A cached context must give
// these on EVERY call, including after a failed open and with a different
// nonce in between: reusing state across calls is exactly what could break.
// CONTROL: a tampered ciphertext is refused each time.
t.test("D3-07 AEAD objects reuse a keyed context", ({ ok }) => {
    const hex = (u) => Array.from(u, (b) => b.toString(16).padStart(2, "0")).join("");
    const unhex = (s) => new Uint8Array(s.match(/../g).map((h) => parseInt(h, 16)));
    const a = new AESGCM(new Uint8Array(32)), iv = new Uint8Array(12);
    let good = 0, refused = 0;
    for (let round = 0; round < 4; round++) {
        if (hex(a.seal(iv, new Uint8Array(0))) === "530f8afbc74536b9a963b4f1c4cb738b") good++;
        if (hex(a.seal(iv, new Uint8Array(16))) === "cea7403d4d606b6e074ec5d3baf39d18d0d1c8a799996bf0265b98b5d48ab919") good++;
        const ct = a.seal(new Uint8Array(12).fill(round + 1), new Uint8Array(40).fill(9), new Uint8Array([1, 2]));
        ct[3] ^= 0x10;
        try { a.open(new Uint8Array(12).fill(round + 1), ct, new Uint8Array([1, 2])); } catch (e) { if (e instanceof TypeError) refused++; }
        if (hex(a.open(iv, unhex("cea7403d4d606b6e074ec5d3baf39d18d0d1c8a799996bf0265b98b5d48ab919"))) === "00".repeat(16)) good++;
    }
    ok(good === 12, "D3-07: AES-256-GCM test cases 13 and 14 hold on every reuse of one object (" + good + "/12)");
    ok(refused === 4, "control: a tampered ciphertext is refused on every round (" + refused + "/4)");

    const key = unhex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
    const nonce = unhex("070000004041424344454647");
    const aad = unhex("50515253c0c1c2c3c4c5c6c7");
    const pt = new TextEncoder().encode("Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.");
    const c = new ChaCha20Poly1305(key);
    let cg = 0;
    for (let round = 0; round < 3; round++) {
        const out = hex(c.seal(nonce, pt, aad));
        if (out.startsWith("d31a8d34648e60db7b86afbc53ef7ec2") && out.endsWith("1ae10b594f09e26a7e902ecbd0600691")) cg++;
        c.seal(new Uint8Array(12).fill(round), pt);
    }
    ok(cg === 3, "D3-07: the RFC 8439 2.8.2 vector holds on every reuse (" + cg + "/3)");
});

t.after(() => removeAll(new Path(String(T))));
await t.run();
