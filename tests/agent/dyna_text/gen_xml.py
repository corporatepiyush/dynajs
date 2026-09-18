#!/usr/bin/env python3
"""gen_xml.py — dyna:xml black-box matrix generator.

Oracle: python xml.etree.ElementTree on the WELL-FORMED subset, normalized the
documented dyna:xml way (whitespace-only text nodes dropped — the engine's
trim:true reading; comments/PIs dropped — same as ET). Rejection rows use
documents ET also rejects (true differential); engine-lenient rows are pinned
with notes. SAX probes assert documented event streams incl. every chunk-split
offset (engine invariant, streaming correctness).
"""
import json
import xml.etree.ElementTree as ET
from gen_common import jslit, norm_json, norm_num, write_probe

X_IMPORTS = [("X", "dyna:xml")]

NS_DROP = lambda _: None


def et_to_tree(elem):
    """ET element -> dyna:xml tree shape (attrs, children: str | node),
    dropping whitespace-only text/tail (the engine's trim:true default)."""
    node = {"name": elem.tag, "attrs": dict(elem.attrib), "children": []}
    if elem.text and elem.text.strip():
        node["children"].append(elem.text)
    for child in elem:
        cn = et_to_tree(child)
        node["children"].append(cn)
        if child.tail and child.tail.strip():
            node["children"].append(child.tail)
    return node


WF_DOCS = [
    ("simple", "<a>t</a>"),
    ("nested", "<a><b><c>deep</c></b></a>"),
    ("attrs", '<a x="1" y=\'2\'>t</a>'),
    ("attr_escapes", '<a x="&lt;&amp;&quot;">t</a>'),
    ("mixed", "<a>one<b>two</b>three</a>"),
    ("empty_elems", "<a><b/><c></c></a>"),
    ("selfclose_attrs", '<br x="1"/>'),
    ("predef_entities", "<a>&lt;&gt;&amp;&quot;&apos;</a>"),
    ("numeric_dec", "<a>&#65;&#66;</a>"),
    ("numeric_hex", "<a>&#x42;&#x43;</a>"),
    ("astral_entity", "<a>&#x1F600;</a>"),
    ("cdata", "<a><![CDATA[x < y & z]]></a>"),
    ("cdata_special", "<a><![CDATA[]]]></a>"),
    ("comment", "<a><!--note--><b/></a>"),
    ("pi", "<a><?php echo 1; ?><b/></a>"),
    ("xml_decl", '<?xml version="1.0" encoding="UTF-8"?><a/>'),
    ("doctype", '<!DOCTYPE a SYSTEM "a.dtd"><a><b/></a>'),
    ("doctype_subset", "<!DOCTYPE a [<!ELEMENT a ANY>]><a/>"),
    ("comment_after_root", "<a/><!--bye-->"),
    ("ws_between_tags", "<a><b>x</b><c>y</c></a>"),
    ("astral_text", "<a>日本語 🌍 text</a>"),
    ("quotes_in_text", "<a>5 &lt; 6 &amp;&amp; 7 &gt; 3</a>"),
    ("many_attrs", "<a a1='1' a2='2' a3='3' a4='4' a5='5'/>"),
    ("deep8", "<a><b><c><d><e><f><g><h>x</h></g></f></e></d></c></b></a>"),
]

BAD_DOCS = [
    ("unclosed", "<a>"),
    ("unclosed_attr", "<a x=\"1>"),
    ("bad_nest", "<a><b></a></b>"),
    ("dup_attr", "<a x='1' x='2'/>"),
    ("two_roots", "<a/><b/>"),
    ("text_after_root", "<a/>tail"),
    ("attr_no_value", "<a x/>"),
    ("empty_doc", ""),
    ("unknown_entity", "<a>&foo;</a>"),
    ("bad_entity_ref", "<a>&#zz;</a>"),
    ("stray_close", "</a>"),
    ("lt_in_text", "<a>1 < 2</a>"),
    ("bad_name", "<1a/>"),
    ("unclosed_comment", "<a><!-- x</a>"),
]


def gen_parse_differential():
    c = []
    for name, text in WF_DOCS:
        et = ET.fromstring(text)
        want = et_to_tree(et)
        c.append('case_eq("wf_%s", %s, function(){ return X.XMLParse(%s); });'
                 % (name, json.dumps(norm_json(want)), jslit(text)))
        # round-trip stability: parse -> stringify -> parse -> stringify fixes
        c.append('case_true("rt_%s", function(){'
                 ' var t1 = X.XMLParse(%s); var s1 = X.XMLStringify(t1);'
                 ' var t2 = X.XMLParse(s1); var s2 = X.XMLStringify(t2);'
                 ' return JSON.stringify(t1) === JSON.stringify(t2) && s1 === s2; });'
                 % (name, jslit(text)))
    for name, text in BAD_DOCS:
        c.append('case_err("bad_%s", "SyntaxError", function(){ return X.XMLParse(%s); });'
                 % (name, jslit(text)))
    return c


def gen_options():
    c = []
    # trim:false keeps whitespace-only nodes (documented switch)
    c.append('case_eq("trim_off_keeps_ws", %s, function(){'
             ' return X.XMLParse("<a>  <b/>  <c/>  </a>", {trim:false}).children.length; });'
             % json.dumps(norm_num(5)))
    # trim:true drops whitespace-only nodes but leaves mixed text untrimmed
    # (IMPL-DEFINED reading of "trim" — pinned; API.md wording is loose here)
    c.append('case_eq("trim_ws_only_dropped", %s, function(){'
             ' return X.XMLParse("<a>  <b/>  <c/>  </a>").children.length; });'
             % json.dumps(norm_num(2)))
    c.append('case_eq("trim_mixed_kept_PINNED", %s, function(){'
             ' return X.XMLParse("<a>  hi  </a>").children; });'
             % json.dumps(norm_json(["  hi  "])))
    # entities keep mode (documented)
    c.append('case_eq("entities_keep", %s, function(){'
             ' return X.XMLParse("<a>&foo;</a>", {entities:"keep"}).children; });'
             % json.dumps(norm_json(["&foo;"])))
    # adjacent text (resolved + kept) merges into ONE child (pinned)
    c.append('case_eq("entities_keep_numeric", %s, function(){'
             ' return X.XMLParse("<a>&#65;&foo;</a>", {entities:"keep"}).children; });'
             % json.dumps(norm_json(["A&foo;"])))
    # attr value normalization: literal tab/CR/LF -> space; char refs survive
    # (XML 1.0 3.3.3 — character references bypass normalization)
    c.append('case_eq("attr_ws_normalize", %s, function(){'
             ' return X.XMLParse("<a t=\'x\\ty&#10;z\'/>").attrs; });'
             % json.dumps(norm_json({"t": "x y\nz"})))
    # __proto__ stays an own property (documented safety)
    c.append('case_eq("proto_attr_own", {t:"b",v:"true"}, function(){'
             ' var o = X.XMLParse("<a __proto__=\'x\'/>").attrs;'
             ' return o.hasOwnProperty("__proto__") && Object.keys(o).join(",") === "__proto__"; });')
    c.append('case_eq("proto_elem_own", {t:"b",v:"true"}, function(){'
             ' var o = X.XMLToObject(X.XMLParse("<r><__proto__>v</__proto__></r>"));'
             ' return Object.keys(o.r).join(",") === "__proto__" && ({}).v === undefined; });')
    # nesting boundary: 255 ok, 256 throws (impl-defined pin of "capped at 256")
    c.append('case_eq("nest_255_ok", {t:"b",v:"true"}, function(){'
             ' var s = "<a>".repeat(255) + "</a>".repeat(255);'
             ' return X.XMLParse(s).name === "a"; });')
    c.append('case_err("nest_256", "SyntaxError", function(){'
             ' var s = "<a>".repeat(256) + "</a>".repeat(256);'
             ' return X.XMLParse(s); });')
    return c


def gen_stringify_obj():
    c = []
    c.append('case_eq("str_minified", {t:"s",v:"<a x=\\"1\\">t</a>"}, function(){'
             ' return X.XMLStringify({name:"a", attrs:{x:"1"}, children:["t"]}); });')
    c.append('case_eq("str_selfclose", {t:"s",v:"<br/>"}, function(){'
             ' return X.XMLStringify({name:"br", attrs:{}, children:[]}); });')
    c.append('case_eq("str_indent", {t:"s",v:"<a>\\n  <b/>\\n</a>"}, function(){'
             ' return X.XMLStringify({name:"a", attrs:{}, children:[{name:"b", attrs:{}, children:[]}]}, {indent:2}); });')
    c.append('case_eq("str_escapes", {t:"s",v:"<a>1 &lt; 2 &amp; 3</a>"}, function(){'
             ' return X.XMLStringify({name:"a", attrs:{}, children:["1 < 2 & 3"]}); });')
    c.append('case_eq("str_attr_escape", {t:"s",v:"<a x=\\"&quot;q&quot;\\"/>"}, function(){'
             ' return X.XMLStringify({name:"a", attrs:{x:\'"q"\'}, children:[]}); });')
    c.append('case_eq("str_astral", {t:"s",v:"<a>🌍</a>"}, function(){'
             ' return X.XMLStringify({name:"a", attrs:{}, children:["🌍"]}); });')
    c.append('case_err("str_bad_name", "RangeError", function(){'
             ' return X.XMLStringify({name:"1bad", attrs:{}, children:[]}); });')
    c.append('case_err("str_deep", "RangeError", function(){'
             ' var n = {name:"leaf", attrs:{}, children:[]};'
             ' for (var i = 0; i < 300; i++) n = {name:"a", attrs:{}, children:[n]};'
             ' return X.XMLStringify(n); });')
    # XMLToObject collapse rules (documented)
    c.append('case_eq("obj_collapse", %s, function(){'
             ' return X.XMLToObject(X.XMLParse("<r a=\'1\'><x>1</x><x>2</x><y>3</y></r>")); });'
             % json.dumps(norm_json({"r": {"@a": "1", "x": ["1", "2"], "y": "3"}})))
    c.append('case_eq("obj_text", %s, function(){'
             ' return X.XMLToObject(X.XMLParse("<r>plain</r>")); });'
             % json.dumps(norm_json({"r": "plain"})))
    return c


def gen_sax():
    c = []
    doc = "<a x='1'>hi<b>there</b><!--c--><?pp d?><![CDATA[cd]]></a>"
    c.append('case_eq("sax_events", %s, function(){'
             ' var ev = [];'
             ' var p = new X.SAXParser({'
             '  onOpen: function(n, a){ ev.push(["open", n, a]); },'
             '  onClose: function(n){ ev.push(["close", n]); },'
             '  onText: function(t){ ev.push(["text", t]); },'
             '  onComment: function(t){ ev.push(["comment", t]); },'
             '  onPI: function(t, d){ ev.push(["pi", t, d]); },'
             '  onCData: function(t){ ev.push(["cdata", t]); }});'
             ' p.write(%s); p.end(); return ev; });'
             % (json.dumps(norm_json([
                 ["open", "a", {"x": "1"}], ["text", "hi"], ["open", "b", {}],
                 ["text", "there"], ["close", "b"], ["comment", "c"],
                 ["pi", "pp", "d"], ["cdata", "cd"], ["close", "a"]])), jslit(doc)))
    # EVERY single-char chunk boundary must produce the identical stream
    c.append('case_true("sax_split_everywhere", function(){'
             ' var ref = null;'
             ' for (var k = 1; k < %s.length; k++) {'
             '  var ev = [];'
             '  var p = new X.SAXParser({'
             '   onOpen: function(n, a){ ev.push(["open", n, a]); },'
             '   onClose: function(n){ ev.push(["close", n]); },'
             '   onText: function(t){ ev.push(["text", t]); }});'
             '  p.write(%s.slice(0, k)); p.write(%s.slice(k)); p.end();'
             '  var j = JSON.stringify(ev);'
             '  if (ref === null) ref = j; else if (j !== ref) return "differs at k=" + k;'
             ' }'
             ' return ref !== null; });' % (jslit(doc), jslit(doc), jslit(doc)))
    # entity split across chunks resolves correctly
    c.append('case_eq("sax_entity_split", %s, function(){'
             ' var ev = [];'
             ' var p = new X.SAXParser({onText: function(t){ ev.push(t); }});'
             ' p.write("<a>&am"); p.write("p;</a>"); p.end(); return ev; });'
             % json.dumps(norm_json(["&"])))
    # CDATA keyword split across chunks at every offset
    cdata_doc = "<a><![CDATA[hello]]></a>"
    c.append('case_true("sax_cdata_split_all", function(){'
             ' for (var k = 3; k <= 12; k++) {'
             '  var ev = [];'
             '  var p = new X.SAXParser({onCData: function(t){ ev.push(t); }});'
             '  p.write(%s.slice(0, k)); p.write(%s.slice(k)); p.end();'
             '  if (ev.join("") !== "hello") return "k=" + k;'
             ' } return true; });' % (jslit(cdata_doc), jslit(cdata_doc)))
    # discipline: write-in-handler, write-after-end, reend
    c.append('case_err("sax_write_in_handler", "TypeError", function(){'
             ' var p = new X.SAXParser({onOpen: function(){ p.write("<x/>"); }});'
             ' p.write("<a/>"); p.end(); return "ok"; });')
    c.append('case_err("sax_write_after_end", "TypeError", function(){'
             ' var p = new X.SAXParser({}); p.write("<a/>"); p.end(); p.write("<b/>"); });')
    c.append('case_err("sax_trailing", "SyntaxError", function(){'
             ' var p = new X.SAXParser({}); p.write("<a/>x"); p.end(); });')
    # root discipline (FIXED this round: SAX previously swallowed trailing
    # content and extra roots that XMLParse rejects)
    c.append('case_err("sax_two_roots", "SyntaxError", function(){'
             ' var p = new X.SAXParser({}); p.write("<a/><b/>"); p.end(); });')
    c.append('case_err("sax_two_roots_selfclose", "SyntaxError", function(){'
             ' var p = new X.SAXParser({}); p.write("<a/><b/>x"); p.end(); });')
    c.append('case_err("sax_prolog_junk", "SyntaxError", function(){'
             ' var p = new X.SAXParser({}); p.write("junk<a/>"); p.end(); });')
    c.append('case_eq("sax_trailing_ws_ok", {t:"u"}, function(){'
             ' var p = new X.SAXParser({}); p.write("<a/>  "); return p.end(); });')
    c.append('case_eq("sax_comment_after_root_ok", {t:"u"}, function(){'
             ' var p = new X.SAXParser({onComment:function(){}});'
             ' p.write("<a/><!--c-->"); return p.end(); });')
    # stray close before/after the root: same refusal as XMLParse
    c.append('case_err("sax_stray_close_pre", "SyntaxError", function(){'
             ' var p = new X.SAXParser({}); p.write("</a><b/>"); p.end(); });')
    c.append('case_err("sax_stray_close_post", "SyntaxError", function(){'
             ' var p = new X.SAXParser({}); p.write("<a></a></b>"); p.end(); });')
    # end()-of-document discipline (REVIEW-FIXES: mirrors XMLParse exactly)
    c.append('case_err("sax_end_unclosed", "SyntaxError", function(){'
             ' var p = new X.SAXParser({}); p.write("<a>hi"); p.end(); });')
    c.append('case_err("sax_end_noroot_comment", "SyntaxError", function(){'
             ' var p = new X.SAXParser({}); p.write("<!-- c -->"); p.end(); });')
    c.append('case_err("sax_end_noroot_empty", "SyntaxError", function(){'
             ' var p = new X.SAXParser({}); p.write(""); p.end(); });')
    c.append('case_err("sax_end_noroot_pi", "SyntaxError", function(){'
             ' var p = new X.SAXParser({}); p.write("<?p d?>"); p.end(); });')
    c.append('case_err("sax_end_noroot_ws", "SyntaxError", function(){'
             ' var p = new X.SAXParser({}); p.write("  "); p.end(); });')
    c.append('case_eq("sax_end_root_ok", {t:"u"}, function(){'
             ' var p = new X.SAXParser({}); p.write("<a/>"); return p.end(); });')
    c.append('case_err("sax_needs_handlers", "TypeError", function(){'
             ' return new X.SAXParser(null); });')
    c.append('case_err("sax_bad_handler", "TypeError", function(){'
             ' return new X.SAXParser({onOpen: 5}); });')
    c.append('case_eq("sax_decl_ignored", %s, function(){'
             ' var ev = [];'
             ' var p = new X.SAXParser({onOpen: function(n){ ev.push(n); }});'
             ' p.write(\'<?xml version="1.0"?><a/>\'); p.end(); return ev; });'
             % json.dumps(norm_json(["a"])))
    return c


def main():
    a = gen_parse_differential()
    write_probe("xml/x01_parse_differential.js", X_IMPORTS, "\n".join(a),
                "XMLParse vs python ElementTree + round-trip + rejection")
    b = gen_options()
    write_probe("xml/x02_options.js", X_IMPORTS, "\n".join(b), "trim/entities/nesting pins")
    d = gen_stringify_obj()
    write_probe("xml/x03_stringify_objtoobj.js", X_IMPORTS, "\n".join(d),
                "XMLStringify + XMLToObject collapse rules")
    e = gen_sax()
    write_probe("xml/x04_sax.js", X_IMPORTS, "\n".join(e), "SAXParser streaming + discipline")
    n = sum(len([x for x in c if x.strip().startswith("case_")]) for c in (a, b, d, e))
    print("gen_xml: %d cases emitted" % n)


if __name__ == "__main__":
    main()
