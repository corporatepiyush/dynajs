#!/usr/bin/env python3
"""Cross-check types/dynajs.d.ts against the binary's own exports.

Enumerates every dyna:* module the binary can load, EACH IN A FRESH PROCESS.
That matters: importing dyna:net after dyna:http fails in one process (a known
engine import-order cycle), which is exactly how tools/api-inventory.js once
reported dyna:net as "missing" and the whole module went undeclared.
"""
import json, re, subprocess, sys

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
    src = open("types/dynajs.d.ts").read()
    pat = re.compile(r'\ndeclare module "((?:dyna:|ext:)[a-z0-9]+)" \{')
    blocks = pat.split(src)
    mods_text = {}
    for i in range(1, len(blocks), 2):
        mods_text[blocks[i]] = blocks[i + 1]

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

    # ext:*: the d.ts augments globals via TOP-LEVEL interfaces. Standard ES
    # names are in TypeScript's own lib; only non-standard extensions matter.
    # The ext rows come from tools/api-inventory.js, whose ext enumeration is
    # reliable (the import-order bug only bites module probes).
    inv = json.loads(subprocess.run(
        ["./dynajs", "tools/api-inventory.js", "--json"],
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

    bad = missing_mods or missing_syms or ext_missing
    sys.exit(1 if bad else 0)

if __name__ == "__main__":
    main()
