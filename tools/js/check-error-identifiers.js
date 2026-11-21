// check-error-identifiers: every Name.method( / new Name( inside a C string literal under src/ must be an
// export of some dyna:* module in the built binary (or an ALLOW word), so an error message never points
// at an API that does not exist.
// F-CEI (2026-11-26): the module list was a hand-kept array and its coverage check globbed only src/*.c,
// so modules registered under src/modules/** (webrtc, ssh, sftp, svg, ...) were never imported and real
// exports such as RTCRtpSender, RTCRtpReceiver, SSHClient and SFTPClient were reported missing (rc=1).
// The modules are now discovered from every JS_NewCModule(ctx, "dyna:...") call under the scanned root.
//   ./dynajs --std tools/js/check-error-identifiers.js ./dynajs [src/...]
//   ./dynajs --std tools/js/check-error-identifiers.js selftest [./dynajs]
// Selftest mutants: discovery limited to the top of the root (nested-module case red), every name
// accepted (unknown-name case red).
import * as std from "std";
import * as os from "os";
import { Exec, pid } from "dyna:sys";
import { walk, join, glob, exists, readText, writeText, Out } from "./pyshim.js";

const ALLOW = [
    "JSON", "Object", "Array", "Math", "Number", "String", "Boolean", "Date",
    "RegExp", "Map", "Set", "WeakMap", "WeakSet", "Promise", "Symbol", "Proxy",
    "Reflect", "Error", "TypeError", "RangeError", "SyntaxError", "BigInt",
    "ArrayBuffer", "SharedArrayBuffer", "DataView", "Uint8Array", "Int8Array",
    "Uint16Array", "Int16Array", "Uint32Array", "Int32Array", "Float32Array",
    "Float64Array", "BigInt64Array", "BigUint64Array", "TextEncoder",
    "TextDecoder", "Function", "Infinity", "NaN",
    "Note", "NOTE", "See", "Use", "Try", "The", "This", "A", "An", "It",
];
const o = new Out();

function discoverModules(root) {
    const found = new Set();
    for (const [d, , fs] of walk(root)) {
        for (const f of fs) {
            if (!f.endsWith(".c")) continue;
            let src;
            try {
                src = readText(join(d, f));
            } catch (e) {
                continue;
            }
            for (const m of src.matchAll(/JS_NewCModule\s*\(\s*ctx\s*,\s*"(dyna:[a-z0-9_]+)"/g)) found.add(m[1]);
        }
    }
    return [...found].sort();
}

const IDENT = /\b([A-Z][A-Za-z0-9_]*)\s*\.\s*[a-zA-Z_][A-Za-z0-9_]*\s*\(/g;
const NEWID = /\bnew\s+([A-Z][A-Za-z0-9_]*)\s*\(/g;
const CSTR = /"((?:[^"\\\n]|\\.)*)"/g;

function exportsFromBinary(dynajs, modules) {
    let src = "const out={};\n";
    for (const m of modules) src += `try { const m = await import("${m}"); out["${m}"] = Object.keys(m); }\ncatch (e) { out["${m}"] = null; }\n`;
    src += "print(JSON.stringify(out));\n";
    const path = `${std.getenv("TMPDIR") || "/tmp"}/_check_error_ids_${pid()}_${Date.now()}_${Math.floor(Math.random() * 1e9)}.mjs`;
    writeText(path, "(async () => {\n" + src + "})();\n");
    let r;
    try {
        r = Exec(dynajs, [path], { timeoutMs: 60000, env: std.getenviron() });
    } finally {
        os.remove(path);
    }
    const lines = r.stdout.split("\n").filter((l) => l.startsWith("{"));
    if (!lines.length) {
        std.err.puts(`check-error-identifiers: could not read exports from ${dynajs}\n${r.stderr.slice(0, 400)}\n`);
        return null;
    }
    return JSON.parse(lines[lines.length - 1]);
}

function configGate(text, lineno) {
    const stack = [];
    const lines = text.split("\n");
    for (let n = 1; n <= lines.length; n++) {
        const st = lines[n - 1].trim();
        if (st.startsWith("#if")) stack.push(st.includes("||") ? [] : [...st.matchAll(/\bCONFIG_[A-Z0-9_]+/g)].map((m) => m[0]));
        else if (st.startsWith("#endif") && stack.length) stack.pop();
        if (n === lineno) return stack.reverse().flat();
    }
    return [];
}

const SIG_KEYS = {
    nm: "CONFIG_NATIVE_MODULES", tls: "CONFIG_TLS", sq: "CONFIG_SQLITE",
    mi: "CONFIG_MIMALLOC", nat: "CONFIG_NATIVE",
    lto: "CONFIG_LTO", pgo: "CONFIG_PGO", sdl3: "CONFIG_SDL3", rl: "CONFIG_RAYLIB", ui: "CONFIG_UI",
};
const SIG_VALUES = new Set(Object.values(SIG_KEYS));

function configSigPath() {
    const env = std.getenv("OBJDIR");
    for (const c of (env ? [env] : []).concat([".obj/release"])) if (exists(join(c, "flags.env"))) return join(c, "flags.env");
    return null;
}

function builtConfigs() {
    const path = configSigPath();
    if (path === null) return null;
    let sig;
    try {
        sig = readText(path);
    } catch (e) {
        return null;
    }
    const out = new Set();
    for (const m of sig.matchAll(/-D(CONFIG_\w+)/g)) if (SIG_VALUES.has(m[1])) out.add(m[1]);
    return out;
}

function check(dynajs, files, root) {
    const modules = discoverModules(root);
    if (!modules.length) {
        o.print(`FAIL: no JS_NewCModule(ctx, "dyna:...") registration found under ${root}`);
        return 1;
    }
    if (!files.length) {
        for (const [d, , fs] of walk(root)) {
            if (d.split("/").includes("fuzz")) continue;
            for (const f of fs) if (f.endsWith(".c") || f.endsWith(".h")) files.push(join(d, f));
        }
    }
    const exports = exportsFromBinary(dynajs, modules);
    if (exports === null) return 1;
    const live = new Set(ALLOW);
    const missingModules = [];
    const gated = new Set();
    const built = builtConfigs();
    for (const [m, names] of Object.entries(exports)) {
        if (names === null) {
            missingModules.push(m);
            continue;
        }
        for (const n of names) live.add(n);
    }
    const bad = [];
    for (const p of files.sort()) {
        let text;
        try {
            text = readText(p);
        } catch (e) {
            continue;
        }
        const lines = text.split("\n");
        for (let i = 1; i <= lines.length; i++) {
            const line = lines[i - 1];
            for (const lm of line.matchAll(CSTR)) {
                const lit = lm[1];
                for (const pat of [IDENT, NEWID]) {
                    for (const nm of lit.matchAll(pat)) {
                        const name = nm[1];
                        if (name.length === 1) continue;
                        if (!live.has(name)) {
                            const g = built === null ? undefined : configGate(text, i).find((c) => SIG_VALUES.has(c) && !built.has(c));
                            if (g) {
                                gated.add(`${name} (${g})`);
                                continue;
                            }
                            bad.push([p, i, name, lit.trim().slice(0, 70)]);
                        }
                    }
                }
            }
        }
    }
    if (gated.size) o.print("check-error-identifiers: gated out of this build, skipped: " + [...gated].sort().join(", "));
    if (missingModules.length) o.print("check-error-identifiers: not in this build, skipped: " + missingModules.sort().join(", "));
    if (bad.length && built === null)
        o.print("check-error-identifiers: NO BUILD SIGNATURE FOUND -- every CONFIG-gated name below is reported because the build's config is unknown, not because it rotted. Build first (./build.sh release writes .obj/release/flags.env) or set OBJDIR to the object dir of the build under test.");
    if (bad.length) {
        o.print(`check-error-identifiers: ${bad.length} message(s) name a non-existent API`);
        for (const [p, i, name, lit] of bad) o.print(`  ${p}:${i}: '${name}' in "${lit}"`);
        return 1;
    }
    o.print("check-error-identifiers: every API named in an error message exists");
    return 0;
}

function selftest(dynajs) {
    const T = `${std.getenv("TMPDIR") || "/tmp"}/_cei_selftest_${pid()}_${Date.now()}`;
    const mk = (p, s) => {
        let cur = T;
        for (const x of p.split("/").slice(0, -1)) {
            cur = join(cur, x);
            os.mkdir(cur);
        }
        writeText(join(T, p), s);
        return join(T, p);
    };
    os.mkdir(T);
    const reg = 'void r(JSContext* ctx) { JS_NewCModule(ctx, "dyna:webrtc", 0); JS_NewCModule(ctx, "dyna:ssh", 0); }\n';
    const good = mk("src/modules/webrtc/w.c", reg
        + 'const char* a = "codec %s is not in RTCRtpSender.getCapabilities(kind)";\n'
        + 'const char* b = "codec %s is not in RTCRtpReceiver.getCapabilities(kind)";\n'
        + 'const char* c = "SSH: use SSHClient.connect(options)";\n');
    const bad = mk("src/modules/webrtc/bad.c", 'const char* d = "use NoSuchWidget.make(x)";\n');
    const cases = [
        ["a module registered in a nested src/modules dir resolves its exports", [good], 0],
        ["an unknown Name.method( in a message is reported", [good, bad], 1],
    ];
    let fails = 0;
    for (const [name, files, want] of cases) {
        const got = check(dynajs, files, join(T, "src"));
        if (got !== want) fails++;
        o.print(`selftest: ${got === want ? "ok  " : "FAIL"} ${name} (rc ${got}, want ${want})`);
    }
    Exec("rm", ["-rf", T], { timeoutMs: 10000 });
    o.print(fails ? `check-error-identifiers selftest: ${fails} failure(s)` : "check-error-identifiers selftest: nested modules resolve and unknown names are caught");
    return fails ? 1 : 0;
}

function main() {
    if (scriptArgs[1] === "selftest") return selftest(scriptArgs[2] || "./dynajs");
    if (scriptArgs.length < 2) {
        o.print("usage: check-error-identifiers.js ./dynajs [src/...] | selftest [./dynajs]");
        return 2;
    }
    return check(scriptArgs[1], scriptArgs.slice(2), "src");
}
const rc = main();
o.flush();
std.exit(rc);
