// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["PASS 06_slice_regexp"];
/* regexp exec/match/replace/search/split with slice subjects. */
var s = "ab".repeat(1000) + "CAPTURE" + "cd".repeat(1000);
var t = s.slice(50, s.length - 50);
var m = /C(A)(P)TURE/.exec(t);
ok(m, "exec match");
eq(m[0], "CAPTURE", "exec full");
eq(m[1] + m[2], "AP", "exec groups");
eq(m.index, 1950, "exec index into slice");
eq(t.slice(m.index, m.index + 7), "CAPTURE", "exec index usable");
/* global exec with lastIndex on a slice */
var re = /a/g;
var t2 = "abab".repeat(500).slice(3, 1999);
var hits = [], r2 = /a/g, r;
while ((r = r2.exec(t2)) !== null) { hits.push(r.index); if (hits.length > 500) break; }
eq(hits.length, 501, "global exec count");
eq(hits[0], 1, "global exec first index");
eq(hits[1], 3, "global exec second index");
/* sticky */
var rs = /b/y, sc = 0, rr;
rs.lastIndex = 0;
while ((rr = rs.exec(t2)) !== null) { sc++; rs.lastIndex = rr.index + 2; }
eq(sc, 998, "sticky count");
/* match/search/split/replace on slices */
eq(t.match(/(C)A/)[1], "C", "match on slice");
eq(t.search(/CAPTURE/), 1950, "search on slice");
deepEq("xxCx".repeat(400).slice(1, 800).split("C").length, 201, "split regexp count");
eq(t.replace(/C(A)(P)TURE/, "$2$1P"), s.slice(50, s.length - 50).replace(/C(A)(P)TURE/, "$2$1P"), "replace groups");
/* matchAll indices */
var it = t2.matchAll(/a/g);
var first = it.next().value;
eq(first.index, 1, "matchAll first index");
/* named groups */
var nm = /(?<x>C)A/.exec(t);
eq(nm.groups.x, "C", "named group on slice");
__L(0, "PASS 06_slice_regexp");

summary("sliced_strings");
