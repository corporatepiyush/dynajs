#!/usr/bin/env python3
# gen_dataframe_order.py -- df_order probe: ordering & ranking vs numpy.
#
# ORACLE POLICY: numpy is the reference.
#   - SORT: np.sort values (NaN last), documented engine rule.
#   - ARG_SORT: np.argsort(kind="stable") EXACT -- the docs promise
#     "ties by row index", i.e. stability, on duplicate-heavy columns.
#   - RANK: average ranks (ties share the mean position), NaN rows stay NaN.
#   - N_LARGEST / N_SMALLEST: sorted references, exact.
#   - UNIQUE: first-seen order (python dict), MODE, GROUP_ARRAY_SORTED.
#   - TO_CSV round trip through an in-probe RFC4180 parser -> FROM_RECORDS.
import os
import json
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
PROBES = os.path.join(HERE, "..", "probes", "df")
HARNESS = os.path.join(HERE, "..", "h.js")

from gen_dataframe import ProbeWriter, lcg, jsnum  # noqa: E402


def gen_order():
    tag = "df_order"
    p = ProbeWriter(os.path.join(PROBES, tag + ".js"),
                    ['import { DataFrame } from "dyna:dataframe";'], tag)
    rng = lcg(7004)
    EMIT = []
    n = 0
    for trial in range(6):
        m = [1, 2, 8, 17, 40, 100][trial]
        # duplicates guaranteed: draw from a small range
        xs = [float(next(rng) % 9) for _ in range(m)]
        if m >= 8:
            xs[3] = float("nan")
        if m >= 17:
            xs[9] = float("nan")
        arr = np.array(xs)
        mask = np.ones(m, dtype=int)
        if m > 4:
            mask[1] = 0
        live_idx = np.flatnonzero(np.array(mask) == 1)
        live = arr[live_idx]
        srt = np.sort(live)                       # NaN last, matches docs
        # engine returns ORIGINAL row indices (verified 2026-09-12), stable,
        # NaN last
        argsrt = live_idx[np.argsort(live, kind="stable")]
        # average ranks with NaN -> NaN; the engine returns a FULL ROWS-length
        # array, NaN at masked-out rows and at NaN rows (verified 2026-09-12)
        # average ranks over the NON-NaN selected values only (NaN rows do
        # not consume rank positions -- verified against the engine
        # 2026-09-12); the engine returns a FULL ROWS-length array, NaN at
        # masked-out rows and at NaN rows
        ranks = np.full(m, np.nan)
        nn = live[~np.isnan(live)]
        nn_idx = live_idx[~np.isnan(live)]
        order = np.argsort(nn, kind="stable")
        svals = nn[order]
        i = 0
        while i < len(nn):
            j = i
            while j + 1 < len(nn) and svals[j + 1] == svals[i]:
                j += 1
            avg = (i + j) / 2.0 + 1.0
            for k in range(i, j + 1):
                ranks[nn_idx[order[k]]] = avg
            i = j + 1
        t = trial
        EMIT.append('var o%d = new DataFrame({ x: new Float64Array([%s]) });'
                    % (t, ",".join(jsnum(v) for v in xs)))
        EMIT.append('var om%d = new Uint8Array([%s]);'
                    % (t, ",".join(map(str, mask))))
        EMIT.append('assert_arr_close(Array.from(o%d.SORT("x", om%d)), [%s], 0, 0, "SORT NaN-last exact t%d");'
                    % (t, t, ",".join("NaN" if v != v else repr(float(v)) for v in srt), t))
        EMIT.append('assert_arr_eq(Array.from(o%d.ARG_SORT("x", om%d)), [%s], "ARG_SORT stable t%d");'
                    % (t, t, ",".join(str(int(v)) for v in argsrt), t))
        EMIT.append(('var rk%d = Array.from(o%d.RANK("x", om%d));'
                     ' var want%d = [%s];'
                     ' var rok%d = rk%d.length === want%d.length && rk%d.every(function (v, i) {'
                     ' return (v !== v && want%d[i] !== want%d[i]) || v === want%d[i]; });'
                     ' assert_true(rok%d, "RANK average ties t%d");')
                    % (t, t, t, t,
                       ",".join("NaN" if v != v else repr(float(v)) for v in ranks),
                       t, t, t, t, t, t, t, t, t))
        if m >= 4:
            k = min(3, len(srt))
            nz = live[~np.isnan(live)]
            nl = sorted(nz, reverse=True)[:k]
            ns = sorted(nz)[:k]
            EMIT.append('assert_arr_close(Array.from(o%d.N_LARGEST("x", %d, om%d)), [%s], 0, 0, "N_LARGEST t%d");'
                        % (t, k, t, ",".join(repr(float(v)) for v in nl), t))
            EMIT.append('assert_arr_close(Array.from(o%d.N_SMALLEST("x", %d, om%d)), [%s], 0, 0, "N_SMALLEST t%d");'
                        % (t, k, t, ",".join(repr(float(v)) for v in ns), t))
            n += 2
        n += 4
    # UNIQUE first-seen order on strings, MODE, duplicate-key groupby arrays
    EMIT.append('var u = new DataFrame({ s: ["b","a","b","c","a","b"], x: new Float64Array([1,2,3,4,5,6]) });')
    EMIT.append('assert_arr_eq(Array.from(u.UNIQUE("s")), ["b","a","c"], "UNIQUE first-seen");')
    EMIT.append('assert_eq(u.MODE("s"), "b", "MODE most frequent");')
    EMIT.append('var ga = u.GROUP_ARRAY_SORTED("s", "x");')
    # GROUP_ARRAY_SORTED (engine verified 2026-09-12): keys in FIRST-SEEN
    # order (not sorted), values = one array PER GROUP, each sorted ascending:
    # b:[1,3,6]  a:[2,5]  c:[4]
    EMIT.append('assert_arr_eq(ga.keys, ["b","a","c"], "group array keys first-seen");')
    EMIT.append('assert_arr_close(Array.from(ga.values[0]), [1,3,6], 0, 0, "group b sorted");')
    EMIT.append('assert_arr_close(Array.from(ga.values[1]), [2,5], 0, 0, "group a sorted");')
    EMIT.append('assert_arr_close(Array.from(ga.values[2]), [4], 0, 0, "group c sorted");')
    n += 4
    # empty frame edges
    EMIT.append('var e0 = new DataFrame({ x: new Float64Array([]), s: [] });')
    EMIT.append('assert_eq(e0.ROWS, 0, "0-row frame");')
    EMIT.append('assert_eq(e0.SUM("x"), 0, "0-row SUM is 0");')
    EMIT.append('assert_eq(Array.from(e0.ARG_SORT("x")).length, 0, "0-row ARG_SORT empty");')
    n += 3
    # TO_CSV round trip through an in-probe RFC4180 parser
    EMIT.append('var rt = new DataFrame({ id: new Int32Array([1,2,3]), v: new Float64Array([3.5,1,NaN]), s: ["plain","has,comma",\'has"quote\'] });')
    EMIT.append('var text = rt.TO_CSV();')
    EMIT.append('function parseCsv(t) {')
    EMIT.append('  var rows = [[]], field = "", inQ = false, i = 0;')
    EMIT.append('  while (i < t.length) {')
    EMIT.append('    var ch = t[i];')
    EMIT.append("""    if (inQ) { if (ch === '"') { if (t[i+1] === '"') { field += '"'; i += 2; continue; } inQ = false; i++; continue; } field += ch; i++; continue; }""")
    EMIT.append("""    if (ch === '"') { inQ = true; i++; continue; }""")
    EMIT.append("""    if (ch === ',') { rows[rows.length-1].push(field); field = ""; i++; continue; }""")
    EMIT.append("""    if (ch === '\\n' || ch === '\\r') { if (ch === '\\r' && t[i+1] === '\\n') i++; rows[rows.length-1].push(field); field = ""; i++; if (i < t.length) rows.push([]); continue; }""")
    EMIT.append('    field += ch; i++;')
    EMIT.append('  }')
    EMIT.append('  if (field !== "" || rows[rows.length-1].length > 0) rows[rows.length-1].push(field);')
    EMIT.append('  return rows.filter(function (r) { return r.length > 0; });')
    EMIT.append('}')
    EMIT.append('var parsed = parseCsv(text);')
    EMIT.append('var header = parsed[0];')
    EMIT.append('var recs = [];')
    EMIT.append('for (var r = 1; r < parsed.length; r++) { var o = {};'
                ' for (var c = 0; c < header.length; c++) o[header[c]] = parsed[r][c]; recs.push(o); }')
    EMIT.append('var back = new DataFrame({}).FROM_RECORDS(recs);')
    EMIT.append('assert_eq(back.ROWS, 3, "csv round trip rows");')
    EMIT.append('assert_eq(back.TO_COLUMNS().id[2], "3", "csv round trip id col (string; CSV has no dtypes)");')
    EMIT.append('assert_true(parseFloat(back.TO_COLUMNS().v[1]) === 1, "csv round trip float col");')
    EMIT.append('assert_true(isNaN(parseFloat(back.TO_COLUMNS().v[2])), "csv round trip NaN cell");')
    EMIT.append('assert_eq(back.TO_COLUMNS().s[1], "has,comma", "csv round trip quoted comma");')
    EMIT.append('assert_eq(back.TO_COLUMNS().s[2], \'has"quote\', "csv round trip escaped quote");')
    n += 6
    for line in EMIT:
        p.w(line)
    p.close(n)


if __name__ == "__main__":
    gen_order()
