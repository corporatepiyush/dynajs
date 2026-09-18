import re, sys, collections
def slug(t):
    """GitHub anchors: lowercase, drop punctuation EXCEPT hyphen and
    underscore, spaces to hyphens. Stripping `_` reported io_uring links as
    broken and would have had me 'fix' a correct one."""
    t = t.strip().lower()
    t = re.sub(r"[`*\[\]()<>.,:;!?'\"/\\|+=~^{}$@#%&]", "", t)
    t = re.sub(r"[–—]", "", t)
    return t.replace(" ", "-")
bad_total = 0
for p in sys.argv[1:]:
    L = open(p).read().split("\n")
    seen = collections.Counter(); anchors = set(); fence = False
    for ln in L:
        if ln.strip().startswith("```"): fence = not fence; continue
        if fence: continue
        m = re.match(r"^(#{1,6}) +(.*)$", ln)
        if not m: continue
        a = slug(m.group(2)); n = seen[a]; seen[a] += 1
        anchors.add(a if n == 0 else f"{a}-{n}")
    bad = [(i, t) for i, ln in enumerate(L, 1)
           for t in re.findall(r"\]\(#([^)]+)\)", ln) if t not in anchors]
    bad_total += len(bad)
    print(f"{p}: {len(anchors)} anchors, {len(bad)} broken")
    for i, t in bad: print(f"    {p}:{i}  #{t}")
sys.exit(1 if bad_total else 0)
