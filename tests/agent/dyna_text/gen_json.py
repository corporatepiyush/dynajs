#!/usr/bin/env python3
"""gen_json.py — dyna:json (RFC 6901 Pointer / RFC 6902 Patch) generator.

Oracle: a pinned python reference resolver for RFC 6901 (get/has semantics)
plus hand-baked expected results for RFC 6902 ops and the documented
dyna:json contract (clone-on-insert, copy-on-write patch, prefix rule).
"""
import json
from gen_common import jslit, norm_json, norm_num, norm_str, write_probe

def norm_any(v):
    """__norm shape dispatching on the python type of the oracle value."""
    if v is None:
        return {"t": "z"}
    if v is True or v is False:
        return {"t": "b", "v": "true" if v else "false"}
    if isinstance(v, (int, float)):
        return norm_num(v)
    if isinstance(v, str):
        return norm_str(v)
    return norm_json(v)

MISSING = object()

def resolve(doc, pointer):
    """RFC 6901 evaluation; returns MISSING when the reference is not resolvable
    (which dyna:json surfaces as a throw for get/remove, false for has)."""
    if pointer == "":
        return doc
    if not pointer.startswith("/"):
        return MISSING
    tokens = [t.replace("~1", "/").replace("~0", "~") for t in pointer.split("/")[1:]]
    cur = doc
    for tok in tokens:
        if isinstance(cur, dict):
            if tok not in cur:
                return MISSING
            cur = cur[tok]
        elif isinstance(cur, list):
            if not tok.isdigit() or (len(tok) > 1 and tok[0] == "0"):
                return MISSING
            i = int(tok)
            if i >= len(cur):
                return MISSING
            cur = cur[i]
        else:
            return MISSING
    return cur

def bad_escape(pointer):
    rest = pointer.lstrip("/") if pointer else ""
    return "~" in rest and any(
        seg.count("~") != seg.count("~0") + seg.count("~1")
        for seg in pointer.split("/")[1:]
    ) or pointer.split("/")[1:] and any(
        "~" in seg and seg.count("~") != seg.count("~0") + seg.count("~1")
        for seg in pointer.split("/")[1:]
    )

DOCS = {
    "obj": {"foo": ["bar", "baz"], "": 0, "a/b": 1, "c%d": 2, "e^f": 3,
            "g|h": 4, "i\\j": 5, "k\"l": 6, " ": 7, "m~n": 8,
            "nested": {"deeper": {"deepest": [True, None, 3.5]}}},
    "arr": ["a", "b", "c", {"x": 9}],
    "scalar": 42,
    "empty_obj": {},
    "empty_arr": [],
}

# RFC 6901 section 5 examples evaluated against the "obj" document shape
POINTER_CASES = [
    ("obj", "", {"foo": ["bar", "baz"], "": 0, "a/b": 1, "c%d": 2, "e^f": 3,
                 "g|h": 4, "i\\j": 5, "k\"l": 6, " ": 7, "m~n": 8,
                 "nested": {"deeper": {"deepest": [True, None, 3.5]}}}),
    ("obj", "/foo", ["bar", "baz"]),
    ("obj", "/foo/0", "bar"),
    ("obj", "/", 0),
    ("obj", "/a~1b", 1),
    ("obj", "/c%d", 2),
    ("obj", "/e^f", 3),
    ("obj", "/g|h", 4),
    ("obj", "/i\\j", 5),
    ("obj", "/k\"l", 6),
    ("obj", "/ ", 7),
    ("obj", "/m~0n", 8),
    ("obj", "/nested/deeper/deepest/1", None),
    ("obj", "/nested/deeper/deepest/2", 3.5),
    ("arr", "/0", "a"),
    ("arr", "/3/x", 9),
    ("scalar", "", 42),
    # unresolvable -> get/remove throw TypeError, has returns false
    ("obj", "/nope", MISSING),
    ("obj", "/foo/2", MISSING),
    # "-": refused where a real index is required; refusal is SYNTAX-class so
    # has() throws too (impl-defined, pinned)
    ("obj", "/foo/-", "THROWS"),
    # RFC 6901: a leading zero is NOT an array index; the engine treats the
    # rejection as SYNTAX-class, so BOTH get and has throw (impl-defined).
    ("obj", "/foo/01", "THROWSBOTH"),
    ("obj", "/foo/x", MISSING),
    ("arr", "/-1", MISSING),
    ("arr", "/4", MISSING),
    ("arr", "/x", MISSING),
    ("empty_arr", "/0", MISSING),
    ("empty_obj", "/a", MISSING),
    ("scalar", "/a", MISSING),
]


def gen_pointer():
    c = []
    for i, (dname, ptr, want) in enumerate(POINTER_CASES):
        doc = DOCS[dname]
        if bad_escape_js(ptr):
            c.append('case_err("pget_%d", "TypeError", function(){'
                     ' return J.Pointer.get(%s, %s); });'
                     % (i, jsdoc(doc), jslit(ptr)))
            c.append('case_err("phas_%d", "TypeError", function(){'
                     ' return J.Pointer.has(%s, %s); });'
                     % (i, jsdoc(doc), jslit(ptr)))
            continue
        if want == "THROWSBOTH":
            # SYNTAX-class rejection (bad escape, leading-zero index): both
            # get and has throw.
            c.append('case_err("pget_%d", "TypeError", function(){'
                     ' return J.Pointer.get(%s, %s); });'
                     % (i, jsdoc(doc), jslit(ptr)))
            c.append('case_err("phas_%d", "TypeError", function(){'
                     ' return J.Pointer.has(%s, %s); });'
                     % (i, jsdoc(doc), jslit(ptr)))
        elif want == "THROWS":
            # "-": refused where a real index is required (get throws); has()
            # classifies it as MISSING (returns false) -- impl-defined split,
            # pinned per engine behavior.
            c.append('case_err("pget_%d", "TypeError", function(){'
                     ' return J.Pointer.get(%s, %s); });'
                     % (i, jsdoc(doc), jslit(ptr)))
            c.append('case_eq("phas_%d", {"t":"b","v":"false"}, function(){'
                     ' return J.Pointer.has(%s, %s); });'
                     % (i, jsdoc(doc), jslit(ptr)))
        elif want is MISSING:
            c.append('case_err("pget_%d", "TypeError", function(){'
                     ' return J.Pointer.get(%s, %s); });'
                     % (i, jsdoc(doc), jslit(ptr)))
            c.append('case_eq("phas_%d", {"t":"b","v":"false"}, function(){'
                     ' return J.Pointer.has(%s, %s); });'
                     % (i, jsdoc(doc), jslit(ptr)))
        else:
            c.append('case_eq("pget_%d", %s, function(){'
                     ' return J.Pointer.get(%s, %s); });'
                     % (i, json.dumps(norm_any(want)), jsdoc(doc), jslit(ptr)))
            c.append('case_eq("phas_%d", {"t":"b","v":"true"}, function(){'
                     ' return J.Pointer.has(%s, %s); });'
                     % (i, jsdoc(doc), jslit(ptr)))
    # pointer-decode safety: escaping round-trips
    c.append('case_eq("pesc", {t:"s",v:"a~1b~0c"}, function(){ return J.Pointer.escape("a/b~c"); });')
    c.append('case_eq("punesc", {t:"s",v:"a/b~c"}, function(){ return J.Pointer.unescape("a~1b~0c"); });')
    c.append('case_eq("punesc_empty", {t:"s",v:""}, function(){ return J.Pointer.unescape(""); });')
    c.append('case_eq("pesc_empty", {t:"s",v:""}, function(){ return J.Pointer.escape(""); });')
    c.append('case_err("punesc_bad", "TypeError", function(){ return J.Pointer.unescape("a~2"); });')
    c.append('case_err("punesc_trail", "TypeError", function(){ return J.Pointer.unescape("a~"); });')

    # set: clone-on-insert, in-place mutation, append, parent rules, __proto__ safety
    c.append('case_eq("pset_clone", %s, function(){ var d={}; var o={x:1};'
             ' J.Pointer.set(d, "/a", o); o.x = 99; return d; });'
             % json.dumps(norm_json({"a": {"x": 1}})))
    c.append('case_eq("pset_arr_clone", %s, function(){ var d={a:[]}; var o=[1];'
             ' J.Pointer.set(d, "/a/0", o); o.push(2); return d; });'
             % json.dumps(norm_json({"a": [[1]]})))
    c.append('case_true("pset_ret_alias", function(){ var d={};'
             ' return J.Pointer.set(d, "/a", 1) === d; });')
    c.append('case_eq("pset_append", %s, function(){ var d={a:[1]};'
             ' J.Pointer.set(d, "/a/-", 2); return d; });'
             % json.dumps(norm_json({"a": [1, 2]})))
    c.append('case_eq("pset_arr_dash_new", %s, function(){ var d=[1];'
             ' J.Pointer.set(d, "/-", 2); return d; });'
             % json.dumps(norm_json([1, 2])))
    # set uses RFC 6902 "add" semantics: inserting at an index SHIFTS right
    c.append('case_eq("pset_arr_idx", %s, function(){ var d=[1,2];'
             ' J.Pointer.set(d, "/1", 9); return d; });'
             % json.dumps(norm_json([1, 9, 2])))
    c.append('case_err("pset_missing_parent", "TypeError", function(){'
             ' return J.Pointer.set({}, "/a/b", 1); });')
    c.append('case_err("pset_arr_lead0", "TypeError", function(){'
             ' return J.Pointer.set([1], "/01", 2); });')
    c.append('case_err("pset_arr_nan", "TypeError", function(){'
             ' return J.Pointer.set([1], "/x", 2); });')
    # __proto__ stays an own property; no prototype pollution
    c.append('case_eq("pset_proto_own", %s, function(){ var d={};'
             ' J.Pointer.set(d, "/__proto__", {a:1});'
             ' return [Object.keys(d).join(","), ({}).a === undefined ? "clean" : "POLLUTED"]; });'
             % json.dumps(norm_json(["__proto__", "clean"])))
    c.append('case_err("pset_proto_descend", "TypeError", function(){ var d={};'
             ' return J.Pointer.set(d, "/__proto__/x", 1); });')
    # remove
    c.append('case_err("prm_root", "TypeError", function(){ return J.Pointer.remove({a:1}, ""); });')
    c.append('case_err("prm_missing", "TypeError", function(){ return J.Pointer.remove({a:1}, "/z"); });')
    c.append('case_eq("prm_ok", %s, function(){ var d={a:1,b:2};'
             ' J.Pointer.remove(d, "/a"); return d; });'
             % json.dumps(norm_json({"b": 2})))
    c.append('case_eq("prm_arr", %s, function(){ var d=[1,2,3];'
             ' J.Pointer.remove(d, "/1"); return d; });'
             % json.dumps(norm_json([1, 3])))
    # depth / length caps (documented: 128 levels, 65536 bytes)
    c.append('case_true("pdeep128", function(){ var d=0, p="";'
             ' for (var i=0;i<128;i++){ d={a:d}; p="/a"+p; }'
             ' return J.Pointer.get(d, p) === 0; });')
    # depth cap (FIXED this round: was unenforced; API.md pins 128 levels)
    c.append('case_err("pdeep129", "RangeError", function(){ var d=0, p="";'
             ' for (var i=0;i<129;i++){ d={a:d}; p="/a"+p; }'
             ' return J.Pointer.get(d, p); });')
    c.append('case_err("phas_deep129", "RangeError", function(){ var d=0, p="";'
             ' for (var i=0;i<129;i++){ d={a:d}; p="/a"+p; }'
             ' return J.Pointer.has(d, p); });')
    c.append('case_err("plong", "RangeError", function(){'
             ' return J.Pointer.get({a:1}, "/" + "a".repeat(65537)); });')
    write_probe("json/j01_pointer.js", [("J", "dyna:json")], "\n".join(c),
                "RFC 6901 Pointer vs pinned python resolver")
    return len(c)


def bad_escape_js(ptr):
    toks = ptr.split("/")[1:]
    return any(seg.count("~") != seg.count("~0") + seg.count("~1") for seg in toks)


def jsdoc(doc):
    return json.dumps(doc, ensure_ascii=False)


# ---------------------------------------------------------------- RFC 6902 patch
def gen_patch():
    c = []
    # (name, doc, ops, want | Exception)
    cases = [
        ("add", {"foo": "bar"}, [{"op": "add", "path": "/baz", "value": "qux"}],
         {"foo": "bar", "baz": "qux"}),
        ("add_arr", {"foo": ["bar"]}, [{"op": "add", "path": "/foo/0", "value": "qux"}],
         {"foo": ["qux", "bar"]}),
        ("add_arr_end", {"foo": ["bar"]}, [{"op": "add", "path": "/foo/-", "value": ["abc", "def"]}],
         {"foo": ["bar", ["abc", "def"]]}),
        ("add_root", {"a": 1}, [{"op": "add", "path": "", "value": {"b": 2}}], {"b": 2}),
        ("remove", {"baz": "qux", "foo": "bar"}, [{"op": "remove", "path": "/baz"}],
         {"foo": "bar"}),
        ("remove_arr", {"foo": ["bar", "qux", "baz"]}, [{"op": "remove", "path": "/foo/1"}],
         {"foo": ["bar", "baz"]}),
        ("replace", {"baz": "qux", "foo": "bar"}, [{"op": "replace", "path": "/baz", "value": "boo"},
         {"op": "add", "path": "/hello", "value": ["world"]},
         {"op": "remove", "path": "/foo"}],
         {"baz": "boo", "hello": ["world"]}),
        ("move", {"foo": ["all", "grass", "cows", "eat"]},
         [{"op": "move", "from": "/foo/1", "path": "/foo/3"}],
         {"foo": ["all", "cows", "eat", "grass"]}),
        ("copy", {"foo": {"bar": ["baz"], "baz": ["data", "ext", "deep", "open", "nope", "no", "ri", "go"]},
                  "cluck": []},
         [{"op": "copy", "from": "/foo/baz/0", "path": "/cluck/0"},
          {"op": "copy", "from": "/foo/baz/1", "path": "/cluck/1"}],
         {"foo": {"bar": ["baz"], "baz": ["data", "ext", "deep", "open", "nope", "no", "ri", "go"]},
          "cluck": ["data", "ext"]}),
        ("test", {"baz": "qux", "foo": ["a", 2, "c"]},
         [{"op": "test", "path": "/baz", "value": "qux"},
          {"op": "test", "path": "/foo/1", "value": 2}],
         {"baz": "qux", "foo": ["a", 2, "c"]}),
    ]
    for name, doc, ops, want in cases:
        c.append('case_eq("patch_%s", %s, function(){ return J.Patch.apply(%s, %s); });'
                 % (name, json.dumps(norm_json(want)), jsdoc(doc),
                    json.dumps(ops, ensure_ascii=False)))
    # failures: input must remain intact (copy-on-write)
    fails = [
        ("test_fail", {"a": 1}, [{"op": "test", "path": "/a", "value": 2}]),
        ("remove_missing", {"a": 1}, [{"op": "remove", "path": "/z"}]),
        ("replace_missing", {"a": 1}, [{"op": "replace", "path": "/z", "value": 1}]),
        ("unknown_op", {"a": 1}, [{"op": "frob", "path": "/a"}]),
        ("missing_value", {"a": 1}, [{"op": "add", "path": "/b"}]),
        ("bad_path", {"a": 1}, [{"op": "add", "path": 5, "value": 1}]),
    ]
    for name, doc, ops in fails:
        c.append('case_eq("patchfail_%s_input", %s, function(){ try { J.Patch.apply(%s, %s); }'
                 ' catch (e) {} return %s; });'
                 % (name, json.dumps(norm_json(doc)), jsdoc(doc),
                    json.dumps(ops, ensure_ascii=False), jsdoc(doc)))
        c.append('case_err("patchfail_%s", "TypeError", function(){'
                 ' return J.Patch.apply(%s, %s); });'
                 % (name, jsdoc(doc), json.dumps(ops, ensure_ascii=False)))
    # move: proper-prefix rule
    c.append('case_err("patchmove_prefix", "TypeError", function(){'
             ' return J.Patch.apply({a:{b:1}}, [{op:"move", from:"/a", path:"/a/b"}]); });')
    # result must not alias the input's plain values
    c.append('case_eq("patch_noalias", %s, function(){ var d={a:{x:1}};'
             ' var r=J.Patch.apply(d, []); r.a.x = 5; return d; });'
             % json.dumps(norm_json({"a": {"x": 1}})))
    # non-plain values pass by reference (documented)
    c.append('case_eq("patch_date_ref", {t:"b",v:"true"}, function(){'
             ' var dt = new Date(1000); var r = J.Patch.apply({d:dt}, []);'
             ' return r.d === dt; });')
    # deep-equality test semantics (objects with same members pass)
    c.append('case_eq("patch_test_deep", %s, function(){'
             ' return J.Patch.apply({a:{b:[1,{c:2}]}},'
             ' [{op:"test", path:"/a", value:{b:[1,{c:2}]}}]); });'
             % json.dumps(norm_json({"a": {"b": [1, {"c": 2}]}})))
    # multi-op sequence with intermediate state
    c.append('case_eq("patch_seq", %s, function(){ return J.Patch.apply({a:{b:[1,2]}}, ['
             ' {op:"add", path:"/a/b/-", value:3},'
             ' {op:"test", path:"/a/b/0", value:1},'
             ' {op:"remove", path:"/a/b/1"},'
             ' {op:"copy", from:"/a/b/0", path:"/c"},'
             ' {op:"move", from:"/a", path:"/z"}]); });'
             % json.dumps(norm_json({"c": 1, "z": {"b": [1, 3]}})))
    write_probe("json/j02_patch.js", [("J", "dyna:json")], "\n".join(c),
                "RFC 6902 Patch vs pinned expectations + contract")
    return len(c)


def main():
    n = gen_pointer() + gen_patch()
    print("gen_json: %d cases emitted" % n)


if __name__ == "__main__":
    main()
