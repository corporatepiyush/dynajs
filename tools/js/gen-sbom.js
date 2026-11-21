// flags: --std
// tools/js/gen-sbom.js: the port of tools/gen-sbom.py (deleted); its output was proven identical, apart from naming itself
// Emit a CycloneDX 1.6 SBOM for the CURRENT build configuration.
//
// A committed SBOM rots the moment a flag changes: half of these components are
// optional and config-gated, so "what this project depends on" is not a constant.
// This reads the configuration of the last build (OBJDIR/flags.env, default
// .obj/release/flags.env, written by ./build.sh release) and reports what is ACTUALLY
// linked: a library appears only when its CONFIG_* define is in that build's CFLAGS.
//
// Usage (what the old sbom target of build.sh ran):
//   ./dynajs --std tools/js/gen-sbom.js --timestamp "$(date -u +%Y-%m-%dT%H:%M:%SZ)" > sbom.json
//   OBJDIR=.obj-san ./dynajs --std tools/js/gen-sbom.js     # the ASan+UBSan build's configuration
//
// Versions come from pkg-config, searched first in the -L<dir>/pkgconfig directories
// the build linked against (so a Homebrew keg reports its own version), and from
// the pinned ref where they are vendored. A component whose version cannot be
// determined says so rather than guessing -- an invented version is worse than a
// missing one, because it looks authoritative.
import * as std from "std";
import { Exec } from "dyna:sys";
import { abspath, dirname, isdir, exists, readText, pyStrip, join } from "./pyshim.js";

const ROOT = dirname(dirname(dirname(abspath(scriptArgs[0]))));

function sh(...cmd) {
    try {
        const r = Exec(cmd[0].includes("/") ? cmd[0] : findExe(cmd[0]), cmd.slice(1), { timeoutMs: 10000, env: std.getenviron() });
        return r.code === 0 ? pyStrip(r.stdout) : "";
    } catch (e) {
        return "";
    }
}

const whichCache = new Map();
function findExe(name) {
    if (whichCache.has(name)) return whichCache.get(name);
    for (const d of (std.getenv("PATH") || "").split(":")) {
        const p = join(d || ".", name);
        if (exists(p)) {
            whichCache.set(name, p);
            return p;
        }
    }
    throw new Error("not found: " + name);
}

function buildFlags() {
    const dir = std.getenv("OBJDIR") || join(ROOT, ".obj/release");
    const f = join(dir, "flags.env");
    if (!exists(f)) {
        std.err.puts(`gen-sbom.js: no ${f}: run ./build.sh release first (or set OBJDIR)\n`);
        std.exit(2);
    }
    const text = readText(f).replace(/\\(.)/g, "$1");
    const defs = new Set([...text.matchAll(/-D(CONFIG_\w+)/g)].map((m) => m[1]));
    const pcDirs = [...new Set([...text.matchAll(/-L(\S+)/g)].map((m) => m[1] + "/pkgconfig"))].filter(isdir);
    return { defs, pcDirs };
}
const BUILD = buildFlags();
std.setenv("PKG_CONFIG_PATH", BUILD.pcDirs.concat(std.getenv("PKG_CONFIG_PATH") ? [std.getenv("PKG_CONFIG_PATH")] : []).join(":"));

function pkgVersion(name) {
    return sh("pkg-config", "--modversion", name) || "unknown";
}

function vendoredVersion(relpath, describeDir) {
    const d = join(ROOT, describeDir);
    if (!isdir(d)) return null;
    return sh("git", "-C", d, "describe", "--tags", "--always") || "unknown";
}

function component(name, version, ctype, supplier, licenseId, purl, desc) {
    const c = { type: ctype, name, version, supplier: { name: supplier }, licenses: [{ license: { id: licenseId } }] };
    if (purl) c.purl = purl;
    if (desc) c.description = desc;
    return c;
}

function main() {
    let timestamp = null;
    const args = scriptArgs.slice(1);
    for (let i = 0; i < args.length; i++) {
        if (args[i] === "--timestamp" && i + 1 < args.length) timestamp = args[++i];
        else if (args[i].startsWith("--timestamp=")) timestamp = args[i].slice(12);
        else {
            std.err.puts(`usage: gen-sbom.js [-h] [--timestamp TIMESTAMP]\ngen-sbom.js: error: unrecognized arguments: ${args.slice(i).join(" ")}\n`);
            return 2;
        }
    }
    let version = "unknown";
    const vf = join(ROOT, "VERSION");
    if (exists(vf)) version = pyStrip(readText(vf));
    const comps = [];
    comps.push(component("quickjs", "2026-06-04", "library", "Fabrice Bellard and Charlie Gordon", "MIT", null, "Upstream engine core; this project is a fork of it."));
    const mi = BUILD.defs.has("CONFIG_MIMALLOC") ? vendoredVersion("third_party/mimalloc", "third_party/mimalloc") : null;
    if (mi) comps.push(component("mimalloc", mi, "library", "Microsoft", "MIT", "pkg:github/microsoft/mimalloc@" + mi));
    const LINKED = [
        ["CONFIG_SQLITE", "sqlite3", "sqlite", "SQLite Consortium", "blessing"],
        ["CONFIG_TLS", "openssl", "openssl", "OpenSSL Project", "Apache-2.0"],
        ["CONFIG_ZSTD", "libzstd", "zstd", "Meta Platforms", "BSD-3-Clause"],
        ["CONFIG_BROTLI", "libbrotlidec", "brotli", "Google", "MIT"],
        ["CONFIG_IO_URING", "liburing", "liburing", "Jens Axboe", "MIT"],
        ["CONFIG_SDL3", "sdl3", "sdl3", "SDL contributors", "Zlib"],
        ["CONFIG_RAYLIB", "raylib", "raylib", "Ramon Santamaria", "Zlib"],
    ];
    for (const [def, pc, name, supplier, lic] of LINKED) {
        if (!BUILD.defs.has(def)) continue;
        const v = pkgVersion(pc);
        comps.push(component(name, v, "library", supplier, lic, v === "unknown" ? null : `pkg:generic/${name}@${v}`));
    }
    const doc = {
        bomFormat: "CycloneDX",
        specVersion: "1.6",
        version: 1,
        metadata: {
            tools: { components: [component("gen-sbom.js", version, "application", "dynajs", "MIT")] },
            component: component("dynajs", version, "application", "dynajs", "MIT", null, "JavaScript engine and runtime, forked from QuickJS."),
        },
        components: comps,
    };
    if (timestamp) doc.metadata.timestamp = timestamp;
    std.out.puts(JSON.stringify(doc, null, 2) + "\n");
    return 0;
}
std.exit(main());
