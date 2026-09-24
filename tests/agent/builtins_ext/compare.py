#!/usr/bin/env python3
# compare.py — byte-compare engine transcripts with TAG-AWARE whitelist.
# A differing line is tolerated ONLY if BOTH lines carry a whitelisted tag.
# Tag semantics (pair-scoped):
#   node pair (tree vs node)  allows: LMT WALL SIGN R36 LOC TOSTR NAN POW BIN
#   base pair (tree vs baseline) allows: BIN only   (semantics must be
#     pre/post-change identical; BIN is the deliberately preserved Grisu
#     binade defect, absent in the pre-change baseline)
import sys

NODE_TAGS = {"LMT", "WALL", "SIGN", "R36", "LOC", "TOSTR", "NAN", "POW", "BIN"}
BASE_TAGS = {"BIN"}

def tags_of(line):
    return {t for t in ("LMT", "WALL", "SIGN", "R36", "LOC", "TOSTR", "NAN", "POW", "BIN")
            if ("[" + t + "]") in line}

def compare(path_t, path_o, allowed):
    with open(path_t, "rb") as f: tb = f.read()
    with open(path_o, "rb") as f: ob = f.read()
    if tb == ob:
        return True, []
    tl = tb.decode("utf-8", "replace").splitlines()
    ol = ob.decode("utf-8", "replace").splitlines()
    if len(tl) != len(ol):
        return False, ["LINE-COUNT %d vs %d" % (len(tl), len(ol))]
    bad = []
    for i, (a, b) in enumerate(zip(tl, ol)):
        if a == b:
            continue
        common = tags_of(a) & tags_of(b)
        if a.strip() and b.strip() and (tags_of(a) & allowed) and (tags_of(b) & allowed) and common:
            continue
        if common & allowed:
            continue
        bad.append("line %d:\n  T: %s\n  O: %s" % (i + 1, a[:220], b[:220]))
        if len(bad) >= 5:
            break
    return (len(bad) == 0), bad

def main():
    # args: mode tree_out other_out pair   mode: full|verdict
    mode, pt, po, pair = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
    allowed = NODE_TAGS if pair == "node" else BASE_TAGS
    if mode == "verdict":
        with open(pt) as f: vt = [l for l in f.read().splitlines() if l.startswith("VERDICT:")]
        with open(po) as f: vo = [l for l in f.read().splitlines() if l.startswith("VERDICT:")]
        if vt == vo:
            print("OK")
        else:
            print("DIFF verdict lines:\n  T: %s\n  O: %s" % (vt[:6], vo[:6]))
        return
    ok, bad = compare(pt, po, allowed)
    print("OK" if ok else "DIFF " + " || ".join(bad))

main()
