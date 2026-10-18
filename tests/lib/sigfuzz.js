// tests/lib/sigfuzz.js -- signature-driven input generation for one module.
//
// The contract (dynajs.d.ts) says what each parameter is. This reads it at run
// time, builds a VALID baseline call for every function, constructor and
// method, and then varies one parameter at a time through the edge values of
// its declared type, through values of the wrong type, and through hostile
// objects whose valueOf/toString/getters run code mid-call (closing the very
// receiver being called). Valid baselines matter: random junk is refused at
// the argument boundary and never reaches the code behind it.
//
// What every call must do, whatever the input:
//   - return, or throw an Error (a non-Error throw, or an Error without a
//     string message, is a defect);
//   - finish inside the budget (a slow or unbounded call is a defect);
//   - leave Object/Array/Function prototypes untouched;
//   - leave its receiver usable or cleanly refusing (a later call on it must
//     not crash -- under ASan this is where a use-after-free shows).
// Promises returned are awaited (bounded) and held to the same rules.
//
// Coverage is reported, not just failures: how many calls reached the code
// (did not throw at the boundary) per name, and which names never did -- a
// name every call is refused at is a name this generator does not test.
import * as std from "std";
import * as os from "os";
import { parseDts } from "./dtsparse.js";
import { STRINGS } from "./fuzzgen.js";
import { BASELINES, SKIP, HUGE_OK, SLOW_OK, FIELD_MAX, PARAMS, INSTANCE, FIXED, THIS } from "./sigfuzz_baselines.js";

const BUDGET_MS = +(std.getenv("DYNAJS_BUDGET_MS") || 2000);
const TRACE = std.getenv("DYNA_SIGFUZZ_TRACE") === "1";
const ONLY = std.getenv("DYNA_SIGFUZZ_ONLY") || "";

let dts = null;
function contract() {
    if (dts) return dts;
    const text = std.loadFile("dynajs.d.ts");
    if (text === null) throw new Error("sigfuzz: run from the repo root (dynajs.d.ts not found)");
    dts = parseDts(text);
    return dts;
}

// ---- the scratch directory every path-shaped value points into -------------
const SCRATCH = (() => {
    const base = (std.getenv("TMPDIR") || "/tmp").replace(/\/+$/, "");
    const dir = `${base}/dyna-sigfuzz-${os.getpid ? os.getpid() : "x"}`;
    os.mkdir(dir, 0o700);
    const f = std.open(dir + "/seed.txt", "w");
    if (f) { f.puts("hello, world\n"); f.close(); }
    return dir;
})();
function removeTree(path) {
    const [st] = os.lstat(path);
    if (st && (st.mode & os.S_IFMT) === os.S_IFDIR) {
        const [names] = os.readdir(path);
        if (names) for (const n of names) if (n !== "." && n !== "..") removeTree(`${path}/${n}`);
    }
    os.remove(path);
}
export function scratchCleanup() { removeTree(SCRATCH); }

// ---- type text -> kinds ------------------------------------------------------
function splitTop(s, sep) {
    const out = []; let depth = 0, cur = "";
    for (let i = 0; i < s.length; i++) {
        const c = s[i];
        if (c === '"' || c === "'") { let j = i + 1; while (j < s.length && s[j] !== c) j += s[j] === "\\" ? 2 : 1; cur += s.slice(i, j + 1); i = j; continue; }
        if (c === "(" || c === "{" || c === "[" || c === "<") depth++;
        else if (c === ")" || c === "}" || c === "]" || (c === ">" && s[i - 1] !== "=")) depth--;
        if (c === sep && depth === 0) { out.push(cur); cur = ""; continue; }
        cur += c;
    }
    if (cur.trim()) out.push(cur);
    return out.map((x) => x.trim()).filter(Boolean);
}
const BYTES_RE = /^(Uint8Array|Int8Array|Uint8ClampedArray|ArrayBuffer|ArrayBufferView|ByteView|BytesInput|BufferSource|DataView)$/;
const TYPED_RE = /^(Uint16Array|Int16Array|Uint32Array|Int32Array|Float32Array|Float64Array|BigInt64Array|BigUint64Array)$/;
export function kindsOf(type, depth = 0) {
    const d = contract();
    type = type.trim().replace(/^\(([\s\S]*)\)$/, "$1").trim();
    if (depth > 6) return [{ k: "any" }];
    const alts = splitTop(type, "|");
    if (alts.length > 1) return alts.flatMap((a) => kindsOf(a, depth + 1));
    const t = alts[0] || "any";
    if (/^"[^"]*"$|^'[^']*'$/.test(t)) return [{ k: "lit", v: t.slice(1, -1) }];
    if (/^-?\d+(\.\d+)?$/.test(t)) return [{ k: "litn", v: +t }];
    if (t === "number") return [{ k: "number" }];
    if (t === "bigint") return [{ k: "bigint" }];
    if (t === "string") return [{ k: "string" }];
    if (t === "boolean" || t === "true" || t === "false") return [{ k: "boolean" }];
    if (t === "null" || t === "undefined" || t === "void") return [{ k: "nullish" }];
    if (BYTES_RE.test(t)) return [{ k: "bytes" }];
    if (TYPED_RE.test(t)) return [{ k: "typed", ctor: t }];
    if (/\[\]$/.test(t)) return [{ k: "array", of: t.slice(0, -2) }];
    if (/^(Readonly)?Array<([\s\S]*)>$/.test(t)) return [{ k: "array", of: /^(?:Readonly)?Array<([\s\S]*)>$/.exec(t)[1] }];
    if (/^\[[\s\S]*\]$/.test(t)) return [{ k: "tuple", of: splitTop(t.slice(1, -1), ",") }];
    if (/=>/.test(t) && /^\(/.test(t)) return [{ k: "fn" }];
    if (/^\{[\s\S]*\}$/.test(t)) return [{ k: "obj", fields: fieldsOf(t.slice(1, -1)) }];
    if (/^Record</.test(t) || t === "object" || t === "Object") return [{ k: "obj", fields: [] }];
    if (/^Promise</.test(t) || /^Iterable|^AsyncIterable|^Iterator/.test(t)) return [{ k: "any" }];
    if (d.aliases[t]) return kindsOf(d.aliases[t], depth + 1);
    if (d.interfaces[t]) return [{ k: "obj", fields: d.interfaces[t].filter((m) => m.kind === "prop").map((m) => ({ name: m.name.replace(/\?$/, ""), type: m.type, optional: /\?$/.test(m.name) || !!m.optional })) }];
    return [{ k: "class", name: t.replace(/<[\s\S]*>$/, "") }];
}
function fieldsOf(body) {
    return splitTop(body.replace(/\n/g, " "), ";").flatMap((f) => splitTop(f, ","))
        .map((f) => /^([A-Za-z_$][\w$]*|"[^"]*")(\??)\s*:\s*([\s\S]+)$/.exec(f.trim()))
        .filter(Boolean).map((m) => ({ name: m[1].replace(/"/g, ""), type: m[3], optional: m[2] === "?" }));
}

// ---- values ------------------------------------------------------------------
const NUMBERS = [0, -0, 1, -1, 0.5, 3.7, NaN, Infinity, -Infinity, 2 ** 31 - 1, 2 ** 31, -(2 ** 31) - 1,
    2 ** 32, 2 ** 53, -(2 ** 53), 1e308, 5e-324, 1e9, 255, 256, 65536];
const BIGINTS = [0n, -1n, 1n, 2n ** 63n - 1n, 2n ** 64n, -(2n ** 64n)];
const lone = /[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(?:[^\uD800-\uDBFF]|^)[\uDC00-\uDFFF]/;
const STRS = ["", "\0", "a\0b", "\uD800", "\uDFFFa", "é", "\u{1F600}\u{1F600}", " ", "\n", "%s%n%x",
    "../../etc/passwd", "__proto__", "constructor", "1e309", "-0", "0x10", "‮hidden",
    "x".repeat(70000), ...STRINGS.filter((s, i) => i % 37 === 0).slice(0, 12)];
// immutable buffers: a byte that changes, or a buffer that detaches, after a call is a native writing through it
const IMMUTABLE = (() => {
    const out = [];
    try {
        const mk = (n, wrap) => {
            const src = new Uint8Array(n);
            for (let i = 0; i < n; i++) src[i] = (i * 37 + 11) & 255;
            const ab = src.buffer.transferToImmutable();
            return { ab, view: wrap(ab), sum: immSum(ab) };
        };
        out.push(mk(64, (ab) => new Uint8Array(ab)), mk(64, (ab) => ab), mk(64, (ab) => new Uint8Array(ab, 8, 32)),
            mk(64, (ab) => new Float64Array(ab)), mk(64, (ab) => new Float32Array(ab)), mk(64, (ab) => new Int32Array(ab)),
            mk(64, (ab) => new DataView(ab)), mk(4096, (ab) => new Uint8Array(ab)));
    } catch (e) {}
    return out;
})();
function immSum(ab) {
    if (ab.detached) return -1;
    const u = new Uint8Array(ab);
    let h = 2166136261;
    for (let i = 0; i < u.length; i++) h = Math.imul(h ^ u[i], 16777619);
    return h >>> 0;
}
export function immutableCount() { return IMMUTABLE.length; }

function bytesEdges() {
    const all = new Uint8Array(256);
    for (let i = 0; i < 256; i++) all[i] = i;
    const big = new Uint8Array(1 << 20);
    for (let i = 0; i < big.length; i++) big[i] = (i * 2654435761) >>> 24;
    const out = [new Uint8Array(0), new Uint8Array([0]), new Uint8Array([255]), all, all.subarray(3, 200),
        new Uint8Array(all.buffer, 1, 7), new Int8Array([-1, -128, 127]), big, all.buffer];
    try {
        const ab = new ArrayBuffer(16);
        const v = new Uint8Array(ab);
        if (typeof ab.transfer === "function") { ab.transfer(); out.push(v); }
    } catch (e) {}
    try { if (typeof SharedArrayBuffer === "function") out.push(new Uint8Array(new SharedArrayBuffer(8))); } catch (e) {}
    for (const im of IMMUTABLE) out.push(im.view);
    return out;
}
const BYTES = bytesEdges();
function deepArray(n) { let a = [1]; for (let i = 0; i < n; i++) a = [a]; return a; }
function deepObj(n) { let o = { v: 1 }; for (let i = 0; i < n; i++) o = { v: o }; return o; }
const throwingProxy = () => new Proxy({}, { get() { throw new Error("proxy get trap"); }, ownKeys() { throw new Error("proxy ownKeys trap"); }, has() { throw new Error("proxy has trap"); } });
function getterThrows() { const o = {}; Object.defineProperty(o, "x", { enumerable: true, get() { throw new Error("getter"); } }); return o; }
const WRONG = () => [undefined, null, true, 0, -1, NaN, "", "abc", 1n, Symbol("s"), {}, [], () => 1,
    new Uint8Array(2), new Date(0), /re/g, new Map(), throwingProxy(), getterThrows(), Object.create(null)];

export function baseline(kinds, pname, ctx, depth = 0) {
    const k = kinds.find((x) => x.k !== "nullish") || kinds[0];
    if (depth > 3) return undefined;
    switch (k.k) {
        case "lit": return k.v;
        case "litn": return k.v;
        case "number": return /^(n|k|count|size|len|length|bins|buckets|depth|width|height|cols|rows|w|periods|capacity|cap|limit|max|min|times|retries|digits|dp|precision|level|bits|index|i|j|pos|offset|start|end|from|to|seed|order|degree|nu|m)$/i.test(pname) ? 3 : 1.5;
        case "bigint": return 3n;
        case "string": return /path|file|dir|name/i.test(pname) ? SCRATCH + "/seed.txt" : "abc";
        case "boolean": return true;
        case "nullish": return undefined;
        case "bytes": return new Uint8Array([104, 105, 33, 10]);
        case "typed": return new globalThis[k.ctor](4).fill(1);
        case "array": {
            const ek = kindsOf(k.of);
            const e = baseline(ek, pname, ctx, depth + 1);
            return [e, e, e];
        }
        case "tuple": return k.of.map((t) => baseline(kindsOf(t), pname, ctx, depth + 1));
        case "fn": return function cb() { return undefined; };
        // an options bag gets its REQUIRED fields, each from its own baseline
        case "obj": {
            const o = {};
            for (const f of k.fields || []) {
                if (f.optional) continue;
                const pv = depth < 3 && ctx && ctx.hint ? ctx.hint(f.name, kindsOf(f.type)) : undefined;
                o[f.name] = pv !== undefined ? pv : baseline(kindsOf(f.type), f.name, ctx, depth + 1);
            }
            return o;
        }
        case "class": return ctx.instanceOf(k.name);
        default: return 1;
    }
}
export function edges(kinds, pname, ctx, depth = 0) {
    const out = [];
    if (depth > 2) return out;
    for (const k of kinds) {
        switch (k.k) {
            case "lit": out.push(k.v, k.v.toUpperCase(), k.v + "\0", "not-a-mode"); break;
            case "number": case "litn": out.push(...NUMBERS); break;
            case "bigint": out.push(...BIGINTS); break;
            case "string": out.push(...STRS); break;
            case "bytes": case "typed": out.push(...BYTES); break;
            case "array": {
                const ek = kindsOf(k.of);
                const e = baseline(ek, pname, ctx);
                const sparse = []; sparse[5] = e;
                const huge = []; huge.length = 2 ** 32 - 1;
                out.push([], [e], sparse, huge, deepArray(5000), [e, undefined, null, e],
                    edges(ek, pname, ctx, depth + 1).slice(0, 24), Object.assign([e, e], { length: 1 }));
                break;
            }
            case "obj": {
                out.push(null, Object.create(null), throwingProxy(), getterThrows(), deepObj(5000), Object.freeze({}));
                for (const f of k.fields || []) {
                    const fk = kindsOf(f.type);
                    for (const v of edges(fk, f.name, ctx, depth + 1).slice(0, 14)) {
                        if (FIELD_MAX[f.name] !== undefined && typeof v === "number" && v > FIELD_MAX[f.name]) continue;
                        out.push({ [f.name]: v });
                    }
                }
                break;
            }
            case "fn": out.push(function thrower() { throw new Error("callback threw"); },
                () => { const p = Promise.reject(new Error("rejected")); p.catch(() => {}); return p; },
                () => ({ then() { throw new Error("evil thenable"); } }), 42); break;
            default: break;
        }
    }
    return out;
}
// a value that runs code when coerced: closes/clears the receiver mid-call
export function hostiles(base, receiver) {
    const disarm = () => {
        if (!receiver) return;
        for (const m of ["close", "destroy", "clear", "free", "stop", "dispose", "reset", "abort"])
            if (typeof receiver[m] === "function") { try { receiver[m](); } catch (e) {} }
    };
    return [
        { valueOf() { disarm(); return typeof base === "number" ? base : 1; }, toString() { disarm(); return typeof base === "string" ? base : "1"; } },
        { [Symbol.toPrimitive]() { disarm(); throw new Error("toPrimitive threw"); } },
        new Proxy([1, 2, 3], { get(t, p) { if (p === "length") disarm(); return Reflect.get(t, p); } }),
    ];
}

// ---- execution ------------------------------------------------------------
const PROTO_SNAP = () => [Object.prototype, Array.prototype, Function.prototype, String.prototype, Number.prototype]
    .map((p) => Object.getOwnPropertyNames(p).sort().join(",")).join("|");

function within(p, ms) {
    let timer;
    return Promise.race([p.then((v) => ({ v }), (e) => ({ e })), new Promise((r) => { timer = setTimeout(() => r({ timeout: true }), ms); })])
        .finally(() => clearTimeout(timer));
}

// Every hostile string can land in a path parameter, so the run's working
// directory is the scratch directory: a relative path cannot reach the tree.
export async function fuzzModule(modName, opts = {}) {
    contract();
    const [cwd0, err] = os.getcwd();
    if (err || os.chdir(SCRATCH) !== 0) throw new Error("sigfuzz: cannot enter the scratch directory " + SCRATCH);
    try { return await fuzzModuleIn(modName, opts); }
    finally { os.chdir(cwd0); }
}

async function fuzzModuleIn(modName, opts) {
    const d = contract();
    const decl = d.modules[modName];
    if (!decl) throw new Error("sigfuzz: no declare module \"dyna:" + modName + "\" in dynajs.d.ts");
    let ns;
    try { ns = await import("dyna:" + modName); }
    catch (e) { print(`sigfuzz dyna:${modName}: SKIP (not in this build)`); return { skipped: true }; }
    const snap0 = PROTO_SNAP();
    const fails = [], reachedNone = [], noInstance = [];
    let calls = 0, reached = 0, names = 0;
    const instances = new Map();
    let Path = null;
    try { Path = (await import("dyna:file")).Path; } catch (e) {}
    const ctx = {
        ns,
        Path,
        scratch: SCRATCH,
        instanceOf(name) {
            if (instances.has(name)) return instances.get(name);
            const C = ns[name] || globalThis[name];
            let inst = null;
            const b = BASELINES[modName + "." + name + "#instance"];
            if (b) { try { inst = b(ctx, SCRATCH, ns); } catch (e) {} }
            if (inst && typeof inst.then === "function") inst = null;
            if (!inst && typeof C === "function") {
                const cls = decl.classes[name];
                const ctor = cls && cls.members.find((m) => m.kind === "method" && m.name === "constructor");
                const given = BASELINES[modName + "." + name + "#new"];
                const args = given ? given(ctx, SCRATCH, ns)
                    : ctor ? ctor.params.filter((p) => !p.optional && !p.rest).map((p) => ctx.param(p.name, kindsOf(p.type))) : [];
                try { inst = new C(...args); } catch (e) { try { inst = new C(); } catch (e2) {} }
            }
            instances.set(name, inst);
            return inst;
        },
        // a new instance that does not replace the shared one
        async fresh(name) {
            const shared = instances.get(name);
            instances.delete(name);
            const r = await ctx.prepare(name);
            if (shared) instances.set(name, shared); else instances.delete(name);
            return r;
        },
        // a factory may be async (a model has to be fitted before it predicts)
        async prepare(name) {
            const b = BASELINES[modName + "." + name + "#instance"]
                || (INSTANCE[modName] && ((c, sc, n) => INSTANCE[modName](name, c, n)));
            if (!b) return ctx.instanceOf(name);
            let inst = null;
            try { inst = await b(ctx, SCRATCH, ns); } catch (e) { inst = null; }
            if (inst === undefined) { instances.delete(name); return ctx.instanceOf(name); }
            instances.set(name, inst);
            return inst;
        },
        // the module's own hint for a parameter name, without the type fallback
        hint(pname, kinds) {
            const h = PARAMS[modName];
            return h ? h(pname, kinds, ctx) : undefined;
        },
        param(pname, kinds) {
            const h = PARAMS[modName];
            const v = h ? h(pname, kinds, ctx) : undefined;
            return v !== undefined ? v : baseline(kinds, pname, ctx);
        },
    };
    const record = (label, why) => { fails.push(label + ": " + why); print("FAIL: " + label + ": " + why); };
    const show = (v, depth = 0) => {
        try {
            if (typeof v === "string") return JSON.stringify(v.length > 40 ? v.slice(0, 40) + "..." : v);
            if (typeof v === "bigint") return v + "n";
            if (typeof v === "symbol") return "Symbol()";
            if (typeof v === "function") return "fn " + (v.name || "anon");
            if (ArrayBuffer.isView(v)) return v.constructor.name + "(" + v.length + ")";
            if (Array.isArray(v)) return "Array(" + v.length + ")";
            if (v && typeof v === "object") return "{" + Object.keys(v).slice(0, 3).map((k) => k + ": " + (depth ? "..." : show(v[k], 1))).join(", ") + "}";
            return String(v);
        } catch (e) { return "<unprintable>"; }
    };

    const immutableCheck = (label, args) => {
        for (const im of IMMUTABLE) {
            const s = immSum(im.ab);
            if (s === im.sum) continue;
            record(label, (s === -1 ? "detached" : "wrote into") + ` an immutable ArrayBuffer on (${args.map(show).join(", ")})`);
            im.sum = s;
        }
    };
    async function call(label, fn, thisArg, args, isNew, after) {
        calls++;
        if (TRACE) { std.err.puts("  call " + label + " (" + args.map(show).join(", ") + ")\n"); std.err.flush(); }
        const t0 = os.now();
        let got, threw = null;
        try { got = isNew ? new fn(...args) : fn.apply(thisArg, args); }
        catch (e) { threw = e; }
        const dt = os.now() - t0;
        const slow = SLOW_OK[modName + "." + label.split(" ")[0]];
        if (dt > (slow ? (slow.ms * BUDGET_MS) / 2000 : BUDGET_MS)) record(label, `took ${dt.toFixed(0)} ms on (${args.map(show).join(", ")})`);
        // a result may be a hostile value the caller stored earlier; probing it must not throw
        const has = (v, m) => { try { return v !== null && (typeof v === "object" || typeof v === "function") && typeof v[m] === "function"; } catch (e) { return false; } };
        if (!threw && has(got, "then")) {
            const r = await within(Promise.resolve(got), BUDGET_MS * 2);
            if (r.timeout) { record(label, `promise did not settle in ${BUDGET_MS * 2} ms on (${args.map(show).join(", ")})`); return false; }
            if ("e" in r) { threw = r.e; got = undefined; } else got = r.v;
        }
        if (threw !== null) {
            if (TRACE) { std.err.puts("    -> " + (threw && threw.message !== undefined ? threw.constructor.name + ": " + threw.message : show(threw)) + "\n"); std.err.flush(); }
            if (!(threw instanceof Error)) record(label, `threw a non-Error (${show(threw)}) on (${args.map(show).join(", ")})`);
            else if (typeof threw.message !== "string") record(label, `threw an Error without a string message on (${args.map(show).join(", ")})`);
            if (after) after();
            immutableCheck(label, args);
            return false;
        }
        if (got !== thisArg && has(got, "close") && !opts.keepResults) {
            try { got.close(); } catch (e) {}
        }
        if (after) after();
        immutableCheck(label, args);
        return true;
    }

    async function fuzzCallable(label, fn, thisArg, params, isNew, receiverFor) {
        if (SKIP[modName + "." + label] || SKIP[label.split(/[#.]/)[0]]) return;
        if (ONLY && label !== ONLY) return;
        // "_name" members are engine-internal (@internal in the d.ts), not API
        if (/(^|[#.])_/.test(label)) return;
        names++;
        const fixed = BASELINES[modName + "." + label]
            || (FIXED[modName] && ((c, sc, n) => FIXED[modName](label, c, sc, n)));
        // a rest parameter is absent in the baseline and varied one element at a time
        const kinds = params.map((p) => {
            const k = kindsOf(p.type);
            return p.rest && k.length === 1 && k[0].k === "array" ? kindsOf(k[0].of) : k;
        });
        const absent = (p) => p.optional || p.rest;
        let base = fixed ? await fixed(ctx, SCRATCH, ns) : params.map((p, i) => (absent(p) ? undefined : ctx.param(p.name, kinds[i])));
        if (!Array.isArray(base)) base = params.map((p, i) => (absent(p) ? undefined : ctx.param(p.name, kinds[i])));
        const trim = (args, keep) => { let n = args.length; while (n > keep && args[n - 1] === undefined) n--; return args.slice(0, n); };
        base = trim(base, 0);
        let hits = 0;
        const recv = async () => (receiverFor ? await receiverFor() : thisArg);
        if (await call(label + " (baseline)", fn, await recv(), base, isNew)) hits++;
        // the contract: a name whose every parameter is optional accepts a call with none
        if (!fixed && params.length && params.every((p) => p.optional || p.rest)) {
            let msg = null;
            try {
                const r = isNew ? new fn() : fn.call(await recv());
                if (r && typeof r.then === "function") await within(Promise.resolve(r).catch((e) => { msg = e; }), BUDGET_MS);
            } catch (e) { msg = e; }
            if (msg instanceof TypeError) record(label, `every parameter is declared optional, but a call with none is refused: ${msg.message}`);
        }
        for (let i = 0; i < params.length; i++) {
            let values = [...edges(kinds[i], params[i].name, ctx), ...WRONG()];
            if (HUGE_OK[modName + "." + label]) values = values.filter((v) => !(Array.isArray(v) && v.length >= 2 ** 31));
            for (const v of values) {
                const args = base.slice();
                args[i] = v;
                if (await call(`${label} arg ${i} (${params[i].name})`, fn, await recv(), args, isNew)) hits++;
            }
            if (!isNew) {
                for (let h = 0; h < 3; h++) {
                    const r = receiverFor ? await receiverFor(true) : thisArg;
                    const args = base.slice();
                    args[i] = hostiles(base[i], receiverFor ? r : null)[h];
                    await call(`${label} arg ${i} (${params[i].name}) hostile#${h}`, fn, r, args, false, () => {
                        if (r && typeof r === "object" && r !== ns)
                            for (const m of ["toString", "valueOf"]) {
                                try { r[m](); } catch (e) { if (!(e instanceof Error)) record(label, "receiver threw a non-Error after a hostile argument"); }
                            }
                    });
                }
            }
        }
        reached += hits;
        if (hits === 0) reachedNone.push(label);
        if (PROTO_SNAP() !== snap0) { record(label, "changed a built-in prototype"); }
    }

    const fns = Object.entries(decl.functions);
    for (const [name, overloads] of fns) {
        const fn = ns[name];
        if (typeof fn !== "function") continue;
        const sig = overloads.reduce((a, b) => (b.params.length > a.params.length ? b : a));
        await fuzzCallable(name, fn, ns, sig.params, false);
    }
    for (const [oname, members] of Object.entries(decl.objects)) {
        const obj = ns[oname];
        if (!obj) continue;
        for (const m of members) {
            if (m.kind !== "method" || typeof obj[m.name] !== "function") continue;
            await fuzzCallable(oname + "." + m.name, obj[m.name], obj, m.params, false);
        }
    }
    const classLike = Object.entries(decl.classes);
    for (const iname of Object.keys(d.interfaces)) if (typeof ns[iname] === "function" && !decl.classes[iname]) classLike.push([iname, { members: d.interfaces[iname] }]);
    for (const [cname, cls] of classLike) {
        const C = ns[cname];
        if (typeof C !== "function") continue;
        const ctor = cls.members.find((m) => m.kind === "method" && m.name === "constructor");
        if (ctor) await fuzzCallable(cname + "#new", C, undefined, ctor.params, true);
        // a throwaway receiver: a destructive call must not consume the shared one
        const fresh = async () => {
            const shared = instances.get(cname);
            instances.delete(cname);
            const r = await ctx.prepare(cname);
            if (shared) instances.set(cname, shared); else instances.delete(cname);
            return r;
        };
        await ctx.prepare(cname);
        const byName = new Map();
        for (const m of cls.members) if (m.kind === "method" && m.name !== "constructor") {
            const prev = byName.get(m.name + (m.isStatic ? "#static" : ""));
            if (!prev || m.params.length > prev.params.length) byName.set(m.name + (m.isStatic ? "#static" : ""), m);
        }
        for (const m of byName.values()) {
            if (m.isStatic) {
                if (typeof C[m.name] === "function") await fuzzCallable(cname + "." + m.name, C[m.name], C, m.params, false);
                continue;
            }
            if (ONLY && cname + "#" + m.name !== ONLY) continue;
            const inst = instances.get(cname) || (await ctx.prepare(cname));
            if (!inst) { if (!noInstance.includes(cname) && !SKIP[modName + "." + cname] && !SKIP[cname]) noInstance.push(cname); continue; }
            if (typeof inst[m.name] !== "function") continue;
            const fn = inst[m.name];
            // a method whose receiver must be in a particular state names it: "mod.Class#method@this"
            const own = BASELINES[modName + "." + cname + "#" + m.name + "@this"]
                || (THIS[modName] && THIS[modName](cname, m.name) && ((c, sc, n) => THIS[modName](cname, m.name)(c, sc, n)));
            if (own) {
                const make = async () => { try { return await own(ctx, SCRATCH, ns); } catch (e) { return null; } };
                const r0 = await make();
                if (!r0) { record(cname + "#" + m.name, "its @this baseline threw"); continue; }
                await fuzzCallable(cname + "#" + m.name, r0[m.name], r0, m.params, false, async () => make());
                continue;
            }
            const disposable = /^(close|destroy|free|dispose|stop|abort|clear|reset|delete|remove|pop|shift|drain|end|finish|kill|cancel|move|rename|truncate|unlink|chmod|chown)/i.test(m.name);
            await fuzzCallable(cname + "#" + m.name, fn, inst, m.params, false,
                async (forHostile) => (disposable || forHostile ? fresh() : instances.get(cname) || ctx.prepare(cname)));
        }
    }
    for (const inst of instances.values()) if (inst && typeof inst.close === "function") { try { inst.close(); } catch (e) {} }
    const pct = calls ? Math.round((100 * reached) / calls) : 0;
    print(`sigfuzz dyna:${modName}: ${names} names, ${calls} calls, ${reached} reached the code (${pct}%), ${fails.length} failures`);
    if (reachedNone.length) print(`  never reached (every call refused at the boundary): ${reachedNone.join(", ")}`);
    if (noInstance.length) print(`  no instance could be built, so no method ran: ${noInstance.join(", ")}`);
    return { names, calls, reached, fails, reachedNone, noInstance };
}
