#!/usr/bin/env python3
"""gen_yaml.py — dyna:yaml black-box matrix generator.

Oracle: PyYAML (yaml.safe_load / safe_load_all) on the YAML 1.1/1.2 core
OVERLAP subset (plain scalars, quoting, block styles, flow, nesting,
comments). Documented divergences are pinned to the engine with notes:
dyna:yaml implements the YAML 1.2 CORE schema (no yes/no bools, 0o octal,
no sexagesimal/dates) and refuses anchors/aliases/tags/merge/directives BY
NAME, plus duplicate keys — where PyYAML 1.1 resolves differently. The
Stringify direction is asserted by Parse(Stringify(v)) round-trip invariants
(key order and line-format are impl-defined) plus exact pins.
"""
import json
import yaml as pyyaml
from gen_common import jslit, norm_json, norm_num, norm_any, write_probe

Y_IMPORTS = [("Y", "dyna:yaml")]

def pyload(text):
    return pyyaml.safe_load(text)

# docs where PyYAML and YAML 1.2 core MUST agree
OVERLAP = [
    ("int", "a: 42"),
    ("neg_int", "a: -7"),
    ("zero", "a: 0"),
    ("float", "a: 3.25"),
    ("neg_float", "a: -0.5"),
    ("bool_t", "a: true"),
    ("bool_f", "a: false"),
    ("null_tilde", "a: ~"),
    ("null_word", "a: null"),
    ("null_empty", "a:"),
    ("dq_string", 'a: "hello world"'),
    ("dq_escapes", 'a: "x\\ty\\u00e9"'),
    ("sq_string", "a: 'it''s'"),
    ("literal_block", "a: |\n  l1\n  l2\n"),
    ("literal_strip", "a: |-\n  l1\n  l2\n"),
    ("literal_keep", "a: |+\n  l1\n\n"),
    ("folded", "a: >\n  f1\n  f2\n"),
    ("flow_seq", "a: [1, 2, three]"),
    ("flow_map", "a: {x: 1, y: two}"),
    ("nested_map", "m:\n  n:\n    k: deep"),
    ("seq_of_ints", "l:\n  - 1\n  - 2\n"),
    ("seq_of_maps", "- a: 1\n  b: 2\n- a: 3\n"),
    ("top_seq", "- 1\n- 2"),
    ("top_scalar", "hello"),
    ("comment", "# hi\na: 1 # trailing\n"),
    ("unicode", " ключ: знач 🌍"),
    ("doc_end", "a: 1\n...\n"),
    ("empty_doc", ""),
    ("nested_flow", "a: [[1, 2], {b: c}]"),
    ("empty_flow", "a: []"),
    ("empty_map", "a: {}"),
    ("bool_key", "true: 1"),
    ("int_key", "42: a"),
    ("spaces_after", "a:    42   "),
    ("colon_in_sq", "a: 'key: value'"),
]

# docs the 1.2 core resolves DIFFERENTLY from PyYAML 1.1 — pinned to engine
DIVERGENT = [
    # YAML 1.2 core: only true/false are booleans (the Norway problem)
    ("norway", "a: no", "no"),
    ("yes_str", "a: yes", "yes"),
    ("on_str", "a: on", "on"),
    ("off_str", "a: off", "off"),
    # 1.2 core octal is 0o-prefixed; PyYAML 1.1 treats 0o17 as a string
    ("octal_0o", "a: 0o17", 15),
    # PyYAML resolves 012 as octal 10; 1.2 core has no 0-prefixed octal
    ("leading_zero", "a: 012", 12),
    # sexagesimal/dates are NOT 1.2 core scalars
    ("sexagesimal", "a: 1:30", "1:30"),
    # PyYAML 1.1 requires a dot for exponent floats ("1e3" stays a string);
    # the 1.2 core resolves it
    ("exp_float", "a: 1e3", 1000),
    ("date", "a: 2026-09-11", "2026-09-11"),
]


def gen_parse_overlap():
    c = []
    for name, text in OVERLAP:
        want = pyload(text)
        c.append('case_eq("ov_%s", %s, function(){ return Y.Parse(%s); });'
                 % (name, json.dumps(norm_any(want)), jslit(text)))
    return c


def gen_parse_divergent():
    c = []
    for name, text, want in DIVERGENT:
        c.append('case_eq("dv_%s", %s, function(){ return Y.Parse(%s); });'
                 % (name, json.dumps(norm_any({"a": want})), jslit(text)))
    # .inf / .nan (IEEE core schema)
    c.append('case_eq("dv_inf", {t:"b",v:"true"}, function(){'
             ' return Y.Parse("a: .inf").a === Infinity; });')
    c.append('case_eq("dv_neginf", {t:"b",v:"true"}, function(){'
             ' return Y.Parse("a: -.inf").a === -Infinity; });')
    c.append('case_eq("dv_nan", {t:"b",v:"true"}, function(){'
             ' return Y.Parse("a: .nan").a !== Y.Parse("a: .nan").a; });')
    # refused BY NAME (documented; PyYAML accepts all of these)
    refuses = [
        ("anchor", "a: &x 1\nb: *x", "SyntaxError"),
        ("alias", "a: *x", "SyntaxError"),
        ("tag", "a: !!str 5", "SyntaxError"),
        ("directive", "%YAML 1.2\n---\na: 1", "SyntaxError"),
        ("merge_key", "x: &d\n  a: 1\ny:\n  <<: *d\n", "SyntaxError"),
        ("multi_doc_parse", "---\na: 1\n---\nb: 2\n", "SyntaxError"),
        ("dup_key", "a: 1\na: 2", "SyntaxError"),
    ]
    for name, text, errname in refuses:
        c.append('case_err("refuse_%s", %s, function(){ return Y.Parse(%s); });'
                 % (name, jslit(errname), jslit(text)))
    # ParseAll handles multi-doc (PyYAML safe_load_all agrees)
    c.append('case_eq("parseall", %s, function(){ return Y.ParseAll("---\\na: 1\\n---\\nb: 2\\n"); });'
             % json.dumps(norm_json([{"a": 1}, {"b": 2}])))
    c.append('case_eq("parseall_seq", %s, function(){ return Y.ParseAll("---\\n- 1\\n- 2\\n---\\nx\\n"); });'
             % json.dumps(norm_json([[1, 2], "x"])))
    return c


def gen_pins():
    c = []
    # TICKET (impl limitation, pinned): multi-line quoted scalars are refused.
    # YAML 8.1.2 folding is not implemented in the line-oriented scanner;
    # block scalars (| and >) are the supported multi-line form.
    pins = [
        ("dq_multiline", 'a: "line one\n  line two"'),
        ("sq_multiline", "a: 'line one\n  line two'"),
        ("dq_multiline_key", '"long\n  key": 1'),
    ]
    for name, text in pins:
        c.append('case_err("pin_%s", "SyntaxError", function(){ return Y.Parse(%s); });'
                 % (name, jslit(text)))
    return c


def gen_reject():
    c = []
    # documents both engines reject (differential); plus engine pins
    bad = [
        ("bad_indent", "a: 1\n  b: 2"),
        ("tab_indent", "a:\n\t- 1"),
        ("unclosed_flow", "a: [1, 2"),
        ("unclosed_flow_map", "a: {x: 1"),
        ("unclosed_dq", 'a: "x'),
        ("unclosed_sq", "a: 'x"),
        ("bad_escape", 'a: "\\q"'),
        ("map_in_seq_bad", "- a: 1\n - b"),
    ]
    for name, text in bad:
        try:
            pyyaml.safe_load(text)
            raise SystemExit("pyyaml ACCEPTS %r -- move to pins" % text)
        except (pyyaml.YAMLError, SystemExit) as e:
            if isinstance(e, SystemExit) and "ACCEPTS" in str(e):
                raise
        c.append('case_err("bad_%s", "SyntaxError", function(){ return Y.Parse(%s); });'
                 % (name, jslit(text)))
    return c


CORPUS_VALUES = [
    {"a": 1, "b": [1, 2, 3]},
    {"nested": {"deep": {"deeper": [True, False, None]}}},
    [1, "two", {"three": 3}, [4, 5]],
    "plain string",
    {"multi\nline": "value"},
    {"x": ""},
    {"empty_list": [], "empty_map": {}},
    {"unicode": "日本語 🌍"},
    {"floats": [1.5, -2.25]},
    {"deep": [[[[[[[[1]]]]]]]]},
]


def gen_stringify():
    c = []
    # round-trip invariant: Parse(Stringify(v)) deep-equals v (impl-defined
    # key order / line format, so the ROUND-TRIP is the oracle)
    for i, v in enumerate(CORPUS_VALUES):
        c.append('case_eq("rt%d", %s, function(){ return Y.Parse(Y.Stringify(%s)); });'
                 % (i, json.dumps(norm_any(v)), json.dumps(v, ensure_ascii=False)))
    # exact pins
    c.append('case_eq("str_scalar", {t:"s",v:"42\\n"}, function(){ return Y.Stringify(42); });')
    c.append('case_eq("str_nan", {t:"s",v:"a: .nan\\n"}, function(){ return Y.Stringify({a:NaN}); });')
    c.append('case_eq("str_inf", {t:"s",v:"a: .inf\\n"}, function(){ return Y.Stringify({a:Infinity}); });')
    c.append('case_eq("str_indent2", %s, function(){ return Y.Stringify({a:[1]}, {indent:2}); });'
             % json.dumps(norm_any("a:\n  - 1\n")))
    c.append('case_eq("str_indent1", %s, function(){ return Y.Stringify({a:[1]}, {indent:1}); });'
             % json.dumps(norm_any("a:\n - 1\n")))
    c.append('case_err("str_indent0", "RangeError", function(){ return Y.Stringify(1, {indent:0}); });')
    c.append('case_err("str_indent11", "RangeError", function(){ return Y.Stringify(1, {indent:11}); });')
    # ambiguity-sensitive keys are quoted on output (safe round-trip)
    c.append('case_eq("str_ambiguous_keys", %s, function(){'
             ' return Y.Stringify({"yes": 1, "no": 2, "null": 3, "123": 4}).indexOf("\\"yes\\"") >= 0'
             ' && Y.Stringify({"yes": 1}).indexOf("\\"yes\\"") >= 0; });'
             % json.dumps({"t": "b", "v": "true"}))
    # nesting cap
    c.append('case_eq("str_deep127", {t:"b",v:"true"}, function(){'
             ' var o = {}, cur = o;'
             ' for (var i = 0; i < 126; i++) { cur.n = {}; cur = cur.n; }'
             ' cur.n = 1;'
             ' return typeof Y.Stringify(o) === "string"; });')
    c.append('case_err("str_deep129", "RangeError", function(){'
             ' var o = {}, cur = o;'
             ' for (var i = 0; i < 129; i++) { cur.n = {}; cur = cur.n; }'
             ' return Y.Stringify(o); });')
    # Parse nesting: 130-deep block map refuses (documented 128 cap)
    c.append('case_err("parse_deep_cap", "SyntaxError", function(){'
             ' var s = "k0: 1";'
             ' for (var i = 1; i < 130; i++) {'
             '  var pad = ""; for (var j = 0; j < i; j++) pad += " ";'
             '  s = "k" + i + ":\\n" + pad + s;'
             ' }'
             ' return Y.Parse(s); });')
    return c


def main():
    p = gen_pins()
    a = gen_parse_overlap()
    b = gen_parse_divergent()
    d = gen_reject()
    e = gen_stringify()
    write_probe("yaml/y01_parse_overlap.js", Y_IMPORTS, "\n".join(a),
                "Parse vs PyYAML on the 1.1/1.2 core overlap")
    write_probe("yaml/y02_divergences.js", Y_IMPORTS, "\n".join(b),
                "YAML 1.2 core + refused-by-name (pinned divergences vs PyYAML)")
    write_probe("yaml/y03_reject.js", Y_IMPORTS, "\n".join(d + p),
                "malformed input rejected by BOTH engines + impl-limitation pins")
    write_probe("yaml/y04_stringify.js", Y_IMPORTS, "\n".join(e),
                "Stringify round-trips + exact pins")
    n = sum(len([x for x in c if x.strip().startswith("case_")]) for c in (a, b, d, e, p))
    print("gen_yaml: %d cases emitted" % n)


if __name__ == "__main__":
    main()
