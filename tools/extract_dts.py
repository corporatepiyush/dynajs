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

raw_src = open(sys.argv[1] if len(sys.argv) > 1 else "dynajs.d.ts").read()

# strip /* */ comments and // comments so doc text cannot inject decl-shaped lines
src = re.sub(r"/\*.*?\*/", "", raw_src, flags=re.S)
src = re.sub(r"//[^\n]*", "", src)

TLS_TOKENS = re.compile(
    r"\bthe\s+([A-Za-z0-9_*/]+(?:/[A-Za-z0-9_*/]+)*)\s+(functions|classes|namespaces)",
    re.I)

# `/** Windows only; absent on POSIX builds. */ const X` -- the doc marker the
# d.ts uses for platform-gated consts (os.O_BINARY/O_TEXT). They belong in the
# win32_consts bucket so the probe run only demands them on Windows.
WIN32_CONST = re.compile(
    r"/\*\*[^*]*Windows only[^*]*\*/\s*\n\s*const\s+([A-Za-z_$][\w$]*)")


def gated_platform(raw, mod):
    """True when the // comment directly above `declare module "<mod>"`
    gates the module to platforms this probe cannot assume (the dyna:uring
    form: 'Linux + CONFIG_IO_URING=y builds only; the module does not exist
    otherwise')."""
    i = raw.find('declare module "%s"' % mod)
    if i < 0:
        return False
    for ln in reversed(raw[:i].splitlines()):
        s = ln.strip()
        if s.startswith("//"):
            return "does not exist otherwise" in s
        if s:
            return False
    return False


def mask_non_members(cbody):
    """Blank out everything that is NOT a top-level class member.

    Object-literal keys inside a constructor's opts type (Fetcher's
    `agent: string`) or inside a method's return-type literal (stats()'s
    `fetched: number`) otherwise read as members and the table drifts from
    the runtime. Everything inside the constructor's parens, or at brace
    depth != 1, is replaced with spaces (line structure preserved so the
    ^-anchored member regexes still see their line starts).
    """
    out = list(cbody)
    paren = brace = 0
    for i, ch in enumerate(cbody):
        if ch == "(":
            paren += 1
        elif ch == ")":
            paren -= 1
        elif ch == "{":
            brace += 1
        elif ch == "}":
            brace -= 1
        # keep only top-level text: outside parens (constructor signatures)
        # and outside nested type literals (return-type objects); the char
        # itself stays visible at top level
        if paren == 0 and brace == 0:
            continue
        if ch not in "\n(":          # keep the ( a method name is keyed on
            out[i] = " "
    return "".join(out)


def tls_gated_tokens(raw, mod, kind):
    """Names listed as CONFIG_TLS-gated for `mod` and one kind.

    Walks the // comment lines immediately above `declare module "<mod>"`;
    returns [] unless one says "CONFIG_TLS=y builds". Token groups look like
    "the Ed25519*/X25519*/Scrypt functions".
    """
    i = raw.find('declare module "%s"' % mod)
    if i < 0:
        return []
    comment = []
    for ln in reversed(raw[:i].splitlines()):
        s = ln.strip()
        if s.startswith("//"):
            comment.append(s[2:].strip())
        elif s == "" and not comment:
            continue
        else:
            break
    text = " ".join(reversed(comment))
    if "CONFIG_TLS=y builds" not in text:
        return []
    out = []
    for tokens, k in TLS_TOKENS.findall(text):
        if k != kind:
            continue
        out += tokens.split("/")
    return out

modules = {}
for m in re.finditer(r'declare module "([^"]+)"\s*\{', src):
    name = m.group(1)
    if "*" in name:
        # Wildcard FILE-PATH modules (src/pool.js, imported relatively) are
        # not importable by this pattern string, so a child-process probe
        # `import * from "*/src/pool.js"` could never resolve. Their surface
        # is verified by tools/check-dts-coverage.py (direction 2) instead.
        continue
    if gated_platform(raw_src, name):
        # Platform-gated modules (dyna:uring: "Linux + CONFIG_IO_URING=y
        # builds only; the module does not exist otherwise") must not enter
        # the probe table: on a build without them the import probe would
        # fail. The test asserts their absence separately.
        continue
    # brace-match the block
    depth, i = 1, m.end()
    start = i
    while depth and i < len(src):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
        i += 1
    modules[name] = modules.get(name, "") + "\n" + src[start : i - 1]

surface = {}
win32_names = set(WIN32_CONST.findall(raw_src))
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
        cbody = mask_non_members(cbody)
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
    # ---- CONFIG_TLS-gated names -------------------------------------------
    # A `// ... exist only in CONFIG_TLS=y builds` comment directly above a
    # block lists the gated functions/classes/namespaces (slash-separated,
    # trailing * = prefix wildcard, e.g. "Ed25519*/X25519*/Scrypt"). Those
    # names move into tls_* buckets so a no-TLS probe run neither demands
    # them (forward) nor reports them as extras (reverse). Names gated this
    # way are checked only when present (see the probe in the test).
    for key, kind in (("tls_functions", "functions"),
                      ("tls_classes", "classes"),
                      ("tls_namespaces", "namespaces")):
        bucket = []
        for tok in tls_gated_tokens(raw_src, mod, kind):
            pool = list(entry[kind])    # dict-keyed for classes/namespaces
            pref = tok[:-1] if tok.endswith("*") else tok
            for n in pool:
                if n == pref or (tok.endswith("*") and n.startswith(pref)):
                    bucket.append(n)
                    if kind in ("classes", "namespaces"):
                        del entry[kind][n]
                    else:
                        entry[kind].remove(n)
        if bucket:
            entry[key] = bucket
    # ---- Windows-only consts (see WIN32_CONST) -----------------------------
    moved = [c for c in entry["consts"] if c in win32_names]
    if moved:
        entry["consts"] = [c for c in entry["consts"] if c not in moved]
        entry["win32_consts"] = moved
    surface[mod] = entry

json.dump(surface, open(sys.argv[2] if len(sys.argv) > 2 else "surface.json", "w"), indent=1)
nmods = len(surface)
nfns = sum(len(e["functions"]) for e in surface.values())
ncls = sum(len(e["classes"]) for e in surface.values())
nmembers = sum(len(v) for e in surface.values() for v in e["classes"].values())
print(f"{nmods} modules, {nfns} functions, {ncls} classes, {nmembers} class members, {sum(len(e['consts']) for e in surface.values())} consts")
