#!/usr/bin/env python3
# gen_dataframe.py -- emit black-box probes for dyna:dataframe.
#
# ORACLE POLICY: python dicts + numpy are the reference. Reductions are exact
# closed forms over float64 (documented NaN policy: SUM skips NaN (0 when
# empty), MEAN -> NaN when nothing survives, MIN/MAX ignore NaN (undefined on
# empty)). Group-by is a python dict groupby. Null policy: a numeric NaN is
# THE null of a numeric column; a string column has no null ("" is a value).
import os, json
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
PROBES = os.path.join(HERE, "..", "probes", "df")
HARNESS = os.path.join(HERE, "..", "h.js")

def lcg(seed):
    s = seed & 0xFFFFFFFF
    while True:
        s = (s * 1664525 + 1013904223) & 0xFFFFFFFF
        yield s

def jsnum(v):
    """JS-safe numeric literal (repr(float('nan')) is the bare word 'nan',
    which is a ReferenceError in JS, not NaN)."""
    v = float(v)
    if v != v:
        return "NaN"
    if v == float("inf"):
        return "Infinity"
    if v == float("-inf"):
        return "-Infinity"
    return repr(v)

class ProbeWriter:
    def __init__(self, path, imports, tag):
        self.path = path
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self.f = open(path, "w")
        self.f.write("// GENERATED probe (%s) -- do not edit; run materialize.sh\n" % tag)
        for imp in imports:
            self.f.write(imp + "\n")
        self.f.write('var __TAG = "%s";\n' % tag)
        self.f.write(open(HARNESS).read().replace("//dyna_num_harness_marker", ""))
    def w(self, line=""):
        self.f.write(line + "\n")
    def close(self, total):
        self.f.write('summary(__TAG); // %d cases\n' % total)
        self.f.close()
        print("wrote %s (%d cases)" % (self.path, total))

def gen_reductions():
    tag = "df_reductions"
    p = ProbeWriter(os.path.join(PROBES, tag + ".js"),
                    ['import { DataFrame } from "dyna:dataframe";'], tag)
    rng = lcg(7001)
    EMIT = []
    n = 0
    for trial in range(8):
        m = [0, 1, 2, 5, 17, 64, 100][trial % 7]
        xs = np.array([next(rng) / 4294967296.0 * 20 - 10 for _ in range(m)])
        # plant NaNs
        if m > 3:
            xs[1] = np.nan
        if m > 10:
            xs[10] = np.nan
        ids = [int(next(rng) % 100) for _ in range(m)]
        mask = [1] * m
        if m > 6:
            mask[2] = 0
            mask[m - 1] = 0
        # oracle aligned with API.md + engine (verified 2026-09-12):
        # SUM/MEAN/VARIANCE propagate NaN (no NaN-skip is documented), MIN/MAX
        # ignore NaN (undefined on empty), COUNT counts ALL selected rows,
        # VARIANCE is sample (n-1, NaN under 2 rows), VARIANCE_POP is /n from
        # one row up.
        v = xs[np.array(mask) == 1]
        vv = v[~np.isnan(v)]
        has_nan = len(vv) != len(v)
        ssum = float("nan") if has_nan else (float(v.sum()) if len(v) else 0.0)
        smean = float("nan") if (has_nan or not len(v)) else float(vv.mean())
        smin = float(vv.min()) if len(vv) else None
        smax = float(vv.max()) if len(vv) else None
        cnt = int(len(v))
        var = float("nan") if (has_nan or len(vv) < 2) else float(vv.var(ddof=1))
        var1 = var
        var_pop = float("nan") if (has_nan or not len(vv)) else float(vv.var(ddof=0))
        EMIT.append('var df%d = new DataFrame({ x: new Float64Array([%s]), id: new Int32Array([%s]) });'
                    % (trial, ",".join(jsnum(x) for x in xs), ",".join(map(str, ids))))
        EMIT.append('var mk%d = new Uint8Array([%s]);' % (trial, ",".join(map(str, mask))))
        EMIT.append('assert_close(df%d.SUM("x", mk%d), %s, 1e-12, 1e-12, "SUM t%d");' % (trial, trial, jsnum(ssum), trial))
        EMIT.append('assert_close(df%d.MEAN("x", mk%d), %s, 1e-12, 1e-12, "MEAN t%d");' % (trial, trial, jsnum(smean), trial))
        EMIT.append('assert_close(df%d.VARIANCE_POP("x", mk%d), %s, 1e-11, 1e-12, "VARIANCE_POP t%d");' % (trial, trial, jsnum(var_pop), trial))
        if len(vv) >= 1:
            EMIT.append('assert_close(df%d.VARIANCE("x", mk%d), %s, 1e-11, 1e-12, "VARIANCE t%d");'
                        % (trial, trial, jsnum(var1), trial))
        if smin is None:
            EMIT.append('assert_eq(df%d.MIN("x", mk%d), undefined, "MIN empty t%d");' % (trial, trial, trial))
        else:
            EMIT.append('assert_close(df%d.MIN("x", mk%d), %r, 1e-12, 0, "MIN t%d");' % (trial, trial, smin, trial))
        if smax is None:
            EMIT.append('assert_eq(df%d.MAX("x", mk%d), undefined, "MAX empty t%d");' % (trial, trial, trial))
        else:
            EMIT.append('assert_close(df%d.MAX("x", mk%d), %r, 1e-12, 0, "MAX t%d");' % (trial, trial, smax, trial))
        EMIT.append('assert_eq(df%d.COUNT("x", mk%d), %d, "COUNT all selected rows t%d");' % (trial, trial, cnt, trial))
        # int column: SUM_CHECKED exact
        EMIT.append('assert_eq(df%d.SUM("id", mk%d), %d, "SUM int exact t%d");'
                    % (trial, trial, int(np.array(ids)[np.array(mask) == 1].sum()), trial))
        n += 8
    # integer sums that overflow float exactness still exact via SUM_CHECKED
    big = [2**31 - 1] * 4
    EMIT.append('var db = new DataFrame({ id: new Int32Array([%s]) });' % ",".join(map(str, big)))
    EMIT.append('assert_eq(db.SUM_CHECKED("id"), %d, "SUM_CHECKED big");' % sum(big))
    n += 1
    # NaN policy: all-NaN column (engine verified 2026-09-12: no documented
    # NaN-skip for SUM -> IEEE NaN; MIN/MAX skip NaNs to their identities,
    # which differs from the EMPTY selection case that gives undefined)
    EMIT.append('var dn = new DataFrame({ x: new Float64Array([NaN, NaN]) });')
    EMIT.append('assert_nan(dn.SUM("x"), "SUM all-NaN is NaN");')
    EMIT.append('assert_nan(dn.MEAN("x"), "MEAN all-NaN is NaN");')
    EMIT.append('assert_close(dn.MIN("x"), Infinity, 0, 0, "MIN all-NaN +Inf identity");')
    EMIT.append('assert_close(dn.MAX("x"), -Infinity, 0, 0, "MAX all-NaN -Inf identity");')
    EMIT.append('assert_eq(dn.COUNT("x"), 2, "COUNT counts rows not non-NaN");')
    EMIT.append('assert_eq(dn.COUNT_NULLS("x"), 2, "COUNT_NULLS");')
    n += 6
    for line in EMIT:
        p.w(line)
    p.close(n)

def gen_groupby():
    tag = "df_groupby"
    p = ProbeWriter(os.path.join(PROBES, tag + ".js"),
                    ['import { DataFrame } from "dyna:dataframe";'], tag)
    rng = lcg(7002)
    EMIT = []
    n = 0
    for trial in range(6):
        m = [1, 2, 5, 12, 40, 64][trial]
        keys = ["a", "b", "c"][: 2 + trial % 2]
        cat = [keys[next(rng) % len(keys)] for _ in range(m)]
        xs = [float(next(rng) % 100) for _ in range(m)]
        mask = [1] * m
        if m > 4:
            mask[0] = 0
        # oracle
        groups = {}
        order = []
        for i in range(m):
            if not mask[i]:
                continue
            if cat[i] not in groups:
                groups[cat[i]] = []
                order.append(cat[i])
            groups[cat[i]].append(xs[i])
        sums = [sum(groups[k]) for k in order]
        means = [sum(groups[k]) / len(groups[k]) for k in order]
        mins = [min(groups[k]) for k in order]
        maxs = [max(groups[k]) for k in order]
        counts = [len(groups[k]) for k in order]
        EMIT.append('var g%d = new DataFrame({ cat: %s, x: new Float64Array([%s]) });'
                    % (trial, json.dumps(cat), ",".join(map(str, xs))))
        EMIT.append('var mk%d = new Uint8Array([%s]);' % (trial, ",".join(map(str, mask))))
        # Key ORDER under a mask is not specified by the docs: the engine
        # discovers keys over ALL rows (first-seen incl. masked rows) and
        # accumulates only contributing rows (verified 2026-09-12). Unmasked
        # frames keep exact first-seen order; masked frames get order-free
        # per-key asserts.
        agg = {"sum": sums, "mean": means, "min": mins, "max": maxs}
        if any(not mk for mk in mask):
            EMIT.append('var gb%d = g%d.GROUP_BY_SUM("cat", "x", mk%d);'
                        ' assert_eq(gb%d.keys.length, %d, "gb key count t%d");'
                        % (trial, trial, trial, trial, len(order), trial))
            for ki, k in enumerate(order):
                EMIT.append('var gi%d_%d = gb%d.keys.indexOf(%s);'
                            ' assert_true(gi%d_%d >= 0, "gb key %s present t%d");'
                            % (trial, ki, trial, json.dumps(k), trial, ki, k, trial))
                for nm in ("sum", "mean", "min", "max"):
                    EMIT.append('var g%s%d_%d = Array.from(g%d.GROUP_BY_%s("cat", "x", mk%d).values);'
                                ' assert_true(g%s%d_%d[gi%d_%d] === %r, "gb %s of %s t%d");'
                                % (nm, trial, ki, trial, nm.upper(), trial,
                                   nm, trial, ki, trial, ki, agg[nm][ki],
                                   nm, k, trial))
                EMIT.append('var gc%d_%d = Array.from(g%d.GROUP_BY_COUNT("cat", mk%d).values);'
                            ' assert_true(gc%d_%d[gi%d_%d] === %d, "gb count of %s t%d");'
                            % (trial, ki, trial, trial, trial, ki, trial, ki, counts[ki], k, trial))
                n += 6
        else:
            EMIT.append('var gb%d = g%d.GROUP_BY_SUM("cat", "x", mk%d);'
                        ' assert_arr_eq(gb%d.keys, %s, "gb keys t%d");'
                        ' assert_arr_close(Array.from(gb%d.values), [%s], 1e-9, 1e-12, "gb sum t%d");'
                        % (trial, trial, trial, trial, json.dumps(order), trial,
                           trial, ",".join(repr(v) for v in sums), trial))
            EMIT.append('assert_arr_close(Array.from(g%d.GROUP_BY_MEAN("cat", "x", mk%d).values), [%s], 1e-9, 1e-12, "gb mean t%d");'
                        % (trial, trial, ",".join(repr(v) for v in means), trial))
            EMIT.append('assert_arr_close(Array.from(g%d.GROUP_BY_MIN("cat", "x", mk%d).values), [%s], 1e-9, 0, "gb min t%d");'
                        % (trial, trial, ",".join(repr(v) for v in mins), trial))
            EMIT.append('assert_arr_close(Array.from(g%d.GROUP_BY_MAX("cat", "x", mk%d).values), [%s], 1e-9, 0, "gb max t%d");'
                        % (trial, trial, ",".join(repr(v) for v in maxs), trial))
            EMIT.append('assert_arr_eq(Array.from(g%d.GROUP_BY_COUNT("cat", mk%d).values), [%s], "gb count t%d");'
                        % (trial, trial, ",".join(map(str, counts)), trial))
            n += 6
    # integer keys come back SORTED ascending (verified 2026-09-12; string
    # keys are first-seen). sums: k2=1+3, k1=2+5, k0=4
    EMIT.append('var gi = new DataFrame({ k: new Int32Array([2,1,2,0,1]), v: new Float64Array([1,2,3,4,5]) });')
    EMIT.append('var r = gi.GROUP_BY_SUM("k", "v");')
    EMIT.append('assert_arr_eq(r.keys, [0,1,2], "int key sorted order");')
    EMIT.append('assert_arr_close(Array.from(r.values), [4,7,4], 1e-12, 0, "int key sums");')
    n += 2
    # float key refused
    EMIT.append('var gf = new DataFrame({ k: new Float64Array([1.5,2.5]), v: new Float64Array([1,2]) });')
    EMIT.append('assert_throws(function () { gf.GROUP_BY_SUM("k", "v"); }, null, "float key refused");')
    n += 1
    for line in EMIT:
        p.w(line)
    p.close(n)

def gen_core():
    tag = "df_core"
    p = ProbeWriter(os.path.join(PROBES, tag + ".js"),
                    ['import { DataFrame } from "dyna:dataframe";'], tag)
    rng = lcg(7003)
    EMIT = []
    n = 0
    # FROM_RECORDS: union of keys, NaN/"" fill, first-seen order
    EMIT.append('var df = new DataFrame({}).FROM_RECORDS([{a:1,b:"x"},{a:2},{b:"y",a:3}]);')
    EMIT.append('assert_eq(df.COLUMNS.join(","), "a,b", "FROM_RECORDS union order");')
    EMIT.append('assert_eq(df.ROWS, 3, "FROM_RECORDS rows");')
    EMIT.append('var rec = df.TO_RECORDS();')
    EMIT.append('assert_eq(rec[1].b, "", "missing string key fills empty");')
    EMIT.append('assert_true(isNaN(rec[1].a) === false && rec[1].a === 2, "present keys keep values");')
    EMIT.append('var rec2 = new DataFrame({}).FROM_RECORDS([{a:1},{b:2}]).TO_RECORDS();')
    EMIT.append('assert_true(isNaN(rec2[0].b) || rec2[0].b === undefined, "missing numeric fills NaN-ish");')
    n += 6
    # zero-copy aliasing contract
    EMIT.append('var buf = new Float64Array([10, 20, 30]);')
    EMIT.append('var dz = new DataFrame({ id: new Int32Array([1,2,3]), v: buf, tag: ["x","y","x"] });')
    EMIT.append('buf[0] = 99;')
    EMIT.append('assert_eq(dz.TO_COLUMNS().v[0], 99, "numeric column aliases the buffer (live)");')
    n += 2
    # TO_CSV round trip vs python csv quoting rules
    rows = [["id", "v", "s"], ["1", "3.5", "plain"], ["2", "1.0", 'has,comma'], ["3", "", 'has"quote']]
    EMIT.append('var dc = new DataFrame({ id: new Int32Array([1,2,3]), v: new Float64Array([3.5,1,NaN]), s: ["plain","has,comma",\'has"quote\'] });')
    EMIT.append('var csv = dc.TO_CSV();')
    EMIT.append('assert_eq(csv.split("\\n")[0], "id,v,s", "csv header");')
    EMIT.append('assert_true(csv.indexOf(\'"has,comma"\') >= 0, "csv quotes commas");')
    EMIT.append('assert_true(csv.indexOf(\'"has""quote"\') >= 0, "csv escapes quotes per RFC4180");')
    n += 3
    # SLICE negative indices
    EMIT.append('var sl = dz.SLICE(-2);')
    EMIT.append('assert_eq(sl.ROWS, 2, "slice(-2) rows");')
    EMIT.append('assert_arr_eq(Array.from(sl.TO_COLUMNS().id), [2,3], "slice(-2) values");')
    EMIT.append('assert_eq(dz.SLICE(1,3).ROWS, 2, "slice(1,3)");')
    EMIT.append('assert_eq(dz.SLICE(5,10).ROWS, 0, "slice clamps");')
    n += 4
    # MASK = pandas where: mask 1 keeps the row, mask 0 takes `fill`
    # (default NaN numeric / "" string per API.md)
    EMIT.append('var dm = dz.MASK(new Uint8Array([1,0,1]));')
    EMIT.append('assert_true(isNaN(dm.TO_COLUMNS().v[1]), "MASK default fill numeric NaN");')
    EMIT.append('assert_eq(dm.TO_COLUMNS().tag[1], "", "MASK default fill string empty");')
    EMIT.append('var dm0 = dz.MASK(new Uint8Array([1,0,1]), 0);')
    EMIT.append('assert_eq(dm0.TO_COLUMNS().v[1], 0, "MASK explicit fill 0");')
    n += 3
    # SAMPLE seeded determinism + without-replacement
    EMIT.append('var s1 = dz.SAMPLE(2, 42), s2 = dz.SAMPLE(2, 42);')
    EMIT.append('assert_arr_eq(Array.from(s1.TO_COLUMNS().id), Array.from(s2.TO_COLUMNS().id), "sample seeded reproducible");')
    EMIT.append('var s3 = dz.SAMPLE(3, 7); assert_eq(new Set(Array.from(s3.TO_COLUMNS().id)).size, 3, "sample without replacement");')
    EMIT.append('assert_throws(function () { dz.SAMPLE(4, 1); }, null, "sample n > ROWS refused");')
    n += 3
    # errors: length mismatch, dup select, rename collisions
    EMIT.append('assert_throws(function () { new DataFrame({ a: new Int32Array([1]), b: new Int32Array([1,2]) }); }, null, "length mismatch refused");')
    EMIT.append('assert_throws(function () { dz.SELECT(["id", "id"]); }, null, "duplicate select refused");')
    EMIT.append('assert_throws(function () { dz.RENAME({ nope: "x" }); }, null, "unknown rename refused");')
    EMIT.append('assert_throws(function () { dz.RENAME({ id: "tag" }); }, null, "colliding rename refused");')
    EMIT.append('assert_eq(dz.DROP_COLUMNS(["tag"]).COLUMNS.join(","), "id,v", "drop columns");')
    EMIT.append('assert_eq(dz.RENAME({ id: "key" }).COLUMNS.join(","), "key,v,tag", "rename");')
    # dtypes
    EMIT.append('var dt = dz.DTYPES();')
    EMIT.append('assert_eq(JSON.stringify(dt), \'{"id":"i32","v":"f64","tag":"str"}\', "dtypes");')
    # TO_JSON nulls for NaN
    EMIT.append('var dj = new DataFrame({ x: new Float64Array([1, NaN]) });')
    EMIT.append('assert_eq(dj.TO_JSON(), \'[{"x":1},{"x":null}]\', "TO_JSON NaN -> null");')
    n += 11
    for line in EMIT:
        p.w(line)
    p.close(n)

def main():
    gen_reductions()
    gen_groupby()
    gen_core()

if __name__ == "__main__":
    main()
