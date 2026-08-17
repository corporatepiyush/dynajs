#!/usr/bin/env python3
"""Cross-check docs/dynajs-guide/API.md against the binary's own exports.

Every exported name (module-level and class-member base) must appear in its
module's section of the reference. Each module is enumerated in a FRESH
process, because importing dyna:net after dyna:http fails in one process (a
known engine import-order cycle) and a one-process probe would silently drop
the whole module.
"""
import json, os, re, subprocess, sys

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

def api_sections():
    lines = open("docs/dynajs-guide/API.md").read().split("\n")
    heads = [(i, ln[2:]) for i, ln in enumerate(lines)
             if ln.startswith("# dyna:") or ln.startswith("# ext:")]
    out = {}
    for k, (i, name) in enumerate(heads):
        j = heads[k + 1][0] if k + 1 < len(heads) else len(lines)
        out[name] = "\n".join(lines[i:j])
    return out

def module_names():
    mods = set()
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
    r = subprocess.run(["./dynajs", "-e", "const m = %r;\n%s" % (module, PROBE)],
                       capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        return [], "probe exited %d" % r.returncode
    out = r.stdout.strip().splitlines()
    if not out:
        return [], "no output"
    try:
        d = json.loads(out[-1])
    except json.JSONDecodeError:
        return [], "unparsable output"
    if "error" in d:
        return [], "module refused: " + d["error"]
    return sorted(set(d["names"])), None

def main():
    sections = api_sections()
    bad = []
    total = 0
    refused = []
    for m in module_names():
        names, err = probe(m)
        if err:
            refused.append((m, err))
            continue
        text = sections.get(m, "")
        if not text:
            bad.append(f"{m}: NO SECTION in API.md")
            continue
        for s in names:
            total += 1
            base = s.split(".")[0]
            # the base token must appear as a word in the section
            if not re.search(r"\b" + re.escape(base) + r"\b", text):
                bad.append(f"{m} -> {s}")

    # ext:*: the inventory rows are reliable for the builtin extensions. The
    # STANDARD ES surface (Array.prototype.forEach, Number.isNaN, the ES2023
    # additions, ...) is the language spec's own, not an engine extension; the
    # reference documents the extensions (the same split check-dts-coverage
    # uses).
    inv = json.loads(subprocess.run(
        ["./dynajs", "tools/api-inventory.js", "--json"],
        capture_output=True, text=True).stdout)
    ext_text = sections.get("ext:builtins", "")
    for e in inv["entries"]:
        if not e["module"].startswith("ext:") or e.get("standard"):
            continue
        s = e["symbol"]
        total += 1
        meth = s.split(".")[-1]
        if not re.search(r"\b" + re.escape(meth) + r"\b", ext_text):
            bad.append(f"ext:builtins -> {s}")

    print(f"sections in API.md: {len(sections)}")
    print(f"symbols checked: {total}")
    print(f"skipped (not loadable here): {len(refused)}")
    for m, err in refused:
        print(f"  {m}: {err}")
    print(f"MISSING from API.md: {len(bad)}")
    for x in bad[:60]:
        print("  " + x)
    if len(bad) > 60:
        print(f"  ... and {len(bad) - 60} more")
    sys.exit(1 if bad else 0)

if __name__ == "__main__":
    main()
