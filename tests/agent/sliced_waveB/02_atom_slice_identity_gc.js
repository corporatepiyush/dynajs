/* T1: atom-parent slices — retention, GC stress, intern-table interplay.
 * The slice holds a strong reference on the atom; the weak intern table
 * must keep the entry while any slice lives. Content must stay correct
 * across GC cycles and after the last non-slice reference is dropped. */
var LIT = "Retention probe literal: the quick brown fox jumps over the lazy dog again and again. Retention probe literal: the quick brown fox jumps over the lazy dog again and again.";

test("retention_keeps_content", function () {
  var keep = [];
  for (var i = 0; i < 100; i++) keep.push(LIT.slice(10 + (i % 5), 100 + (i % 5)));
  /* drop and re-create garbage to force collections */
  for (var i = 0; i < 10000; i++) {
    var junk = ("garbage-" + i + "-garbage").slice(5);
    junk.length;
  }
  var okc = true;
  for (var i = 0; i < 100; i++) {
    var e = LIT.substring(10 + (i % 5), 100 + (i % 5));
    if (keep[i] !== e) { okc = false; break; }
  }
  ok(okc, "100 live atom slices keep content across GC");
});

test("retention_after_parent_value_dropped", function () {
  /* value-built parent sliced, parent ref dropped: slices keep chars */
  var v = mkvalRnd(2000, 7, false);
  var sl = [];
  for (var i = 0; i < 50; i++) sl.push(v.slice(100 + i, 1000 + i));
  v = null;
  for (var i = 0; i < 20000; i++) ("junk" + i).slice(2);
  var okc = true;
  for (var i = 0; i < 50; i++) {
    if (sl[i].length !== 900) { okc = false; break; }
    if (sl[i].charCodeAt(0) !== mkvalRnd(2000, 7, false).charCodeAt(100 + i)) { okc = false; break; }
  }
  ok(okc, "slices keep parent bytes alive");
});

test("gc_stress_atom_slices", function () {
  var acc = 0;
  for (var i = 0; i < 5000; i++) {
    var t = LIT.slice(i % 30, 120 + (i % 30));
    acc += t.length;
    if (i % 97 === 0) ok(t === LIT.substring(i % 30, 120 + (i % 30)), "spot check " + i);
  }
  eq(acc, 5000 * 120, "stress length sum sanity");
});

test("intern_table_intact_after_slices", function () {
  /* slices of a literal must not disturb interning of the literal itself */
  var keep = LIT.slice(10, 100);
  var o = {};
  o[LIT] = 1;
  for (var i = 0; i < 3000; i++) LIT.slice(5, 105); /* atom slices churn */
  eq(o[LIT], 1, "literal key still resolves to same interned entry");
  ok(keep === LIT.substring(10, 100), "slice content unchanged");
});

test("slice_of_slice_atom_root", function () {
  var a = LIT.slice(5, 150);   /* atom-parent slice */
  var b = a.slice(10, 100);    /* resolves to atom root with offset 15 */
  var c = b.slice(5, 50);      /* depth-2 creation, depth-1 structure */
  eq(c, LIT.substring(20, 65), "slice-of-slice over atom parent");
  eq(b.charCodeAt(0), LIT.charCodeAt(15), "offset math");
  ok(b === LIT.substring(15, 105), "content equality");
});

test("empty_and_boundary_atom_slices", function () {
  eq(LIT.slice(5, 5), "", "empty slice");
  eq(LIT.slice(LIT.length), "", "slice at end");
  eq(LIT.slice(0, 0), "", "zero width");
  var tail = LIT.slice(LIT.length - 64 < 0 ? 0 : LIT.length - 64);
  ok(tail === LIT.substring(LIT.length - 64), "tail slice");
  eq(LIT.slice(3, 3 + 1), LIT[3], "single char slice content");
});

summary("sliced_waveB");
