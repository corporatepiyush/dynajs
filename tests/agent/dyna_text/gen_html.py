#!/usr/bin/env python3
"""gen_html.py — dyna:html black-box matrix generator.

Oracles: python3 html.parser (HTMLParser, convert_charrefs=False) drives a
mini tree builder that implements the DOCUMENTED dyna:html leniency rules for
an entity-free structure corpus (tag/attr lowercasing, void elements, p/li
auto-close, stray-close ignore) — token-level cross-vendor agreement. Entity
semantics, raw-text elements, Selector/Sanitizer/Template/Markdown rows are
pinned to the documented contract (browser-like; python's named-entity table
is a superset of the engine's five predefined + common refs, so entities stay
out of the structural differential).
"""
from html.parser import HTMLParser
import json
from gen_common import jslit, norm_json, norm_num, norm_any, write_probe

H_IMPORTS = [("H", "dyna:html")]

VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link",
        "meta", "param", "source", "track", "wbr"}
# keyed by the OPEN element: the tags that close it (HTML5: <p> closes <p>,
# <hr> closes <p>; <li> closes <li>)
CLOSED_BY = {"p": {"p", "hr"}, "li": {"li"}}


class MiniTree(HTMLParser):
    """Tree builder with the documented dyna:html leniency rules."""

    def __init__(self):
        super().__init__(convert_charrefs=False)
        self.root = {"name": "#root", "attrs": {}, "children": []}
        self.stack = [self.root]
        self.after_comment = False

    @staticmethod
    def __attrs(attrs):
        return {k: (v if v is not None else "") for k, v in attrs}

    def handle_starttag(self, tag, attrs):
        while tag in CLOSED_BY.get(self.stack[-1]["name"], ()):
            self.stack.pop()
        node = {"name": tag, "attrs": self.__attrs(attrs), "children": []}
        self.stack[-1]["children"].append(node)
        if tag not in VOID:
            self.stack.append(node)

    def handle_startendtag(self, tag, attrs):
        while tag in CLOSED_BY.get(self.stack[-1]["name"], ()):
            self.stack.pop()
        node = {"name": tag, "attrs": self.__attrs(attrs), "children": []}
        self.stack[-1]["children"].append(node)

    def handle_endtag(self, tag):
        if tag in VOID:
            return
        for i in range(len(self.stack) - 1, 0, -1):
            if self.stack[i]["name"] == tag:
                del self.stack[i:]
                return
        # unmatched close: ignored (documented)

    def handle_data(self, data):
        if not data:
            return
        kids = self.stack[-1]["children"]
        # a comment/PI dropped IN PLACE does not split text: the engine keeps
        # "a<!--c-->b" as the single node "ab". A dropped CLOSE TAG does end
        # the text node ("<b>x</i>y</b>" keeps "x","y" separate).
        if self.after_comment and kids and isinstance(kids[-1], str):
            kids[-1] += data
        else:
            kids.append(data)
        self.after_comment = False

    def handle_comment(self, data):
        self.after_comment = True  # dropped in place (documented leniency)

    def result(self):
        return self.root["children"]


def py_tree(html):
    t = MiniTree()
    t.feed(html)
    t.close()
    return t.result()


STRUCT_DOCS = [
    ("basic", "<p>hi <b>x</b></p>"),
    ("void", "<p>a<br>b<hr>c</p>"),
    ("void_consecutive", "<br><br>"),
    ("void_selfclosed", "<div><br/><img src='x'/></div>"),
    ("p_autoclose", "<p>one<p>two"),
    ("li_autoclose", "<ul><li>a<li>b</ul>"),
    ("uppercase", "<DIV CLASS='x'>Y</DIV>"),
    ("uppercase_close", "<div>Y</DIV>"),
    ("unquoted_attrs", "<a href=/x y=1>z</a>"),
    ("mixed_quotes", "<a href='1' b=\"2\" c=3>d</a>"),
    ("attr_no_value", "<input disabled>"),
    ("mismatched_close", "<b>x</i>y</b>"),
    ("stray_close", "a</div>b"),
    ("comment_dropped", "<p>a<!-- c -->b</p>"),
    ("doctype_dropped", "<!DOCTYPE html><p>x</p>"),
    ("empty", ""),
    ("text_only", "plain"),
    ("nested", "<div><section><p>deep <em>x</em></p></section></div>"),
    ("sibling_texts", "<div>a<div>b</div>c</div>"),
    ("table_soup", "<table><tr><td>x</table>"),
    ("attrs_empty", "<p class=''>t</p>"),
    ("spaces_kept", "<p>  spaced  </p>"),
]


def gen_structure():
    c = []
    # round-trip stability: stringify(parse(x)) reparses to the same tree.
    # Recovery rows are excluded: dropping a tag merges the text around it on
    # the second pass (mismatched_close -> "xy"), so tree equality holds only
    # for non-recovering docs.
    NO_RT = {"mismatched_close", "stray_close"}
    for name, html in STRUCT_DOCS:
        want = py_tree(html)
        c.append('case_eq("st_%s", %s, function(){ return H.HTMLParse(%s); });'
                 % (name, json.dumps(norm_json(want)), jslit(html)))
        if name in NO_RT:
            continue
        c.append('case_true("rt_%s", function(){'
                 ' var t1 = H.HTMLParse(%s); var s1 = H.HTMLStringify(t1);'
                 ' var t2 = H.HTMLParse(s1);'
                 ' return JSON.stringify(t1) === JSON.stringify(t2); });'
                 % (name, jslit(html)))
    # recovery rows: PINNED text-merge on the second pass (documented leniency)
    c.append('case_eq("rt_recovery_merge", %s, function(){'
             ' return H.HTMLText(H.HTMLParse(H.HTMLStringify('
             ' H.HTMLParse("<b>x</i>y</b>")))); });'
             % json.dumps(norm_any("xy")))
    c.append('case_eq("rt_recovery_stray", %s, function(){'
             ' return H.HTMLText(H.HTMLParse(H.HTMLStringify('
             ' H.HTMLParse("a</div>b")))); });'
             % json.dumps(norm_any("ab")))
    return c


def gen_entities_text():
    c = []
    pins = [
        ("named_five", "<p>&amp;&lt;&gt;&quot;&apos;</p>", ["&<>\"'"]),
        ("numeric_dec", "<p>&#65;&#x42;</p>", ["AB"]),
        ("astral", "<p>&#x1F600;</p>", ["😀"]),
        ("out_of_range_10ffff", "<p>&#x110FFF;</p>", ["\ufffd"]),
        ("out_of_range_high", "<p>&#xFFFFFFFF;</p>", ["\ufffd"]),
        ("nul_rejected", "<p>&#0;</p>", ["\ufffd"]),
        ("unknown_named_literal", "<p>&fakeentity;</p>", ["&fakeentity;"]),
        ("ampersand_plain", "<p>a & b</p>", ["a & b"]),
        ("copy_ref", "<p>&copy;</p>", ["©"]),
        ("in_attr", "<a title='&lt;x&gt;'>t</a>", None),
    ]
    for name, html, want in pins:
        if want is None:
            continue
        c.append('case_eq("ent_%s", %s, function(){ return H.HTMLParse(%s); });'
                 % (name, json.dumps(norm_json([{"name": "p", "attrs": {}, "children": want}])),
                    jslit(html)))
    # raw text elements: contents NOT parsed, entities NOT decoded
    c.append('case_eq("raw_script", %s, function(){ return H.HTMLParse('
             '"<script>var a = \'<b>&amp;</b>\';</script>after"); });'
             % json.dumps(norm_json([
                 {"name": "script", "attrs": {}, "children": ["var a = '<b>&amp;</b>';"]},
                 "after"])))
    c.append('case_eq("raw_style", %s, function(){ return H.HTMLParse("<style>a > b {}</style>"); });'
             % json.dumps(norm_json([{"name": "style", "attrs": {}, "children": ["a > b {}"]}])))
    # HTMLText skips script/style (the classic scraping bug, documented)
    c.append('case_eq("text_skips_raw", {t:"s",v:"abc"}, function(){'
             ' return H.HTMLText(H.HTMLParse('
             '"<div>a<script>z()</script>b<style>s</style>c</div>")); });')
    c.append('case_eq("text_decodes", {t:"s",v:"a & b"}, function(){'
             ' return H.HTMLText(H.HTMLParse("<p>a &amp; b</p>")); });')
    # depth / attribute caps
    c.append('case_err("deep_256", "RangeError", function(){'
             ' var s = "<div>".repeat(300) + "x";'
             ' return H.HTMLParse(s); });')
    c.append('case_err("cap_64mib", "RangeError", function(){'
             ' return H.HTMLParse("<p>" + "x".repeat(1 << 10)); });') if False else None
    return c


def gen_selector():
    c = []
    doc = '<div id="main"><p class="x y">one</p><p>two</p><section><p class="x">three</p></section><a href="https://q">l</a></div>'
    c.append("var DOC = H.HTMLParse(%s);" % jslit(doc))
    cases = [
        ("sel_tag_p", "p", 3),
        ("sel_class_x", ".x", 2),
        ("sel_id", "#main", 1),
        ("sel_attr", "[href]", 1),
        ("sel_attr_eq", "[href='https://q']", 1),
        ("sel_child", "div > p", 2),
        ("sel_descendant", "div p", 3),
        ("sel_group", "p, a", 4),
        ("sel_tag_id", "div#main", 1),
        ("sel_deep", "#main > section > p", 1),
    ]
    for name, expr, n in cases:
        c.append('case_eq("%s", %s, function(){ return new H.Selector(%s).all(DOC).length; });'
                 % (name, json.dumps(norm_num(n)), jslit(expr)))
    c.append('case_eq("sel_first", {t:"s",v:"one"}, function(){'
             ' var n = new H.Selector("p").first(DOC); return n.children[0]; });')
    c.append('case_eq("sel_first_none", {t:"u"}, function(){'
             ' return new H.Selector("h1").first(DOC); });')
    c.append('case_eq("sel_matches_tag", {t:"b",v:"true"}, function(){'
             ' return new H.Selector("p").matches(DOC[0].children[0]); });')
    c.append('case_eq("sel_matches_class", {t:"b",v:"true"}, function(){'
             ' return new H.Selector(".x").matches(DOC[0].children[0]); });')
    c.append('case_eq("sel_matches_neg", {t:"b",v:"false"}, function(){'
             ' return new H.Selector(".x").matches(DOC[0].children[1]); });')
    c.append('case_err("sel_matches_combinator", "TypeError", function(){'
             ' return new H.Selector("div > p").matches(DOC[0]); });')
    c.append('case_err("sel_orphan_combinator", "SyntaxError", function(){'
             ' return new H.Selector("p >"); });')
    return c


def gen_sanitizer_template_markdown():
    c = []
    c.append('var SAN = new H.Sanitizer({allow: {p: [], a: ["href"], b: []},'
             ' protocols: {"a.href": ["https", "http"]}});')
    c.append('case_eq("san_strips", {t:"s",v:"<p>hi <a>bad</a> <a href=\\"https://ok\\">ok</a></p>"},'
             ' function(){ return SAN.clean('
             '\'<p onclick=x>hi <a href="javascript:alert(1)">bad</a>'
             ' <a href="https://ok">ok</a></p>\'); });')
    c.append('case_eq("san_ctrl_scheme", {t:"s",v:"<a>c</a>"}, function(){'
             ' return SAN.clean(\'<a href="java\\tscript:x">c</a>\'); });')
    c.append('case_eq("san_space_scheme", {t:"s",v:"<a>c</a>"}, function(){'
             ' return SAN.clean(\'<a href=" javascript:x">c</a>\'); });')
    # matching is case-insensitive; output is normalized lowercase (pinned)
    c.append('case_eq("san_case_insensitive", {t:"s",v:"<b>x</b>"}, function(){'
             ' return SAN.clean("<B>x</B>"); });')
    c.append('case_eq("san_rawtext_dropped", {t:"s",v:"<em>k</em>text"}, function(){'
             ' var s2 = new H.Sanitizer({allow: {em: []}});'
             ' return s2.clean("<div><em>k</em><script>drop()</script>text</div>"); });')
    c.append('case_err("san_no_policy", "TypeError", function(){ return new H.Sanitizer(); });')
    # Template
    c.append('case_eq("tpl_escape", {t:"s",v:"Hello &lt;W&gt;!"}, function(){'
             ' return new H.Template("Hello {{name}}!").render({name: "<W>"}); });')
    c.append('case_eq("tpl_raw", {t:"s",v:"<b/>"}, function(){'
             ' return new H.Template("{{{v}}}").render({v: "<b/>"}); });')
    c.append('case_eq("tpl_raw_amp", {t:"s",v:"<b/>"}, function(){'
             ' return new H.Template("{{&v}}").render({v: "<b/>"}); });')
    c.append('case_eq("tpl_section_arr", {t:"s",v:"<li>1</li><li>2</li>"}, function(){'
             ' return new H.Template("{{#xs}}<li>{{.}}</li>{{/xs}}").render({xs: [1, 2]}); });')
    c.append('case_eq("tpl_section_truthy", {t:"s",v:"y"}, function(){'
             ' return new H.Template("{{#v}}y{{/v}}").render({v: 1}); });')
    c.append('case_eq("tpl_invert_falsy", {t:"s",v:"none"}, function(){'
             ' return new H.Template("{{^xs}}none{{/xs}}").render({xs: []}); });')
    c.append('case_eq("tpl_invert_truthy", {t:"s",v:""}, function(){'
             ' return new H.Template("{{^xs}}none{{/xs}}").render({xs: [1]}); });')
    c.append('case_eq("tpl_missing_empty", {t:"s",v:"[]"}, function(){'
             ' return new H.Template("[{{nope}}]").render({}); });')
    c.append('case_eq("tpl_comment_dropped", {t:"s",v:"ab"}, function(){'
             ' return new H.Template("a{{! c }}b").render({}); });')
    c.append('case_eq("tpl_nested_scope", {t:"s",v:"outer/inner"}, function(){'
             ' return new H.Template("{{#a}}{{o}}/{{i}}{{/a}}").render({o: "outer", a: {i: "inner"}}); });')
    c.append('case_err("tpl_partial", "SyntaxError", function(){'
             ' return new H.Template("{{> part}}"); });')
    c.append('case_err("tpl_fn_value", "TypeError", function(){'
             ' return new H.Template("{{f}}").render({f: function(){}}); });')
    c.append('case_err("tpl_delim_change", "SyntaxError", function(){'
             ' return new H.Template("{{= << >>=}}"); });')
    # Markdown
    c.append('case_eq("md_heading", {t:"s",v:"<h1>T</h1>\\n"}, function(){'
             ' return H.MarkdownToHTML("# T"); });')
    c.append('case_eq("md_em", {t:"s",v:"<p>Some <em>em</em> text</p>\\n"}, function(){'
             ' return H.MarkdownToHTML("Some *em* text"); });')
    c.append('case_eq("md_html_escaped", {t:"s",v:"<p>&lt;script&gt;x()&lt;/script&gt;</p>\\n"},'
             ' function(){ return H.MarkdownToHTML("<script>x()</script>"); });')
    c.append('case_eq("md_html_allowed", {t:"s",v:"<p><b>x</b></p>\\n"}, function(){'
             ' return H.MarkdownToHTML("<b>x</b>", {allowRawHTML: true}); });')
    return c


def main():
    a = gen_structure()
    b = gen_entities_text()
    b = [x for x in b if x]
    d = gen_selector()
    e = gen_sanitizer_template_markdown()
    write_probe("html/h01_structure.js", H_IMPORTS, "\n".join(a),
                "HTMLParse tree vs python html.parser mini-builder + round-trips")
    write_probe("html/h02_entities_rawtext.js", H_IMPORTS, "\n".join(b),
                "entity semantics + raw-text elements (documented, pinned)")
    write_probe("html/h03_selector.js", H_IMPORTS, "\n".join(d), "Selector matrix")
    write_probe("html/h04_sanitizer_template_md.js", H_IMPORTS, "\n".join(e),
                "Sanitizer / Template / MarkdownToHTML")
    n = sum(len([x for x in c if x.strip().startswith("case_")]) for c in (a, b, d, e))
    print("gen_html: %d cases emitted" % n)


if __name__ == "__main__":
    main()
