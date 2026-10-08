import { MsgPackEncode, CBOREncode, CBORCanonical, ValueHash,
         structuredClone } from "dyna:serialize";

const MIN_MS = 120;
let sink = 0;

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

const use = (v) => (typeof v === "string" ? v.length : v.length);

function mkObj(n, wide) {
    const o = {};
    for (let i = 0; i < n; i++) o[(wide ? "鍵値" : "key") + i + "_x"] = i;
    return o;
}

function mkShuffled(n, wide) {
    const idx = [];
    for (let i = 0; i < n; i++) idx.push(i);
    for (let i = n - 1; i > 0; i--) {
        const j = (i * 1103515245 + 12345) % (i + 1);
        const t = idx[i]; idx[i] = idx[j]; idx[j] = t;
    }
    const o = {};
    for (const i of idx) o[(wide ? "鍵値" : "key") + i + "_x"] = i;
    return o;
}

print("=== vserialize: canonical key sort ===");
print("#V op            keys  width  ms/call");

const SIZES = [16, 64, 256, 1024];
for (const wide of [false, true]) {
    const w = wide ? "utf8 " : "ascii";
    for (const n of SIZES) {
        const o = mkShuffled(n, wide);
        const rows = [
            ["CBORCanonical", () => use(CBORCanonical(o))],
            ["ValueHash    ", () => use(ValueHash(o))],
            ["CBOREncode*  ", () => use(CBOREncode(o))],
            ["MsgPackEnc*  ", () => use(MsgPackEncode(o))],
        ];
        for (const [name, fn] of rows)
            print("#V " + name + " " + String(n).padStart(5) + "  " + w +
                  "  " + ms(fn).toFixed(4));
    }
}
print("(* = CONTROL: the unsorted path, must not move)");

print("");
print("#V op            keys  shape        ms/call");
{
    const P = "common_prefix_that_forces_a_full_compare_";
    for (const n of [64, 256]) {
        const o = {};
        for (let i = 0; i < n; i++) o[P + String(i).padStart(6, "0")] = i;
        print("#V CBORCanonical " + String(n).padStart(5) + "  shared-prefix " +
              ms(() => use(CBORCanonical(o))).toFixed(4));
    }
}

print("");
print("#V op            nodes  ms/call");
for (const n of [16, 128, 512, 2048]) {
    const root = {};
    for (let i = 0; i < n; i++) root["c" + i] = { v: i };
    print("#V clone         " + String(n).padStart(5) + "  " +
          ms(() => structuredClone(root) && 1).toFixed(4));
}

{
    const a = { name: "a" }; a.self = a;
    const c = structuredClone(a);
    if (c.self !== c) throw new Error("bench_vserialize: clone memo lost a cycle");
}

if (sink === -1) print("unreachable");
print("done");
