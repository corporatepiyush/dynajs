#!/usr/bin/env python3
# gen_strings.py — black-box string/sliced-string matrix generator for DynaJS review.
#
# Phase G: emit expect_strings.js (every case evaluated under node once) + cases.jsonl
# Phase B (after node ran expect_strings.js): bake expectations into per-case probe files.
#
# Every probe asserts (portable harness h.js). Expectations come from node v22
# (oracle) at generation time; run.sh re-runs node to re-verify determinism.
import json, os, subprocess, sys, collections

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
NODE = os.environ.get("NODE_BIN", "node")
EXPECT = os.path.join(HERE, "expect_strings.js")
CASES = os.path.join(HERE, "cases_strings.jsonl")
PDIR = os.path.join(HERE, "probes", "strings")

LENGTHS = [0, 1, 2, 3, 31, 32, 62, 63, 64, 65, 66, 127, 128, 255, 256, 999, 1000, 4095, 4096, 65535, 65536, 100000]
PATTERNS = {
    "l1":   "  ab cd ef gh ij kl mn op qr st uv wx yz ",      # latin1-only
    "bmp":  " ab\u00e9cd\u4e2def\u00e9gh\u4e2d ",             # wide BMP (U+00E9, U+4E2D)
    "astr": " \U0001F600ab cd\U0001F601ef gh ",               # astral-mixed (U+1F600/U+1F601)
}
# lengths for which a derived SLICE parent exists (substring crossing the 64 threshold)
SLICEABLE = [L for L in LENGTHS if L >= 66]

def jslit(s):
    """Python str -> ASCII JS string literal (BMP chars as \\uXXXX, astral as surrogate pairs)."""
    return json.dumps(s, ensure_ascii=True)

def utf16_len(s):
    return len(s.encode("utf-16-le")) // 2

def parent_expr(width, L):
    pat = PATTERNS[width]
    plen = utf16_len(pat)  # JS .repeat/.slice count UTF-16 units, not code points
    k = (L + plen - 1) // plen + 1
    return "(%s).repeat(%d).slice(0,%d)" % (jslit(pat), k, L)

def slice_sel(width, L):
    # (A,B) so that B-A > 64 and the slice sits inside the flat parent
    if L >= 100:
        return (5, L - 6)
    return (1, L - 1)

# ---------------------------------------------------------------- op bodies
# Every body is JS code inside function(p, s, L){ ... }. `p` = flat parent,
# `s` = string under test (p for flat probes, p.substring(A,B) for slice probes).
OPS = []  # (opid, body, condition)
def op(oid, body, cond=None):
    OPS.append((oid, body, cond))

PROLOG = "var n2 = String.fromCharCode(p.charCodeAt(9), p.charCodeAt(10));\n"

op("len",        "return s.length;")
op("idx0",       "return s[0];")
op("idxMid",     "return s[s.length >> 1];")
op("idxLast",    "return s[s.length - 1];")
op("idxOob",     "return s[s.length + 7];")
op("idxNeg",     "return s[-1];")
op("charAt0",    "return s.charAt(0);")
op("charAt1",    "return s.charAt(1);")
op("charAtLast", "return s.charAt(s.length - 1);")
op("charAtOob",  "return s.charAt(s.length + 5);")
op("charAtNeg",  "return s.charAt(-3);")
op("at0",        "return s.at(0);")
op("at1",        "return s.at(1);")
op("atNeg1",     "return s.at(-1);")
op("atNeg5",     "return s.at(-5);")
op("atLast",     "return s.at(s.length - 1);")
op("atOob",      "return s.at(s.length);")
op("cca0",       "return s.charCodeAt(0);")
op("cca5",       "return s.charCodeAt(5);")
op("ccaMid",     "return s.charCodeAt(s.length >> 1);")
op("ccaLast",    "return s.charCodeAt(s.length - 1);")
op("ccaOob",     "return s.charCodeAt(s.length + 3);")
op("ccaNeg",     "return s.charCodeAt(-1);")
op("cpa0",       "return s.codePointAt(0);")
op("cpaMid",     "return s.codePointAt(s.length >> 1);")
op("cpaLast",    "return s.codePointAt(s.length - 1);")
op("cpaOob",     "return s.codePointAt(s.length + 1);")
op("slicePos",   "return s.slice(6, 40);")
op("sliceThr",   "return s.slice(2, s.length - 3);")
op("sliceNeg",   "return s.slice(-20, -4);")
op("sliceOmit1", "return s.slice(3);")
op("sliceOmit2", "return s.slice(-15);")
op("sliceRev",   "return s.slice(30, 10);")
op("sliceOob",   "return s.slice(-1e9, 1e9);")
op("sliceFull",  "return s.slice(0);")
op("sliceZero",  "return s.slice(4, 4);")
op("sliceAllNeg","return s.slice(-1000, -900);")
op("subsPos",    "return s.substring(4, 30);")
op("subsSwap",   "return s.substring(30, 4);")
op("subsNeg",    "return s.substring(-7, 12);")
op("subsOob",    "return s.substring(10, 1e9);")
op("subsAllNeg", "return s.substring(-1e9, -5);")
op("substrPos",  "return s.substr(5, 20);")
op("substrNoLen","return s.substr(5);")
op("substrNeg",  "return s.substr(-8);")
op("substrNegLen","return s.substr(3, -2);")
op("substrOob",  "return s.substr(1e7);")
op("indexOf2",   PROLOG + "return s.indexOf(n2);")
op("indexOfFrom",PROLOG + "return s.indexOf(n2, 10);")
op("lastIndexOf2",PROLOG + "return s.lastIndexOf(n2);")
op("includes2",  PROLOG + "return s.includes(n2);")
op("includesFrom",PROLOG + "return s.includes(n2, 15);")
op("startsWith2",PROLOG + "return s.startsWith(n2);")
op("startsWithPos",PROLOG + "return s.startsWith(n2, 8);")
op("endsWith2",  PROLOG + "return s.endsWith(n2);")
op("endsWithPos",PROLOG + "return s.endsWith(n2, s.length - 3);")
op("indexOfStraddle", "return s.indexOf(p.substring(3, 9));", cond="slice")
op("lcc",        "return s.toLowerCase();")
op("ucc",        "return s.toUpperCase();")
op("lccLocale",  "return s.toLocaleLowerCase();")
op("uccLocale",  "return s.toLocaleUpperCase();")
op("trim",       "return s.trim();")
op("trimStart",  "return s.trimStart();")
op("trimEnd",    "return s.trimEnd();")
op("padStartX",  "return s.padStart(s.length + 3, '*');")
op("padEndX",    "return s.padEnd(s.length + 3, 'ab');")
op("padStartShrink","return s.padStart(4, 'xyzzy');")
op("padEndShrink","return s.padEnd(4, 'xy');")
op("repeat2",    "return s.repeat(2);")
op("repeat0",    "return s.repeat(0);")
op("concat1",    "return s + '!!';")
op("concat2",    "return '^^' + s;")
op("concat3",    "return s + s;")
op("concatChain","return (s + '1') + (s + '2');")
op("template",   "return `x${s}y`;")
op("templateMulti", "return `a${s}b${s.length}c`;")
op("taggedRaw",  "return function (t, v) { return t.raw[0] + '|' + v; }`A\\n${s}`;")
op("jsonStr",    "return JSON.stringify(s);")
op("jsonRound",  "return JSON.parse(JSON.stringify(s));")
op("StringCtor", "return String(s);")
op("toStringM",  "return s.toString();")
op("valueOfM",   "return s.valueOf();")
op("localeCmpN2", PROLOG + "var c = s.localeCompare(n2); return (c < 0 ? -1 : c > 0 ? 1 : 0);")
op("localeCmpSelf", "var c = s.localeCompare(String(s)); return (c < 0 ? -1 : c > 0 ? 1 : 0);")
op("normNFC",    "return s.normalize('NFC');")
op("normNFD",    "return s.normalize('NFD');")
op("isWF",       "return s.isWellFormed();")
op("toWF",       "return s.toWellFormed();")
op("cpCount",    "var c = 0; for (var ch of s) c++; return c;")
op("forOfJoin",  "var o = ''; for (var ch of s) o += ch; return o;", cond="short")
op("forOfFirst3","var out = []; var it = 0; for (var ch of s) { out.push(ch); if (++it >= 3) break; } return out.join('|');")
op("splitEmpty", "return s.split('');", cond="short256")
op("splitNeedle",PROLOG + "return s.split(n2);")
op("splitLimit", PROLOG + "return s.split(n2, 2);")
op("splitSpace", "return s.split(' ');")
op("splitRe",    "return s.split(/ +/);")
op("splitReLimit","return s.split(/ +/, 3);")
op("replaceStr", PROLOG + "return s.replace(n2, '[[Z]]');")
op("replaceAllStr",PROLOG + "return s.replaceAll(n2, 'Q');")
op("replaceRe",  "return s.replace(/ +/g, '-');")
op("replaceReOnce","return s.replace(/ +/, '-');")
op("replaceFn",  PROLOG + "return s.replace(n2, function (m, o) { return '<' + o + '>'; });")
op("replaceFnRe","return s.replace(/ +/g, function (m, o) { return '[' + o + ']'; });")
op("replaceAllRe","return s.replaceAll(/ +/g, '_');")
op("replaceErr", "try { s.replaceAll(/ +/, '_'); return 'NO-THROW'; } catch (e) { return 'THREW:' + (e && e.name); }")
op("matchSimple","return s.match(/ +/);")
op("matchGlobal","return s.match(/ +/g);")
op("matchAnchored","return s.match(/^.{0,4}x/);")
op("matchAllIx", "var a = []; var it = s.matchAll(/ +/g); for (var m of it) a.push(m.index); return a.join(',');")
op("matchAllU",  "var a = []; for (var m of s.matchAll(/\\u{1F600}/gu)) a.push(m.index); return a.join(',');")
op("matchAllY",  "var a = []; var re = / +/gy; var n = 0; while (n < s.length) { var m = re.exec(s); if (!m) break; a.push(m.index); n = m.index + m[0].length; } return a.join(',');")
op("searchRe",   "return s.search(/ +/);")
op("encURI",     "return encodeURI(s);")
op("decURI",     "return decodeURI(encodeURI(s));")
op("encURIC",    "return encodeURIComponent(s);")
op("decURIC",    "return decodeURIComponent(encodeURIComponent(s));")

SHORT_OPS = {"forOfJoin"}            # full join only for tiny strings
SHORT256_OPS = {"splitEmpty"}        # split("") array only <= 256 units

def op_applicable(oid, cond, width, L, is_slice):
    if cond == "slice" and not is_slice:
        return False
    if cond == "short" and L > 16:
        return False
    if cond == "short256" and L > 256:
        return False
    if oid in ("cpaMid",) and L < 8:
        return False
    return True

# ------------------------------------------------------------ scenarios
SCEN = []  # (sid, body) — body returns value; same expectation machinery
def scen(sid, body):
    SCEN.append((sid, body))

def ladder_body(width, L, depth):
    pe = parent_expr(width, L)
    return """
var p = %s;
var cur = p;
var chain = [];
for (var i = 1; i <= %d; i++) { cur = cur.substring(2 * i, cur.length - i); chain.push(cur.length); }
var flatTwin = JSON.parse(JSON.stringify(cur));
var m = new Map(); m.set(cur, 7);
return { lens: chain[chain.length - 1], len: cur.length, cca0: cur.charCodeAt(0),
         ccaLast: cur.charCodeAt(cur.length - 1),
         twinEq: cur === flatTwin, mapGet: m.get(flatTwin), sz: m.size };
""" % (pe, depth)

for w in ("l1", "bmp", "astr"):
    for L in (1000, 4096, 65536):
        for d in range(1, 26):
            scen("ladder_%s_%d_d%02d" % (w, L, d), ladder_body(w, L, d))

def concatslice_body(w, L, k):
    pe = parent_expr(w, L)
    return """
var p = %s;
var big = p + p;
var s = big.substring(%d, %d);
var twin = JSON.parse(JSON.stringify(s));
var acc = 0;
for (var i = 0; i < s.length; i += 17) acc = (acc * 31 + s.charCodeAt(i)) | 0;
return { len: s.length, acc: acc, twinEq: s === twin, c0: s.charCodeAt(0), cl: s.charCodeAt(s.length - 1) };
""" % (pe, k, k + 200)

for w in ("l1", "bmp", "astr"):
    for L in (255, 1000, 65536):
        for k in (3, L - 100):
            scen("concslice_%s_%d_%d" % (w, L, k), concatslice_body(w, L, k))

def intern_body(w, L):
    pe = parent_expr(w, L)
    return """
var p = %s;
var s = p.substring(3, %d);
var twin = JSON.parse(JSON.stringify(s));
var m = new Map(); m.set(s, 1);
var st = new Set(); st.add(s);
var o = {}; o[s] = 42;
var sw = 'no';
switch (s) { case twin: sw = 'yes'; break; }
var m2 = new Map(); m2.set(s, 5); var del = m2.delete(twin);
return { eqTwin: s === twin, mapGet: m.get(twin), setHas: st.has(twin),
         propKey: o[twin], switchHit: sw, deleted: del, m2size: m2.size,
         neqOther: s === p };
""" % (pe, L - 7)

for w in ("l1", "bmp", "astr"):
    for L in (255, 4096):
        scen("intern_%s_%d" % (w, L), intern_body(w, L))

def weakref_body(w, L):
    pe = parent_expr(w, L)
    return """
var p = %s;
var s = p.substring(2, %d);
if (typeof WeakRef !== 'function') return 'skip-WeakRef';
var holder = { v: s };
var wr = new WeakRef(holder);
var sink = [];
for (var i = 0; i < 200000; i++) { sink.push('g' + (i %% 97)); if (sink.length > 500) sink.length = 0; }
var h2 = wr.deref();
return { alive: h2 === holder, kept: !!h2 && h2.v === s, len: s.length };
""" % (pe, L - 4)

for w in ("l1", "bmp", "astr"):
    for L in (1000, 65536):
        scen("weakref_%s_%d" % (w, L), weakref_body(w, L))

def errbody(w, L):
    pe = parent_expr(w, L)
    return """
var p = %s;
var s = p.substring(4, %d);
var e = new Error(s);
var e2;
try { null.x; } catch (x) { e2 = x; }
return { msg: e.message, stackIsString: typeof e.stack === 'string', stackLen: e.stack.length > 0,
         typeErrMsg: e2.message, typeErrStack: typeof e2.stack === 'string' };
""" % (pe, L - 9)

for w in ("l1", "bmp", "astr"):
    for L in (128, 4096):
        scen("errmsg_%s_%d" % (w, L), errbody(w, L))

def reidx_body(w, kind):
    pe = parent_expr(w, 4096)
    if kind == "g":
        return """
var p = %s;
var s = p.substring(6, 4000);
var re = /cd/g; var out = [];
while (true) { var m = re.exec(s); if (!m) break; out.push(m.index); if (re.lastIndex === m.index) re.lastIndex++; }
return out.join(',');
""" % pe
    return """
var p = %s;
var s = p.substring(6, 4000);
var re = /ab/y; var out = []; var pos = 0;
while (pos <= s.length) { re.lastIndex = pos; var m = re.exec(s); if (!m) { pos++; continue; } out.push(m.index); pos = m.index + 1; }
return out.join(',');
""" % pe

for w in ("l1", "bmp", "astr"):
    for kind in ("g", "y"):
        scen("relastidx_%s_%s" % (w, kind), reidx_body(w, kind))

def rawuri_body(w):
    pe = parent_expr(w, 4096)
    return """
var p = %s;
var s = p.substring(3, 500);
var tagged = function (t, v) { return t.raw.join('~') + '#' + v.length; };
var r = tagged`pre\\n${s}post`;
return { raw: r, enc: encodeURI(s).length, dec: decodeURI(encodeURI(s)) === s };
""" % pe

for w in ("l1", "bmp", "astr"):
    scen("rawuri_%s" % w, rawuri_body(w))

def jsonslice_body(w):
    pe = parent_expr(w, 4096)
    return """
var p = %s;
var s = p.substring(8, 900);
var obj = {};
obj[s] = 'v';
var parsed = JSON.parse('"' + JSON.stringify(s).slice(1, -1) + '"');
return { keyGet: obj[s], roundtrip: parsed === s, inObj: JSON.stringify({ a: s }).length > s.length };
""" % pe

for w in ("l1", "bmp", "astr"):
    scen("jsonslice_%s" % w, jsonslice_body(w))

# T2 documented cap: "x".repeat(2**30) throws RangeError (documented dynajs divergence T2;
# node v22 also throws RangeError with different message text -> name-level assert both sides).
scen("t2_repeat_cap", """
var threw = null;
try { var s = 'x'.repeat(1073741824); } catch (e) { threw = e; }
if (!threw) return 'NO-THROW';
return { name: threw.name };
""")
scen("t2_repeat_cap_big", """
var threw = null;
try { var s = 'x'.repeat(2147483648); } catch (e) { threw = e; }
if (!threw) return 'NO-THROW';
return { name: threw.name };
""")

def emit_cases():
    cases = []
    for w in PATTERNS:
        for L in LENGTHS:
            pe = parent_expr(w, L)
            for (oid, body, cond) in OPS:
                if not op_applicable(oid, cond, w, L, False):
                    continue
                cid = "F_%s_%d_%s" % (w, L, oid)
                fn = "function () { var p = %s; var s = p; %s }" % (pe, body)
                cases.append((cid, fn))
        for L in SLICEABLE:
            pe = parent_expr(w, L)
            (A, B) = slice_sel(w, L)
            for (oid, body, cond) in OPS:
                if not op_applicable(oid, cond, w, L, True):
                    continue
                cid = "S_%s_%d_%s" % (w, L, oid)
                fn = "function () { var p = %s; var s = p.substring(%d, %d); %s }" % (pe, A, B, body)
                cases.append((cid, fn))
    for (sid, body) in SCEN:
        fn = "function () { %s }" % body
        cases.append(("SC_" + sid, fn))
    return cases

def write_expect_and_cases(cases):
    with open(CASES, "w") as f:
        for (cid, fn) in cases:
            f.write(json.dumps([cid, fn]) + "\n")
    with open(EXPECT, "w") as f:
        f.write("// generated by gen_strings.py — DO NOT EDIT; expectation oracle (node)\n")
        with open(os.path.join(HERE, "h.js")) as h:
            f.write(h.read())
        f.write("\nvar __cases = [\n")
        for (cid, fn) in cases:
            f.write("[%s, %s],\n" % (json.dumps(cid), fn))
        f.write("];\nfor (var i = 0; i < __cases.length; i++) {\n"
                "  var cid = __cases[i][0], fn = __cases[i][1];\n"
                "  var r;\n"
                "  try { r = __norm(fn()); }\n"
                "  catch (e) {\n"
                "    var n = (e && typeof e.name === 'string') ? e.name : 'Throw';\n"
                "    r = { t: 'e', n: n };\n"
                "  }\n"
                "  __out(JSON.stringify([cid, r]));\n"
                "}\n")

def bake(expfile):
    exps = {}
    with open(expfile) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            (cid, val) = json.loads(line)
            exps[cid] = val
    os.makedirs(PDIR, exist_ok=True)
    with open(os.path.join(HERE, "cases_strings.jsonl")) as f:
        cases = [json.loads(x) for x in f if x.strip()]
    n = 0
    for (cid, fn) in cases:
        if cid not in exps:
            print("MISSING expectation for", cid, file=sys.stderr)
            sys.exit(2)
        want = json.dumps(exps[cid], ensure_ascii=True)
        path = os.path.join(PDIR, cid + ".js")
        with open(path, "w") as g:
            g.write("// GENERATED probe (gen_strings.py) — black-box string matrix\n")
            with open(os.path.join(HERE, "h.js")) as h:
                g.write(h.read())
            g.write("\ncase_eq(%s, %s, %s);\nsummary(%s);\n" % (json.dumps(cid), want, fn, json.dumps(cid)))
        n += 1
    return n

def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--bake":
        print("baked", bake(sys.argv[2]), "probes")
        return
    cases = emit_cases()
    write_expect_and_cases(cases)
    print("cases:", len(cases))

if __name__ == "__main__":
    main()
