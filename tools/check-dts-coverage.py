#!/usr/bin/env python3
"""Cross-check dynajs.d.ts against the binary's own exports.

TWO directions (audit X-1: the original tool only ran the first, which is how
PgPool/RedisPool sat declared inside dyna:net while absent from it at runtime):

1. live -> d.ts: every runtime export of every loadable dyna:* module must be
   declared. Enumerates each module IN A FRESH PROCESS. That matters: importing
   dyna:net after dyna:http fails in one process (a known engine import-order
   cycle), which is exactly how tools/api-inventory.js once reported dyna:net as
   "missing" and the whole module went undeclared.

2. d.ts -> live: every RUNTIME-LEVEL declaration (class/function/const/var/
   enum/namespace) in every declared module block must resolve to a live
   export of that module. `interface`/`type` declarations are type-only and
   correctly absent at runtime, so they are out of scope here, as are members
   INSIDE namespaces (pinned by tests/test_module_surface.js). File-path
   (wildcard) module blocks -- src/pool.js, imported relatively, not a builtin
   -- are probed through the FILE_MODULES map below; a wildcard block with no
   mapping entry is a hard error, because an unprobeable declaration is
   exactly the drift this direction exists to catch. Modules declared but not
   loadable on this platform (dyna:uring on darwin) are a documented SKIP,
   listed under EXCEPTIONS rather than silently dropped.
"""
import json, os, re, subprocess, sys

# Declared file-path (wildcard) module blocks: d.ts pattern -> live specifier
# to import from the repo root. pool.js is pure JS shipped in the source tree;
# the d.ts declares it as "*/src/pool.js" (TypeScript's one-wildcard ambient
# form) because a relative name is not legal in an ambient module declaration.
FILE_MODULES = {
    "*/src/pool.js": "./src/pool.js",
}

# Declared dyna:* modules that this platform cannot load to verify. Keep the
# reason next to the name; anything NOT listed here fails the gate.
EXCEPTIONS = {
    "dyna:uring": "io_uring is Linux-only; a darwin binary cannot load it to "
                  "probe (its d.ts block documents the gating). Linux CI "
                  "verifies this block through the same direction.",
}

RUNTIME_DECL = re.compile(
    r"^ {4}(?:export\s+)?(?:declare\s+)?(?:abstract\s+)?"
    r"(?:class|function|const|var|enum|namespace)\s+([A-Za-z_$][\w$]*)", re.M)


def declared_runtime_names(block_text):
    """Top-level runtime-level names declared in one module block.

    Comments are stripped first: doc text like 'Same class as dyna:http.X'
    must not read as a declaration. interface/type are deliberately NOT
    matched -- they are type-only and correctly absent at runtime. The {4}
    indent anchors TOP level: functions inside a `namespace` block (indented
    8) belong to the namespace's surface, not the module's.
    """
    text = re.sub(r"/\*.*?\*/", "", block_text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return sorted(set(RUNTIME_DECL.findall(text)))


def brace_blocks(src):
    """Exact per-module block bodies via brace matching.

    The split() trick above leaves a block's text running to the NEXT block
    marker, which is fine for direction 1 (a name may be declared anywhere in
    the file's dyna blocks) but would over-approximate direction 2: scanning
    the tail would attribute every later module's declarations to this one.
    Comments are stripped first so doc braces cannot unbalance the count.
    """
    text = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    out = {}
    for m in re.finditer(r'\ndeclare module "([^"]+)"\s*\{', text):
        depth, i = 1, m.end()
        while depth and i < len(text):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
            i += 1
        out[m.group(1)] = out.get(m.group(1), "") + "\n" + text[m.end(): i - 1]
    return out


PROBE = """
import(m).then(ns => {
  const out = { names: [] };
  const seen = new Set();
  function collect(prefix, obj) {
    for (const k of Object.getOwnPropertyNames(obj)) {
      if (k === "prototype" || k === "constructor" || k === "length" || k === "name") continue;
      out.names.push(prefix + k);
    }
    const p = obj.prototype;
    if (p && !seen.has(p)) {
      seen.add(p);
      for (const k of Object.getOwnPropertyNames(p)) {
        if (k === "constructor") continue;
        out.names.push(prefix + "prototype." + k);
      }
    }
  }
  for (const k of Object.getOwnPropertyNames(ns)) {
    if (k === "default") continue;
    const v = ns[k];
    out.names.push(k);
    if (typeof v === "function" && v.prototype && Object.getOwnPropertyNames(v.prototype).length > 1)
      collect(k + ".", v);
  }
  print(JSON.stringify(out));
}).catch(e => print(JSON.stringify({ error: e.message })));
"""

def module_names():
    mods = set()
    import os
    for root, _, files in os.walk("src"):
        for f in files:
            if not f.endswith((".c", ".h")):
                continue
            try:
                text = open(os.path.join(root, f), encoding="utf-8", errors="ignore").read()
            except OSError:
                continue
            for m in re.findall(r'"dyna:[a-z0-9]+"', text):
                mods.add(m[1:-1])
    return sorted(mods)

def probe(module):
    """Enumerate one module in a FRESH process; (names, error)."""
    r = subprocess.run(["./dynajs", "-e", "const m = %r;\n%s" % (module, PROBE)],
                       capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        return [], "probe exited %d: %s" % (r.returncode, r.stderr[-200:])
    out = r.stdout.strip().splitlines()
    if not out:
        return [], "no output"
    try:
        d = json.loads(out[-1])
    except json.JSONDecodeError:
        return [], "unparsable output: " + out[-1][:200]
    if "error" in d:
        return [], "module refused: " + d["error"]
    return sorted(set(d["names"])), None

def main():
    src = open("dynajs.d.ts").read()
    pat = re.compile(r'\ndeclare module "((?:dyna:|ext:)[a-z0-9]+)" \{')
    blocks = pat.split(src)
    mods_text = {}
    for i in range(1, len(blocks), 2):
        # a module may appear twice (e.g. the dyna:net augmentation block that
        # declares pool.js's queryIter): MERGE, or the last block wins and
        # every symbol declared only in the first reads as missing
        mods_text[blocks[i]] = mods_text.get(blocks[i], "") + "\n" + blocks[i + 1]
    # file-path (wildcard) blocks: src/pool.js and future JS-side modules
    pat_file = re.compile(r'\ndeclare module "([^"]*\*[^"]*)" \{')
    fblocks = pat_file.split(src)
    for i in range(1, len(fblocks), 2):
        mods_text[fblocks[i]] = fblocks[i + 1]

    declared = {m for m in mods_text if m.startswith("dyna:")}
    loadable = module_names()
    probed, refused = [], []
    for m in loadable:
        names, err = probe(m)
        if err:
            refused.append((m, err))
            continue
        probed.append((m, names))

    # A module this platform cannot load (dyna:uring off Linux) is a SKIP,
    # not a gate failure: there is no binary here to verify declarations
    # against, so the d.ts deliberately does not declare it.
    missing_mods = [m for (m, _) in probed if m not in declared]
    missing_syms = []
    total = 0
    for m, names in probed:
        text = mods_text.get(m, "")
        for s in names:
            total += 1
            base = s.split(".")[0]
            if not re.search(
                    r"\b(class|function|const|var|namespace|interface|type)\s+"
                    + re.escape(base) + r"\b", text):
                missing_syms.append(f"{m} -> {s}")

    print(f"modules loadable: {len(probed)}")
    print(f"skipped (not loadable on this platform): {len(refused)}")
    for m, err in refused:
        print(f"  {m}: {err}")
    print(f"symbols exported: {total}")
    print(f"MISSING module blocks: {len(missing_mods)}")
    for m in missing_mods:
        print("  " + m)
    print(f"MISSING symbols (base undeclared): {len(missing_syms)}")
    for x in missing_syms[:60]:
        print("  " + x)
    if len(missing_syms) > 60:
        print(f"  ... and {len(missing_syms) - 60} more")

    # ---- direction 2: d.ts -> live (audit X-1) ---------------------------
    # Every runtime-level declaration must resolve to a live export of its
    # module. Scope (see module docstring): interface/type decls and members
    # INSIDE namespaces are type-only / pinned elsewhere, so they are not
    # compared here.
    declared_not_live = []
    exception_skips = []
    exact = brace_blocks(src)
    for m in sorted(exact):
        if not (m.startswith("dyna:") or "*" in m):
            continue                      # std/os/global blocks: not probed
        decl_names = declared_runtime_names(exact[m])
        if not decl_names:
            continue                      # type-only block; nothing to verify
        if "*" in m:
            # a declared file-path module: load it by its real specifier
            if m not in FILE_MODULES:
                declared_not_live.append(
                    f"{m} -> <wildcard module with no FILE_MODULES entry>")
                continue
            names, err = probe(FILE_MODULES[m])
            if err:
                declared_not_live.append(f"{m} -> <probe failed: {err}>")
                continue
            live_names = {n.split(".")[0] for n in names}
        else:
            live = next((n for (mm, n) in probed if mm == m), None)
            if live is None:
                # declared but not loadable on this platform: an explicit,
                # documented exception -- or drift the gate must flag
                if m in EXCEPTIONS:
                    exception_skips.append(m)
                    continue
                declared_not_live.append(f"{m} -> <declared, not loadable here>")
                continue
            live_names = {n.split(".")[0] for n in live}
        declared_not_live += [f"{m} -> {n}" for n in decl_names
                              if n not in live_names]
    nblocks = sum(1 for m in exact
                  if (m.startswith("dyna:") or "*" in m)
                  and declared_runtime_names(exact[m]))
    print(f"d.ts->live: runtime decls checked in {nblocks} module blocks")
    print(f"DECLARED NOT LIVE (d.ts -> runtime): {len(declared_not_live)}")
    for x in declared_not_live[:60]:
        print("  " + x)
    if len(declared_not_live) > 60:
        print(f"  ... and {len(declared_not_live) - 60} more")
    if exception_skips:
        print(f"d.ts->live skipped via EXCEPTIONS: {len(exception_skips)}")
        for m in exception_skips:
            print(f"  {m}: {EXCEPTIONS[m]}")

    # ext:*: the d.ts augments globals via TOP-LEVEL interfaces. Standard ES
    # names are in TypeScript's own lib; only non-standard extensions matter.
    # The ext rows come from tools/api-inventory.js, whose ext enumeration is
    # reliable (the import-order bug only bites module probes).
    # The standard/non-standard classification needs the test262 checkout
    # (api-inventory scans its built-ins directories). Linux CI legs exclude
    # test262 by design (265 MB), and there EVERY name reads non-standard --
    # 214 false "missing". Skip this ONE section loudly rather than fail.
    have_t262 = os.path.isdir("test262/test/built-ins")
    if not have_t262:
        print("ext top-level coverage: SKIPPED -- test262/ absent "
              "(standard-vs-extension classification needs it)")
    inv = json.loads(subprocess.run(
        ["./dynajs", "--std", "tools/api-inventory.js", "--json"],
        capture_output=True, text=True).stdout)
    top_level = re.sub(r'declare module "dyna:[a-z0-9]+" \{.*?\n\}', "", src, flags=re.S)
    IN_STANDARD_LIB = {
        "String.prototype.substr", "String.prototype.trimRight", "String.prototype.trimLeft",
        "String.prototype.anchor", "String.prototype.big", "String.prototype.blink",
        "String.prototype.bold", "String.prototype.fixed", "String.prototype.fontcolor",
        "String.prototype.fontsize", "String.prototype.italics", "String.prototype.link",
        "String.prototype.small", "String.prototype.strike", "String.prototype.sub",
        "String.prototype.sup",
        "Number.NaN", "Number.EPSILON", "Number.MAX_SAFE_INTEGER", "Number.MIN_SAFE_INTEGER",
        "RegExp.prototype.compile",
    }
    ext_missing = []
    ext_total = 0
    for e in inv["entries"]:
        if not e["module"].startswith("ext:") or e.get("standard"):
            continue
        if not have_t262:
            continue
        ext_total += 1
        s = e["symbol"]
        if s in IN_STANDARD_LIB:
            continue
        base = s.split(".")[0]
        meth = s.split(".")[-1]
        if not re.search(r"interface " + base + r"(?:Constructor)?\b[\s\S]{0,20000}?"
                         + re.escape(meth) + r"\b", top_level):
            ext_missing.append(s)
    print(f"non-standard ext symbols: {ext_total}")
    print(f"MISSING from top-level interfaces: {len(ext_missing)}")
    for x in ext_missing[:60]:
        print("  " + x)

    bad = missing_mods or missing_syms or ext_missing or declared_not_live
    sys.exit(1 if bad else 0)

if __name__ == "__main__":
    main()
