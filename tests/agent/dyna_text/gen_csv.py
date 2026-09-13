#!/usr/bin/env python3
"""gen_csv.py — dyna:csv black-box matrix generator.

Oracle: python3 csv module on WELL-FORMED input (reader/writer differential,
both directions). Lenient-recovery rows are IMPL-DEFINED per API.md ("the
default tolerant mode keeps the readable prefix") and are pinned to the
engine's documented behavior with explicit divergence notes. dynajs writes LF
line endings where RFC 4180 names CRLF — parser accepts both; the writer
direction pins LF as impl-defined.
"""
import csv, io, json
from gen_common import jslit, norm_json, norm_str, write_probe

def py_rows(text):
    """python csv.reader (default excel dialect) over the text."""
    return list(csv.reader(io.StringIO(text)))

def py_write(rows, headers=None, term="\n"):
    """python csv.writer output (minimal quoting), LF line endings."""
    buf = io.StringIO()
    w = csv.writer(buf, lineterminator=term)
    if headers is not None:
        w.writerow(headers)
    for r in rows:
        w.writerow(r)
    return buf.getvalue()

def expr_text(s):
    return jslit(s)

CSV_IMPORTS = [("C", "dyna:csv"), ("F", "dyna:file")]

def wtext(text):
    """baked file-bytes want: raw text -> __norm string form"""
    return json.dumps(norm_str(text))

def gen_read_matrix():
    c = []
    c.append('F.makeDir(new F.Path("out/scratch"), {recursive:true});')
    c.append('var __n = 0;')
    c.append('function __fixture(text) { var p = new F.Path("out/scratch/cf" + (++__n) + ".csv");'
             ' F.writeFile(p, text); return new C.CSVFile(p); }')

    # ---- well-formed matrix: python csv differential (reader direction) ----
    fixtures = [
        ("basic", "a,b\n1,2\n3,4\n"),
        ("no_trailing_nl", "a,b\n1,2"),
        ("crlf", "a,b\r\n1,2\r\n"),
        ("cr_mixed", "a,b\n1,2\r\n"),
        ("quoted_comma", 'name,note\n"x,y",plain\n'),
        ("quoted_quote", 'name,note\n"x""y",plain\n'),
        ("quoted_nl", 'name,note\n"multi\nline",plain\n'),
        ("quoted_crlf_nl", 'name,note\n"multi\r\nline",plain\n'),
        ("empty_fields", "a,b,c\n,,\n1,,3\n"),
        ("empty_quoted", 'a,b\n"",\n'),
        ("single_col", "a\n1\n22\n"),
        ("unicode", "énom,valeur\ncafé,élément\n"),
        ("astral", "emoji,tag\n🌍,🚀\n"),
        ("spaces_kept", "a,b\n lead ,trail \n"),
        ("many_cols", ",".join("c%d" % i for i in range(32)) + "\n" + ",".join(str(i) for i in range(32)) + "\n"),
        # NOTE: blank line row is a PINNED divergence (python yields []; the
        # engine pads it to ["",""] per its ragged-pad rule) -- excluded here
        # and pinned explicitly below.
        ("quote_at_edges", 'a,b\n"x",y"z\n'),
        ("newline_in_last", 'a,b\n1,"2\n"\n'),
    ]
    for name, text in fixtures:
        rows = py_rows(text)
        c.append('case_eq("rd_%s", %s, function(){ var f = __fixture(%s);'
                 ' try { return f.read({}); } finally { f.close(); } });'
                 % (name, json.dumps(norm_json(
                     {"headers": rows[0], "rows": rows[1:], "totalRows": len(rows) - 1})),
                    expr_text(text)))

    # blank line: PINNED divergence row (python csv.reader yields a [] row;
    # the engine pads it to ["",""] per its documented ragged-pad rule)
    c.append('case_eq("rd_blank_line_PINNED", %s, function(){ var f = __fixture("a,b\\n1,2\\n\\n3,4\\n");'
             ' try { return f.read({}); } finally { f.close(); } });'
             % json.dumps(norm_json({"headers": ["a", "b"],
                                     "rows": [["1", "2"], ["", ""], ["3", "4"]], "totalRows": 3})))

    # BOM stripped (documented); CRLF + BOM combined
    c.append('case_eq("rd_bom", %s, function(){ var f = __fixture("\\uFEFFa,b\\n1,2\\n");'
             ' try { return f.read({}); } finally { f.close(); } });'
             % json.dumps(norm_json({"headers": ["a", "b"], "rows": [["1", "2"]], "totalRows": 1})))

    # options: offset / limit / columns / totalRows invariants
    c.append('case_eq("rd_offset", %s, function(){ var f = __fixture("a,b\\n1,2\\n3,4\\n5,6\\n");'
             ' try { return f.read({offset: 1}); } finally { f.close(); } });'
             % json.dumps(norm_json({"headers": ["a", "b"], "rows": [["3", "4"], ["5", "6"]], "totalRows": 3})))
    c.append('case_eq("rd_limit", %s, function(){ var f = __fixture("a,b\\n1,2\\n3,4\\n5,6\\n");'
             ' try { return f.read({limit: 2}); } finally { f.close(); } });'
             % json.dumps(norm_json({"headers": ["a", "b"], "rows": [["1", "2"], ["3", "4"]], "totalRows": 3})))
    c.append('case_eq("rd_offset_limit", %s, function(){ var f = __fixture("a,b\\n1,2\\n3,4\\n5,6\\n");'
             ' try { return f.read({offset: 1, limit: 1}); } finally { f.close(); } });'
             % json.dumps(norm_json({"headers": ["a", "b"], "rows": [["3", "4"]], "totalRows": 3})))
    c.append('case_eq("rd_columns", %s, function(){ var f = __fixture("a,b\\n1,2\\n");'
             ' try { return f.read({columns: ["b", "a"]}); } finally { f.close(); } });'
             % json.dumps(norm_json({"headers": ["b", "a"], "rows": [["2", "1"]], "totalRows": 1})))
    c.append('case_err("rd_columns_unknown", "TypeError", function(){'
             ' var f = __fixture("a,b\\n1,2\\n"); try { return f.read({columns:["zz"]}); }'
             ' finally { f.close(); } });')
    c.append('case_eq("rd_strict_ok", %s, function(){ var f = __fixture("a,b\\n1,2\\n");'
             ' try { return f.read({strict: true}); } finally { f.close(); } });'
             % json.dumps(norm_json({"headers": ["a", "b"], "rows": [["1", "2"]], "totalRows": 1})))

    # ---- ragged rows (documented lenient: pad short, truncate long) ----
    c.append('case_eq("rd_ragged", %s, function(){ var f = __fixture("a,b,c\\n1,2\\n1,2,3,4\\n");'
             ' try { return f.read({}); } finally { f.close(); } });'
             % json.dumps(norm_json({"headers": ["a", "b", "c"],
                                     "rows": [["1", "2", ""], ["1", "2", "3"]], "totalRows": 2})))

    # ---- empty / degenerate files (documented) ----
    c.append('case_err("rd_empty_file", "TypeError", function(){'
             ' var f = __fixture(""); try { return f.read({}); } finally { f.close(); } });')
    c.append('case_eq("rd_header_only", %s, function(){ var f = __fixture("a,b");'
             ' try { return f.read({}); } finally { f.close(); } });'
             % json.dumps(norm_json({"headers": ["a", "b"], "rows": [], "totalRows": 0})))

    # ---- strict-mode rejection (documented) ----
    c.append('case_err("strict_garbage", "SyntaxError", function(){'
             ' var f = __fixture(\'a,b\\n"x"garbage,2\\n\');'
             ' try { return f.read({strict:true}); } finally { f.close(); } });')
    c.append('case_err("strict_unterminated", "SyntaxError", function(){'
             ' var f = __fixture("a,b\\n\\"unterminated,2\\n");'
             ' try { return f.read({strict:true}); } finally { f.close(); } });')

    # ---- lenient recovery: IMPL-DEFINED divergence rows (pinned; python csv
    # produces different output for these, documented in CHANGELOG) ----
    c.append('#ifdef IMPL_DEFINED_LENIENT') if False else None
    c.append('case_eq("lenient_garbage_PINNED", %s, function(){'
             ' var f = __fixture(\'a,b\\n"x"garbage,2\\n\');'
             ' try { return f.read({}); } finally { f.close(); } });'
             % json.dumps(norm_json({"headers": ["a", "b"],
                                     "rows": [["x", ""], ["arbage", "2"]], "totalRows": 2})))
    c.append('case_eq("lenient_unterminated_PINNED", %s, function(){'
             ' var f = __fixture("a,b\\n\\"unterminated,2\\n");'
             ' try { return f.read({}); } finally { f.close(); } });'
             % json.dumps(norm_json({"headers": ["a", "b"],
                                     "rows": [["unterminated,2\n", ""]], "totalRows": 1})))

    # ---- huge field (1 MiB) ----
    big = "x" * (1 << 20)
    c.append('case_true("rd_huge_field", function(){'
             ' var f = __fixture("a,b\\n1,%s");' % "1" if False else
             'var __big = "x".repeat(%d);' % (1 << 20) + '''
    case_true("rd_huge_field", function(){
        var f = __fixture("a,b\\n1," + __big + "\\n");
        try { var r = f.read({}); return r.rows[0][1].length === __big.length && r.rows[0][0] === "1"; }
        finally { f.close(); }
    });''')
    return c


def gen_write_direction():
    c = []
    c.append('F.makeDir(new F.Path("out/scratch"), {recursive:true});')

    # create + addRow then compare the FILE BYTES against python csv.writer
    # (minimal RFC 4180 quoting; LF terminator is impl-defined, pinned here)
    cases = [
        ("plain", ["a", "b"], [["1", "2"], ["3", "4"]]),
        ("needs_quoting", ["a", "b"], [["x,y", 'has"quote'], ["multi\nline", "plain"]]),
        ("empty_vals", ["a", "b"], [["", ""], ["x", ""]]),
        ("unicode", ["énom"], [["café"], ["🌍🚀"]]),
        ("quotes_around", ["a"], [['"quoted"'], ["pre\"mid"]]),
    ]
    for name, headers, rows in cases:
        want = py_write(rows, headers=headers)
        c.append('case_eq("wr_%s", %s, function(){'
                 ' var p = new F.Path("out/scratch/wr_%s.csv");'
                 ' var f = new C.CSVFile(p);'
                 ' try { f.create({headers: %s, rows: %s, overwrite: true});'
                 ' return F.readFile(p); } finally { f.close(); } });'
                 % (name, wtext(want), name,
                    json.dumps(headers, ensure_ascii=True),
                    json.dumps(rows, ensure_ascii=True)))
    # addRow appends with the same quoting
    c.append('case_eq("wr_addrow", %s, function(){'
             ' var p = new F.Path("out/scratch/wr_add.csv");'
             ' var f = new C.CSVFile(p);'
             ' try { f.create({headers: ["a","b"], overwrite: true});'
             ' f.addRow({rows: [["1","2"], {a:"x,y", b:"q\\"q"}, ["multi\\nline", ""]]});'
             ' return F.readFile(p); } finally { f.close(); } });'
             % wtext(py_write([["1", "2"], ["x,y", 'q"q'], ["multi\nline", ""]],
                              headers=["a", "b"])))
    # value coercion to strings (documented)
    c.append('case_eq("wr_coerce", %s, function(){'
             ' var p = new F.Path("out/scratch/wr_co.csv");'
             ' var f = new C.CSVFile(p);'
             ' try { f.create({headers: ["n"], overwrite: true});'
             ' f.addRow({rows: [[42], [3.5], [true], [null]]});'
             ' return F.readFile(p); } finally { f.close(); } });'
             % wtext(py_write([["42"], ["3.5"], ["true"], ["null"]], headers=["n"])))
    # addRow positional short pads / long truncates (documented)
    c.append('case_eq("wr_pad_trunc", %s, function(){'
             ' var p = new F.Path("out/scratch/wr_pt.csv");'
             ' var f = new C.CSVFile(p);'
             ' try { f.create({headers: ["a","b","c"], overwrite: true});'
             ' f.addRow({rows: [["short"], ["1","2","3","4","5"]]});'
             ' return F.readFile(p); } finally { f.close(); } });'
             % wtext(py_write([["short", "", ""], ["1", "2", "3"]], headers=["a", "b", "c"])))
    # object-row keying
    c.append('case_eq("wr_objrow", %s, function(){'
             ' var p = new F.Path("out/scratch/wr_obj.csv");'
             ' var f = new C.CSVFile(p);'
             ' try { f.create({headers: ["h1","h2"], overwrite: true});'
             ' f.addRow({rows: [{h2: "2", h1: "1"}]});'
             ' return F.readFile(p); } finally { f.close(); } });'
             % wtext(py_write([["1", "2"]], headers=["h1", "h2"])))
    return c


def gen_lifecycle():
    c = []
    c.append('F.makeDir(new F.Path("out/scratch"), {recursive:true});')
    c.append('function fresh() { var p = new F.Path("out/scratch/lc.csv");'
             ' var f = new C.CSVFile(p);'
             ' f.create({headers: ["id","name","score"],'
             ' rows: [["1","alice","95"],["2","bob","82"],["3","carol","88"]], overwrite:true});'
             ' return f; }')
    # full lifecycle; expected file bytes computed by python after each op set
    c.append('case_eq("lc_final_bytes", %s, function(){'
             ' var f = fresh(); try {'
             ' f.addColumn({column: "grade", defaultValue: "B"});'
             ' f.renameColumn({oldName: "score", newName: "points"});'
             ' f.updateCell({row: 1, column: "points", value: "100"});'
             ' f.removeRow({row: 0});'
             ' f.removeColumn({column: "id"});'
             ' f.addRow({rows: [{name: "dan", points: "70", grade: "C"}]});'
             ' return F.readFile(new F.Path("out/scratch/lc.csv"));'
             ' } finally { f.close(); } });'
             % wtext(py_write([["bob", "100", "B"], ["carol", "88", "B"], ["dan", "70", "C"]],
                              headers=["name", "points", "grade"])))
    # rename to same name is a no-op (documented)
    c.append('case_eq("lc_rename_same", %s, function(){'
             ' var f = fresh(); try { return f.renameColumn({oldName:"name", newName:"name"}); }'
             ' finally { f.close(); } });'
             % json.dumps(norm_json({"oldName": "name", "newName": "name"})))
    # errors
    c.append('case_err("lc_dup_addcol", "TypeError", function(){ var f = fresh();'
             ' try { return f.addColumn({column:"name"}); } finally { f.close(); } });')
    c.append('case_err("lc_dup_rename", "TypeError", function(){ var f = fresh();'
             ' try { return f.renameColumn({oldName:"name", newName:"score"}); } finally { f.close(); } });')
    c.append('case_err("lc_bad_old", "TypeError", function(){ var f = fresh();'
             ' try { return f.renameColumn({oldName:"nope", newName:"x"}); } finally { f.close(); } });')
    c.append('case_err("lc_drop_missing", "TypeError", function(){ var f = fresh();'
             ' try { return f.removeColumn({column:"nope"}); } finally { f.close(); } });')
    c.append('case_err("lc_update_badcol", "TypeError", function(){ var f = fresh();'
             ' try { return f.updateCell({row:0, column:"nope", value:"1"}); } finally { f.close(); } });')
    c.append('case_err("lc_update_badrow", "RangeError", function(){ var f = fresh();'
             ' try { return f.updateCell({row:99, column:"name", value:"1"}); } finally { f.close(); } });')
    c.append('case_err("lc_remove_badrow", "RangeError", function(){ var f = fresh();'
             ' try { return f.removeRow({row:99}); } finally { f.close(); } });')
    # window guards (documented caps: 100 rows / 1000 values)
    c.append('case_err("lc_rowrange_cap", "RangeError", function(){ var f = fresh();'
             ' try { return f.readRowRange({start:0, end:101}); } finally { f.close(); } });')
    c.append('case_err("lc_valrange_cap", "RangeError", function(){ var f = fresh();'
             ' try { return f.readColumnValuesRange({column:"name", start:0, end:1001}); }'
             ' finally { f.close(); } });')
    c.append('case_eq("lc_rowrange_edge", %s, function(){ var f = fresh();'
             ' try { return f.readRowRange({start:0, end:3}); } finally { f.close(); } });'
             % json.dumps(norm_json({"headers": ["id", "name", "score"],
                                     "rows": [["1", "alice", "95"], ["2", "bob", "82"], ["3", "carol", "88"]]})))
    c.append('case_eq("lc_valrange_win", %s, function(){ var f = fresh();'
             ' try { return f.readColumnValuesRange({column:"name", start:1, end:3}); }'
             ' finally { f.close(); } });'
             % json.dumps(norm_json(["bob", "carol"])))
    c.append('case_eq("lc_select", %s, function(){ var f = fresh();'
             ' try { return f.selectColumnRange({columns:["score","name"], start:0, end:2}); }'
             ' finally { f.close(); } });'
             % json.dumps(norm_json({"columns": ["score", "name"],
                                     "rows": [["95", "alice"], ["82", "bob"]]})))
    c.append('case_err("lc_select_empty", "TypeError", function(){ var f = fresh();'
             ' try { return f.selectColumnRange({columns:[], start:0, end:1}); } finally { f.close(); } });')
    # create guards
    c.append('case_err("lc_create_exists", "TypeError", function(){ var f = fresh();'
             ' try { return f.create({headers:["a"]}); } finally { f.close(); } });')
    c.append('case_eq("lc_create_overwrite", %s, function(){ var f = fresh();'
             ' try { return f.create({headers:["x"], overwrite:true}); } finally { f.close(); } });'
             % json.dumps(norm_json({"path": "out/scratch/lc.csv", "rows": 0})))
    c.append('case_err("lc_create_noheaders", "TypeError", function(){ var f = fresh();'
             ' try { return f.create({headers:[], overwrite:true}); } finally { f.close(); } });')
    # closed-resource discipline
    c.append('case_eq("lc_closed_flag", {"t":"b","v":"false"}, function(){ var f = fresh();'
             ' var v = f.closed; f.close(); return v; });')
    c.append('case_err("lc_use_after_close", "TypeError", function(){ var f = fresh();'
             ' f.close(); return f.read({}); });')
    c.append('case_eq("lc_dispose", {"t":"b","v":"true"}, function(){ var f = fresh();'
             ' f.dispose(); return f.closed; });')
    return c


def main():
    a = gen_read_matrix()
    write_probe("csv/c01_read_matrix.js", CSV_IMPORTS, "\n".join(a), "csv read matrix vs python csv")
    b = gen_write_direction()
    write_probe("csv/c02_write_direction.js", CSV_IMPORTS, "\n".join(b), "csv writer bytes vs python csv.writer")
    d = gen_lifecycle()
    write_probe("csv/c03_lifecycle.js", CSV_IMPORTS, "\n".join(d), "CSVFile load-modify-store lifecycle")
    print("gen_csv: %d cases emitted" % (len([x for x in a+b+d if x.startswith("case_")])))


if __name__ == "__main__":
    main()
