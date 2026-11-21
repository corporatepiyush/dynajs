// flags: --std
// covers: contract dyna:ai.* dyna:asn1.* dyna:async.* dyna:bench.* dyna:bytes.* dyna:cli.* dyna:compress.* dyna:config.* dyna:crypto.* dyna:csv.* dyna:dataframe.* dyna:decimal.* dyna:dkim.* dyna:dns.* dyna:docdb.* dyna:encoding.* dyna:file.* dyna:graph.* dyna:hash.* dyna:html.* dyna:http.* dyna:json.* dyna:log.* dyna:matcher.* dyna:metrics.* dyna:mathx.* dyna:ml.* dyna:net.* dyna:netip.* dyna:oauth2.* dyna:otp.* dyna:postgres.* dyna:protobuf.* dyna:proxy.* dyna:random.* dyna:redis.* dyna:schema.* dyna:scrape.* dyna:semver.* dyna:serialize.* dyna:simd.* dyna:sftp.* dyna:smtp.* dyna:sqlite.* dyna:ssh.* dyna:stream.* dyna:structures.* dyna:svg.* dyna:sys.* dyna:time.* dyna:url.* dyna:uuid.* dyna:validate.* dyna:vec.* dyna:webrtc.* dyna:wireguard.* dyna:xml.* dyna:yaml.* -- the two house rules from README "Traps", enumerated from dynajs.d.ts
// RUNTIME-TRAPS-DOC (C3). Two rules every module promises (d.ts header):
//  1. an exported PascalCase FUNCTION is a plain function: `new X()` is TypeError "X is not a constructor";
//  2. an option bag refuses an unknown key with a TypeError that names it.
// The suite parses dynajs.d.ts (tests/lib/dtsparse.js), so a new export is checked without editing this file. Rule 2 needs
// a call that works: each candidate is first called with type-derived minimal arguments and an empty bag; only when that
// baseline succeeds is it called again with { __bogus: 1 }. Calls whose minimal arguments are not cheap or not derivable
// are counted as "not callable", never as a pass. Modules with process-wide side effects are skipped for rule 2 (reason in
// SKIP_MODULES). Every known violation is in ALLOW with a reason; an ALLOW entry that no longer fires fails the suite so the
// list only shrinks. C3 found 30 natives accepting unknown keys; C3b made them strict (dyn_opts_strict), so ALLOW now holds
// only the by-design exceptions. C5: dyna:net stays skipped (sockets), but its pure spec factories (the frame decoders build
// a config object and touch no socket) are in PURE_IN_SKIPPED so their strict bags are checked here too. A second bag named
// `meta` (the dyna-specific extension beyond a reference spec, e.g. the frame decoders' stopOnError/holdOverlapping) is
// probed the same way once the first bag passed: { __bogus: 1 } in the meta position must be a TypeError naming it.
import * as std from "std";
import { suite } from "../lib/kit.js";
import { parseDts } from "../lib/dtsparse.js";
import { Path, makeTempDir, removeAll, writeFile as fileWrite } from "dyna:file";

const t = suite("test_contract_traps");
const D = parseDts(std.loadFile(scriptArgs[0].replace(/tests\/api\/test_contract_traps\.js$/, "") + "dynajs.d.ts") || std.loadFile("dynajs.d.ts"));

const OPTIONAL_BUILDS = new Set(["sdl", "raylib", "ui", "uring"]);
const COVERED = new Set(["ai", "asn1", "async", "bench", "bytes", "cli", "compress", "config", "crypto", "csv", "dataframe", "decimal", "dkim", "dns", "docdb", "encoding", "file", "graph", "hash", "html", "http", "json", "log", "matcher", "mathx", "metrics", "ml", "net", "netip", "oauth2", "otp", "postgres", "protobuf", "proxy", "random", "redis", "schema", "scrape", "semver", "serialize", "simd", "sftp", "smtp", "sqlite", "ssh", "stream", "structures", "svg", "sys", "time", "url", "uuid", "validate", "vec", "webrtc", "wireguard", "xml", "yaml"]);
const SKIP_MODULES = {
    net: "opens sockets and servers",
    http: "opens sockets, servers and outbound requests",
    sys: "exit, exec, spawn and signals act on the process",
    webrtc: "starts ICE/DTLS machinery and network sockets",
    wireguard: "binds UDP sockets and holds the reactor until close()",
    docdb: "opens and writes database files",
    async: "offload() queues work on the shared pool",
    bench: "runs timed benchmark loops",
    ai: "may start model downloads or network calls",
    stream: "pipes need live sources and sinks to finish",
    uring: "optional io_uring build",
    sdl: "optional SDL build",
    raylib: "optional raylib build",
    ui: "optional UI build",
};
const PURE_IN_SKIPPED = new Set(["net.LengthFieldDecoder", "net.DelimiterDecoder"]);
const SKIP_FUNCTIONS = {
    "cli.prompt": "reads the terminal",
    "cli.confirm": "reads the terminal",
    "cli.select": "reads the terminal",
};

const ALLOW = {
    "opts oauth2.parseBearerFromRequest": "not an option bag: req is a request-shaped object whose other fields are legitimately ignored",
    "opts serialize.structuredClone": "spec: HTML structuredClone options is a WebIDL dictionary (StructuredSerializeOptions), which ignores unknown members",
};

function isOptionParam(p) {
    const ty = (p.type || "").trim();
    if (/\]\s*$/.test(ty)) return false;
    return /^(opts|options|opt|config|cfg)$/i.test(p.name) || /^\{/.test(ty) || /\b[A-Z]\w*Opt(ion)?s\b/.test(ty);
}

let tmpRoot, tmpN = 0;
const NOT = Symbol("not-derivable");

function valueFor(type, depth) {
    let ty = (type || "").trim();
    if (depth > 3) return NOT;
    if (D.aliases[ty] && depth < 3) return valueFor(D.aliases[ty], depth + 1);
    if (/=>/.test(ty) && !/^\(?\s*\{/.test(ty)) return () => {};
    const parts = splitUnion(ty);
    if (parts.length > 1) {
        for (const p of parts) { const v = valueFor(p, depth + 1); if (v !== NOT) return v; }
        return NOT;
    }
    const lit = /^"([^"]*)"$/.exec(ty);
    if (lit) return lit[1];
    if (/^-?\d+(\.\d+)?$/.test(ty)) return Number(ty);
    if (ty === "string") return "a";
    if (ty === "number") return 1;
    if (ty === "bigint") return 1n;
    if (ty === "boolean" || ty === "true" || ty === "false") return ty === "true";
    if (ty === "null") return null;
    if (ty === "undefined" || ty === "void") return undefined;
    if (ty === "Path") return new Path(tmpRoot.toString() + "/p" + (++tmpN));
    if (/^(ByteView|BytesInput|Uint8Array|ArrayBufferView|BufferSource)$/.test(ty)) return new Uint8Array([97, 98, 99, 100]);
    if (ty === "ArrayBuffer") return new ArrayBuffer(8);
    if (/^(Float64Array|F64Like|NumericArray)$/.test(ty)) return new Float64Array([1, 2, 3, 4]);
    if (/^(Float32Array|F32Like)$/.test(ty)) return new Float32Array([1, 2, 3, 4]);
    if (ty === "Int32Array") return new Int32Array([1, 2, 3, 4]);
    if (/^(ArrayLike<ArrayLike<number>>|number\[\]\[\])$/.test(ty)) return [[1, 2], [3, 4]];
    if (ty === "ArrayLike<number>" || ty === "number[]" || ty === "readonly number[]") return [1, 2, 3, 4, 5, 6, 7, 8];
    if (ty === "string[]") return ["a"];
    if (ty === "GraphCSR") return { offsets: Int32Array.of(0, 1, 2, 2), edges: Int32Array.of(1, 2) };
    if (/\[\]$/.test(ty) || /^(readonly\s+)?Array</.test(ty)) return [];
    if (/^Record</.test(ty) || /^\{\s*\[/.test(ty)) return {};
    if (/^\{/.test(ty)) return {};
    if (ty === "object" || ty === "unknown" || ty === "any") return {};
    return NOT;
}

function splitUnion(ty) {
    const out = [];
    let depth = 0, cur = "", q = null;
    for (let i = 0; i < ty.length; i++) {
        const c = ty[i];
        if (q) { cur += c; if (c === q && ty[i - 1] !== "\\") q = null; continue; }
        if (c === '"' || c === "'") { q = c; cur += c; continue; }
        if ("({[<".includes(c)) depth++;
        else if (")}]>".includes(c) && !(c === ">" && ty[i - 1] === "=")) depth--;
        if (c === "|" && depth === 0) { if (cur.trim()) out.push(cur.trim()); cur = ""; continue; }
        cur += c;
    }
    if (cur.trim()) out.push(cur.trim());
    return out;
}

async function settle(v) {
    if (!v || typeof v.then !== "function") return v;
    let timer;
    try {
        return await Promise.race([v, new Promise((_, rej) => { timer = setTimeout(() => rej(new Error("__timeout")), 3000); })]);
    } finally {
        clearTimeout(timer);
    }
}

function release(v) {
    try { if (v && typeof v === "object") { if (typeof v.close === "function") v.close(); else if (typeof v.dispose === "function") v.dispose(); } } catch (e) {}
}

const B = (s) => new TextEncoder().encode(s);
const FIX = {
    "net.DelimiterDecoder": () => ({ args: [], bag: { delimiter: "\n" } }),
    "compress.gunzip": (ns) => ({ args: [ns.gzip(B("abc"))] }),
    "compress.unzstd": (ns) => ({ args: [ns.zstd(B("abc"))] }),
    "compress.unbrotli": (ns) => ({ args: [ns.brotli(B("abc"))] }),
    "compress.unsnappy": (ns) => ({ args: [ns.snappy(B("abc"))] }),
    "compress.lz4Decompress": (ns) => ({ args: [ns.lz4Compress(B("abc"))] }),
    "compress.lz4Unframe": (ns) => ({ args: [ns.lz4Frame(B("abc"))] }),
    "compress.inflate": (ns) => ({ args: [ns.deflate(B("abc"))] }),
    "compress.compressBound": () => ({ args: [16], bag: { algo: "gzip" } }),
    "compress.TarList": (ns) => ({ args: [ns.TarPack([{ name: "a.txt", data: B("x") }])] }),
    "compress.ZipList": (ns) => ({ args: [ns.ZipPack([{ name: "a.txt", data: B("x") }])] }),
    "compress.ZipRead": (ns) => ({ args: [ns.ZipPack([{ name: "a.txt", data: B("x") }]), "a.txt"] }),
    "compress.ZipReadAt": (ns) => ({ args: [ns.ZipPack([{ name: "a.txt", data: B("x") }]), 0] }),
    "config.TOML.parse": () => ({ args: ["a = 1"] }),
    "crypto.HKDF": () => ({ args: [], bag: { key: B("k") } }),
    "crypto.PBKDF2": () => ({ args: [], bag: { password: B("p"), salt: B("saltsalt"), iterations: 1 } }),
    "crypto.JWTVerify": (ns) => ({ args: [ns.JWTSign({ a: 1 }, B("k"), { alg: "HS256" }), B("k")], bag: { algorithms: ["HS256"] } }),
    "crypto.Argon2id.hash": () => ({ args: [B("p"), B("saltsalt")], bag: { memory: 64, iterations: 1, parallelism: 1 } }),
    "crypto.Ed25519PemFromRaw": () => ({ args: [], bag: { publicKey: new Uint8Array(32) } }),
    "crypto.X25519PemFromRaw": () => ({ args: [], bag: { publicKey: new Uint8Array(32) } }),
    "file.readFile": () => { const p = new Path(tmpRoot.toString() + "/exists.txt"); fileWrite(p, B("x")); return { args: [p] }; },
    "file.copyFile": () => { const p = new Path(tmpRoot.toString() + "/src.txt"); fileWrite(p, B("x")); return { args: [p, new Path(tmpRoot.toString() + "/dst" + (++tmpN) + ".txt")] }; },
    "mathx.signal.firwin": () => ({ args: [9, 0.25] }),
    "mathx.signal.biquad": () => ({ args: ["lowpass", 1000], bag: { fs: 48000 } }),
    "mathx.signal.butter": () => ({ args: [2, 0.25] }),
    "mathx.geo.crossTrack": () => ({ args: [1, 0, 0, -10, 0, 10] }),
    "ml.trainTestSplit": () => ({ args: [10] }),
    "ml.kFold": () => ({ args: [10] }),
    "ml.stratifiedKFold": () => ({ args: [[0, 1, 0, 1, 0, 1, 0, 1, 0, 1]] }),
    "svg.render": () => ({ args: ['<svg xmlns="http://www.w3.org/2000/svg" width="2" height="2"/>'] }),
    "svg.renderRGBA": () => ({ args: ['<svg xmlns="http://www.w3.org/2000/svg" width="2" height="2"/>'] }),
    "serialize.MsgPackDecode": (ns) => ({ args: [ns.MsgPackEncode(1)] }),
    "serialize.CBORDecode": (ns) => ({ args: [ns.CBOREncode(1)] }),
    "config.Env.load": () => { const p = tmpRoot.toString() + "/x.env"; fileWrite(new Path(p), B("A=1\n")); return { args: [p] }; },
    "xml.XMLParse": () => ({ args: ["<a/>"] }),
    "xml.XMLStringify": () => ({ args: [{ name: "a", attrs: {}, children: [] }] }),
    "uuid.v8": () => ({ args: [], bag: { fill: new Uint8Array(16) } }),
    "oauth2.buildWWWAuthenticate": () => ({ args: [], bag: { error: "invalid_token" } }),
    "oauth2.buildAuthorizationUrl": () => ({ args: [], bag: { authorizationEndpoint: "https://a.example/auth", clientId: "c", redirectUri: "https://a.example/cb" } }),
    "oauth2.verifyJWT": async (ns) => { const c = await import("dyna:crypto"); return { args: [c.JWTSign({ a: 1 }, B("k"), { alg: "HS256" }), B("k")], bag: { algorithms: ["HS256"], requireExp: false } }; },
};

const nsCache = {};
async function load(mod) {
    if (mod in nsCache) return nsCache[mod];
    try { nsCache[mod] = await import("dyna:" + mod); } catch (e) { nsCache[mod] = null; }
    return nsCache[mod];
}

const report = { pascal: 0, pascalOk: 0, pascalSkipped: 0, optCandidates: 0, optOk: 0, optNotCallable: 0, optSkipped: 0, violations: [], notCallable: [], pureChecked: [], metaOk: [] };

t.before(() => { tmpRoot = makeTempDir("traps-"); });
t.after(() => { if (tmpRoot) removeAll(tmpRoot); });

t.test("PascalCase exported functions are not constructors", async (a) => {
    for (const mod of Object.keys(D.modules)) a.ok(COVERED.has(mod) || OPTIONAL_BUILDS.has(mod), "dyna:" + mod + " is listed in COVERED and in the covers header");
    for (const [mod, m] of Object.entries(D.modules)) {
        const names = Object.keys(m.functions).filter((n) => /^[A-Z]/.test(n));
        if (!names.length) continue;
        const ns = await load(mod);
        if (!ns) { report.pascalSkipped += names.length; a.ok(OPTIONAL_BUILDS.has(mod), "dyna:" + mod + " imports"); continue; }
        for (const n of names) {
            const key = "new " + mod + "." + n;
            report.pascal++;
            const f = ns[n];
            let verdict;
            if (typeof f !== "function") verdict = "export is " + typeof f;
            else {
                try { new f(); verdict = "constructed"; }
                catch (e) { verdict = e instanceof TypeError && e.message === n + " is not a constructor" ? "ok" : "threw " + (e && e.name) + ": " + (e && e.message); }
            }
            if (verdict === "ok") { report.pascalOk++; a.ok(!(key in ALLOW), "stale ALLOW entry: " + key); continue; }
            report.violations.push(key + " -> " + verdict);
            a.ok(key in ALLOW, key + ": " + verdict);
        }
    }
    a.ok(report.pascal > 150, "PascalCase functions checked: " + report.pascal);
});

t.test("option bags refuse an unknown key with a TypeError naming it", async (a) => {
    const seen = new Set();
    for (const [mod, m] of Object.entries(D.modules)) {
        const entries = [];
        for (const [n, ovs] of Object.entries(m.functions)) entries.push([n, ovs, null]);
        for (const [o, members] of Object.entries(m.objects)) if (Array.isArray(members)) for (const mm of members) entries.push([mm.name, [mm], o]);
        for (const [n, ovs, obj] of entries) {
            const label = mod + "." + (obj ? obj + "." : "") + n;
            const ovsWithBag = ovs.filter((o) => Array.isArray(o.params) && o.params.some(isOptionParam));
            if (!ovsWithBag.length || seen.has(label)) continue;
            seen.add(label);
            report.optCandidates++;
            if ((SKIP_MODULES[mod] && !PURE_IN_SKIPPED.has(label)) || SKIP_FUNCTIONS[label]) { report.optSkipped++; continue; }
            const ns = await load(mod);
            const holder = ns && (obj ? ns[obj] : ns);
            const f = holder && holder[n];
            if (typeof f !== "function") { report.optNotCallable++; continue; }
            let verdict = "not-callable", why = "";
            for (const ov of ovsWithBag) {
                const bagAt = ov.params.findIndex(isOptionParam);
                let args = [], bag = {};
                let derivable = true;
                if (FIX[label]) {
                    try { const fx = await FIX[label](ns, holder); args = fx.args; bag = fx.bag || {}; } catch (e) { why = "fixture threw " + e; derivable = false; }
                } else for (let i = 0; i < bagAt; i++) {
                    const v = valueFor(ov.params[i].type, 0);
                    if (v === NOT) { derivable = false; why = "no minimal value for " + ov.params[i].name + ": " + ov.params[i].type; break; }
                    args.push(v);
                }
                if (!derivable) continue;
                let base;
                try { base = await settle(f.apply(holder, [...args, bag])); } catch (e) { why = "baseline threw " + (e && e.name) + ": " + String(e && e.message).slice(0, 80); continue; }
                release(base);
                let res, err = null;
                try { res = await settle(f.apply(holder, [...args, { ...bag, __bogus: 1 }])); } catch (e) { err = e; }
                release(res);
                if (err && err instanceof TypeError && /__bogus/.test(err.message)) verdict = "ok";
                else verdict = err ? "threw " + err.name + ": " + String(err.message).slice(0, 100) : "accepted { __bogus: 1 }";
                const metaAt = ov.params.findIndex((q) => q.name === "meta");
                if (verdict === "ok" && metaAt > bagAt) {
                    let me = null, mres;
                    try { mres = await settle(f.apply(holder, [...args, bag, { __bogus: 1 }])); } catch (e) { me = e; }
                    release(mres);
                    if (me instanceof TypeError && /__bogus/.test(me.message)) report.metaOk.push(label);
                    else verdict = "meta bag " + (me ? "threw " + me.name + ": " + String(me.message).slice(0, 100) : "accepted { __bogus: 1 }");
                }
                break;
            }
            if (verdict === "not-callable") { report.optNotCallable++; report.notCallable.push(label + ": " + why); continue; }
            const key = "opts " + label;
            if (verdict === "ok") { if (PURE_IN_SKIPPED.has(label)) report.pureChecked.push(label); report.optOk++; a.ok(!(key in ALLOW), "stale ALLOW entry: " + key); continue; }
            report.violations.push(key + " -> " + verdict);
            a.ok(key in ALLOW, key + ": " + verdict);
        }
    }
    a.ok(report.optOk > 40, "option bags verified: " + report.optOk);
});

t.test("report", (a) => {
    print("contract-traps: PascalCase checked " + report.pascal + " (ok " + report.pascalOk + ", optional-build skipped " + report.pascalSkipped + ")");
    print("contract-traps: option-bag functions " + report.optCandidates + " (verified " + report.optOk + ", not cheaply callable " + report.optNotCallable + ", side-effect modules skipped " + report.optSkipped + ")");
    print("contract-traps: violations " + report.violations.length + " (all in ALLOW)");
    for (const v of report.notCallable) print("  not callable: " + v);
    for (const v of report.violations) print("  " + v + "  [" + (ALLOW[v.split(" -> ")[0]] || "NOT ALLOWED") + "]");
    a.eq(Object.keys(ALLOW).length, report.violations.length, "every ALLOW entry corresponds to a live violation");
    a.eq(report.pureChecked.sort().join(","), [...PURE_IN_SKIPPED].sort().join(","), "every PURE_IN_SKIPPED factory was found in the d.ts and checked");
    print("contract-traps: meta bags verified " + report.metaOk.length + " (" + report.metaOk.join(", ") + ")");
    a.ok(report.metaOk.includes("net.LengthFieldDecoder") && report.metaOk.includes("net.DelimiterDecoder"), "the frame decoders' meta bags are strict");
}, { dependsOn: ["PascalCase exported functions are not constructors", "option bags refuse an unknown key with a TypeError naming it"] });

await t.run();
