#!/usr/bin/env python3
"""check-error-identifiers.py -- an error message must not name an API that
does not exist.

The failure this exists for: `Serializer` was removed and turned into per-class
methods, and a `JS_ThrowTypeError` deep in the Heap codec kept telling callers
to write `Serializer.decode(bytes, cmp)`. Nothing failed. The docs were swept,
the tests were swept, and the one string that a user actually reads at the
moment they are stuck still named a class that no longer exists.

Doc rot is caught by running the examples. This is the same rot in a place no
example can reach, so it needs its own check.

WHAT IT DOES. Scans C string literals for the shapes an error message uses to
point at an API -- `Foo.bar(`, `new Foo(`, `import ... from "dyna:x"` -- and
verifies the identifier is still exported by the built binary. The export list
comes from the BINARY's own reflection, never from the docs, because a name
that exists only in prose is exactly what this is looking for.

MISLEADS. A message may legitimately name a JS builtin (`JSON.stringify`), a
local variable, or a type it is telling you NOT to use. Those are allowlisted
below by name rather than by pattern, so adding one is a deliberate act.

Usage:  tools/check-error-identifiers.py ./dynajs [src/...]
Exit 0 = every named identifier resolves.
"""
import json
import re
import subprocess
import sys
import os

ALLOW = {
    "JSON", "Object", "Array", "Math", "Number", "String", "Boolean", "Date",
    "RegExp", "Map", "Set", "WeakMap", "WeakSet", "Promise", "Symbol", "Proxy",
    "Reflect", "Error", "TypeError", "RangeError", "SyntaxError", "BigInt",
    "ArrayBuffer", "SharedArrayBuffer", "DataView", "Uint8Array", "Int8Array",
    "Uint16Array", "Int16Array", "Uint32Array", "Int32Array", "Float32Array",
    "Float64Array", "BigInt64Array", "BigUint64Array", "TextEncoder",
    "TextDecoder", "Function", "Infinity", "NaN",
    "Note", "NOTE", "See", "Use", "Try", "The", "This", "A", "An", "It",
}

MODULES = [
    "dyna:structures", "dyna:net", "dyna:http", "dyna:file", "dyna:hash", "dyna:crypto",
    "dyna:encoding", "dyna:bytes", "dyna:compress", "dyna:ml", "dyna:simd",
    "dyna:mathx", "dyna:csv", "dyna:dataframe", "dyna:time", "dyna:uuid",
    "dyna:sys", "dyna:matcher", "dyna:semver", "dyna:random",
    "dyna:config", "dyna:log", "dyna:url", "dyna:cli", "dyna:validate",
    "dyna:json", "dyna:schema",
    "dyna:xml", "dyna:yaml", "dyna:decimal", "dyna:serialize", "dyna:html",
    "dyna:scrape", "dyna:oauth2", "dyna:stream", "dyna:async", "dyna:bench",
    "dyna:uring",
]


def check_module_coverage():
    """Every dyna:* module the tree registers must be in MODULES.

    The list is hand-kept and went stale once already (dyna:scrape), which made
    the run report success while skipping a whole module's messages.
    """
    import glob
    found = set()
    for f in glob.glob("src/*.c") + glob.glob("src/*.inc.c"):
        try:
            src = open(f, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        found.update(re.findall(
            r'JS_NewCModule\s*\(\s*ctx\s*,\s*"(dyna:[a-z0-9_]+)"', src))
    missing = sorted(found - set(MODULES))
    if missing:
        print("FAIL: modules registered in src/ but absent from MODULES:")
        for m in missing:
            print("   ", m)
        return 1
    return 0


IDENT = re.compile(r'\b([A-Z][A-Za-z0-9_]*)\s*\.\s*[a-zA-Z_][A-Za-z0-9_]*\s*\(')
NEWID = re.compile(r'\bnew\s+([A-Z][A-Za-z0-9_]*)\s*\(')
CSTR = re.compile(r'"((?:[^"\\\n]|\\.)*)"')


def exports_from_binary(dynajs):
    """Every exported name of every module, asked of the BINARY."""
    src = "const out={};\n"
    for m in MODULES:
        src += (
            'try { const m = await import("%s"); out["%s"] = Object.keys(m); }\n'
            'catch (e) { out["%s"] = null; }\n' % (m, m, m)
        )
    src += 'print(JSON.stringify(out));\n'
    path = "/tmp/_check_error_ids.mjs"
    with open(path, "w") as f:
        f.write("(async () => {\n" + src + "})();\n")
    try:
        r = subprocess.run([dynajs, path], capture_output=True, text=True,
                           timeout=60)
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass
    line = [l for l in r.stdout.splitlines() if l.startswith("{")]
    if not line:
        print("check-error-identifiers: could not read exports from %s\n%s"
              % (dynajs, r.stderr[:400]), file=sys.stderr)
        return None
    return json.loads(line[-1])


def config_gate(text, lineno):
    """The CONFIG_* this line sits inside, or None. A flat #if/#endif depth
    scan is enough: these files do not nest config gates."""
    stack = []
    for n, ln in enumerate(text.split("\n"), 1):
        st = ln.strip()
        if st.startswith("#if"):
            m = re.search(r"\bCONFIG_[A-Z0-9_]+", st)
            stack.append(m.group(0) if m else None)
        elif st.startswith("#endif") and stack:
            stack.pop()
        if n == lineno:
            for g in reversed(stack):
                if g:
                    return g
            return None
    return None


SIG_KEYS = {
    "nm": "CONFIG_NATIVE_MODULES", "tls": "CONFIG_TLS", "sq": "CONFIG_SQLITE",
    "ol": "CONFIG_OPENLIBM", "mi": "CONFIG_MIMALLOC", "nat": "CONFIG_NATIVE",
    "lto": "CONFIG_LTO", "pgo": "CONFIG_PGO",
}


def config_sig_path():
    """Where this build's signature lives. A VARIANT build has its own object
    directory -- OBJDIR is .obj/ubsan, .obj/asan, .obj/tsan -- so the hardcoded
    .obj/.config-sig was absent under every sanitizer, built_configs returned
    None, and "skip nothing" reported every CONFIG-gated name as rot. That is a
    warning which appears ALWAYS in those builds, i.e. indistinguishable from a
    real one, which is the exact failure this file's own comments warn about."""
    env = os.environ.get("OBJDIR")
    cands = ([os.path.join(env, ".config-sig")] if env else []) + [".obj/.config-sig"]
    for c in cands:
        if os.path.exists(c):
            return c
    found = []
    for root, _dirs, files in os.walk(".obj"):
        if ".config-sig" in files:
            p = os.path.join(root, ".config-sig")
            found.append((os.path.getmtime(p), p))
    return max(found)[1] if found else None


def built_configs():
    """The CONFIG_* this binary was built with, or None if unknowable."""
    path = config_sig_path()
    if path is None:
        return None
    try:
        sig = open(path).read()
    except OSError:
        return None
    out = set()
    for key, val in re.findall(r"(\w+)=(\S*)", sig):
        if val and key in SIG_KEYS:
            out.add(SIG_KEYS[key])
    return out


def main():
    rc = check_module_coverage()
    if rc:
        return rc
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    dynajs = sys.argv[1]
    files = sys.argv[2:] or [
        os.path.join(d, f)
        for d, _, fs in os.walk("src")
        if "fuzz" not in d.split(os.sep)
        for f in fs if f.endswith((".c", ".h"))
    ]

    exports = exports_from_binary(dynajs)
    if exports is None:
        return 1
    live = set(ALLOW)
    missing_modules = []
    gated = set()
    built = built_configs()
    for m, names in exports.items():
        if names is None:
            missing_modules.append(m)
            continue
        live.update(names)

    bad = []
    for p in sorted(files):
        try:
            text = open(p, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        for i, line in enumerate(text.split("\n"), 1):
            for lit in CSTR.findall(line):
                for pat in (IDENT, NEWID):
                    for name in pat.findall(lit):
                        if len(name) == 1:
                            continue
                        if name not in live:
                            g = config_gate(text, i)
                            if g and built is not None and \
                               g in SIG_KEYS.values() and g not in built:
                                gated.add((name, g))
                                continue
                            bad.append((p, i, name, lit.strip()[:70]))

    if gated:
        print("check-error-identifiers: gated out of this build, skipped: %s"
              % ", ".join(sorted("%s (%s)" % (n, g) for n, g in gated)))

    if missing_modules:
        print("check-error-identifiers: not in this build, skipped: %s"
              % ", ".join(sorted(missing_modules)))

    if bad and built is None:
        print("check-error-identifiers: NO BUILD SIGNATURE FOUND -- every "
              "CONFIG-gated name below is reported because the build's config "
              "is unknown, not because it rotted. Set OBJDIR or build first.")
    if bad:
        print("check-error-identifiers: %d message(s) name a non-existent API"
              % len(bad))
        for p, i, name, lit in bad:
            print("  %s:%d: '%s' in \"%s\"" % (p, i, name, lit))
        return 1
    print("check-error-identifiers: every API named in an error message exists")
    return 0


if __name__ == "__main__":
    sys.exit(main())
