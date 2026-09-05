#!/usr/bin/env python3
"""Extract declared API surface from dynajs.d.ts and emit a JSON table.

Captures per `declare module "dyna:x"` block:
  - top-level functions:  function name(...)
  - top-level consts:     const NAME
  - classes + their members (methods, getters, statics, ctor)
  - enums + names
Comment-only lines, overloads (repeat names) dedupe.
"""
import json
import re
import sys

src = open(sys.argv[1] if len(sys.argv) > 1 else "dynajs.d.ts").read()

# strip /* */ comments and // comments so doc text cannot inject decl-shaped lines
src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
src = re.sub(r"//[^\n]*", "", src)

modules = {}
for m in re.finditer(r'declare module "([^"]+)"\s*\{', src):
    name = m.group(1)
    # brace-match the block
    depth, i = 1, m.end()
    start = i
    while depth and i < len(src):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
        i += 1
    modules[name] = src[start : i - 1]

surface = {}
for mod, body in modules.items():
    entry = {"functions": [], "consts": [], "classes": {}, "enums": [],
             "namespaces": {}}
    for fn in re.finditer(r"^\s{4}function\s+([A-Za-z_$][\w$]*)", body, re.M):
        if fn.group(1) not in entry["functions"]:
            entry["functions"].append(fn.group(1))
    for c in re.finditer(r"^\s{4}const\s+([A-Za-z_$][\w$]*)", body, re.M):
        if c.group(1) not in entry["consts"]:
            entry["consts"].append(c.group(1))
    # export { X as in } alias lines: only the exported name is runtime-visible
    alias_locals = set()
    for al in re.finditer(r"^\s{4}export\s*\{([^}]*)\}", body, re.M):
        for part in al.group(1).split(","):
            part = part.strip()
            if not part:
                continue
            if " as " in part:
                alias_locals.add(part.split(" as ", 1)[0].strip())
            nm = part.split(" as ", 1)[1].strip() if " as " in part else part
            if nm not in entry["consts"]:
                entry["consts"].append(nm)
    entry["consts"] = [c for c in entry["consts"] if c not in alias_locals]
    for ns_m in re.finditer(r"^\s{4}namespace\s+([A-Za-z_$][\w$]*)\s*\{", body, re.M):
        depth, i = 1, ns_m.end()
        while depth and i < len(body):
            if body[i] == "{":
                depth += 1
            elif body[i] == "}":
                depth -= 1
            i += 1
        nbody = body[ns_m.end() : i - 1]
        nsmembers = []
        for f2 in re.finditer(r"^\s{8}function\s+([A-Za-z_$][\w$]*)", nbody, re.M):
            if f2.group(1) not in nsmembers:
                nsmembers.append(f2.group(1))
        for v2 in re.finditer(r"^\s{8}(?:const|readonly)\s+([A-Za-z_$][\w$]*)", nbody, re.M):
            if v2.group(1) not in nsmembers:
                nsmembers.append(v2.group(1))
        entry["namespaces"][ns_m.group(1)] = nsmembers
    for e in re.finditer(r"^\s{4}(?:export\s+)?enum\s+([A-Za-z_$][\w$]*)", body, re.M):
        entry["enums"].append(e.group(1))
    for cl in re.finditer(
        r"^\s{4}(?:export\s+)?(?:abstract\s+)?class\s+([A-Za-z_$][\w$]*)[^\n{]*\{",
        body,
        re.M,
    ):
        cname = cl.group(1)
        inherits = "DynResource" in body[cl.start() : cl.end()]
        depth, i = 1, cl.end()
        while depth and i < len(body):
            if body[i] == "{":
                depth += 1
            elif body[i] == "}":
                depth -= 1
            i += 1
        cbody = body[cl.end() : i - 1]
        members = []
        if inherits:
            members += ["close", "dispose", "closed", "[Symbol.dispose]"]
        for mm in re.finditer(
            r"^\s+(?:static\s+)?(?:get\s+|readonly\s+)?([A-Za-z_$][\w$]*)\s*[(:<=]", cbody, re.M
        ):
            n = mm.group(1)
            if n not in ("constructor",) and n not in members:
                members.append(n)
        # getters like: get foo(): T   (already caught by [(:<=)? no — 'foo(' catches)
        for mm in re.finditer(r"^\s+get\s+([A-Za-z_$][\w$]*)", cbody, re.M):
            if mm.group(1) not in members:
                members.append(mm.group(1))
        entry["classes"][cname] = members
    surface[mod] = entry

json.dump(surface, open(sys.argv[2] if len(sys.argv) > 2 else "/tmp/surface.json", "w"), indent=1)
nmods = len(surface)
nfns = sum(len(e["functions"]) for e in surface.values())
ncls = sum(len(e["classes"]) for e in surface.values())
nmembers = sum(len(v) for e in surface.values() for v in e["classes"].values())
print(f"{nmods} modules, {nfns} functions, {ncls} classes, {nmembers} class members, {sum(len(e['consts']) for e in surface.values())} consts")
