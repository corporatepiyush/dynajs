/* test_template.js -- the Mustache-shaped Template in dyna:html (design 25).
 *
 * A template engine's security property is that {{x}} ESCAPES and {{{x}}} does
 * not, so the payload cases assert the exact output of both. The other half is
 * Mustache's falsiness rule, where an EMPTY ARRAY is falsy -- the one case a
 * plain truthiness test gets wrong, and the one that decides whether a list
 * section renders its body once or not at all.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_template.js
 */
import { Template } from "dyna:html";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}
const R = (s, d, o) => new Template(s, o).render(d);

/* --------------------------------------------------------- interpolation */

eq(R("plain", {}), "plain", "text with no tags");
eq(R("Hi {{name}}!", { name: "Ada" }), "Hi Ada!", "a variable");
eq(R("{{ name }}", { name: "x" }), "x", "whitespace inside the tag is trimmed");
eq(R("{{a}}{{b}}", { a: "1", b: "2" }), "12", "two in a row");
eq(R("{{n}}", { n: 42 }), "42", "a number");
eq(R("{{n}}", { n: 0 }), "0", "including zero");
eq(R("{{b}}", { b: false }), "false", "and false, which is a VALUE here");
eq(R("{{missing}}|", {}), "|", "a missing name renders as nothing");
eq(R("{{a.b.c}}", { a: { b: { c: "deep" } } }), "deep", "a dotted path");
eq(R("{{a.b}}|", { a: {} }), "|", "a path that runs out renders as nothing");
eq(R("{{! a note }}ok", {}), "ok", "a comment emits nothing");
eq(R("{{x}}", {}), "", "an empty result");

/* THE ESCAPING, which is the whole reason this lives beside the sanitizer. */
{
    const dirty = '<script>alert("x")</script>&';
    eq(R("{{v}}", { v: dirty }),
       "&lt;script&gt;alert(&quot;x&quot;)&lt;/script&gt;&amp;",
       "{{x}} escapes -- including the quote, because the landing context is "
       + "not knowable from the template");
    eq(R("{{{v}}}", { v: dirty }), dirty, "{{{x}}} does not");
    eq(R("{{&v}}", { v: dirty }), dirty, "and {{&x}} is the other spelling of raw");
    eq(R("{{v}}", { v: dirty }, { escape: false }), dirty,
       "escape:false is for the non-HTML case, and is explicit");
    eq(R('<a href="{{u}}">', { u: '" onx="1' }), '<a href="&quot; onx=&quot;1">',
       "a double quote cannot end the attribute it lands in");
    eq(R("<a href='{{u}}'>", { u: "' onx='1" }), "<a href='&#39; onx=&#39;1'>",
       "and neither can a single one -- a template does not know which "
       + "delimiter its value landed between");
}

/* -------------------------------------------------------------- sections */

eq(R("{{#s}}yes{{/s}}", { s: true }), "yes", "a truthy section renders once");
eq(R("{{#s}}yes{{/s}}", { s: false }), "", "a falsy one renders nothing");
eq(R("{{#s}}yes{{/s}}", {}), "", "and so does a missing one");
eq(R("{{#items}}[{{.}}]{{/items}}", { items: [1, 2, 3] }), "[1][2][3]",
   "an array iterates, and {{.}} is the item");
eq(R("{{#items}}x{{/items}}", { items: [] }), "",
   "AN EMPTY ARRAY IS FALSY -- a plain truthiness test renders the body once");
eq(R("{{^items}}none{{/items}}", { items: [] }), "none",
   "which is what makes the inverted section useful");
eq(R("{{^items}}none{{/items}}", { items: [1] }), "", "and it inverts");
eq(R("{{^missing}}none{{/missing}}", {}), "none", "on a missing name too");
eq(R("{{#u}}{{name}}{{/u}}", { u: { name: "a" } }), "a", "an object pushes a scope");
eq(R("{{#u}}{{name}}@{{host}}{{/u}}", { u: { name: "a" }, host: "h" }), "a@h",
   "and the OUTER scope is still visible through it");
eq(R("{{#u}}{{x}}{{/u}}", { u: { x: "inner" }, x: "outer" }), "inner",
   "with the innermost winning");
eq(R("{{#a}}{{#b}}{{v}}{{/b}}{{/a}}", { a: { b: { v: "n" } } }), "n", "nested sections");
eq(R("{{#rows}}{{#cols}}{{.}}{{/cols}};{{/rows}}",
     { rows: [{ cols: [1, 2] }, { cols: [3] }] }), "12;3;",
   "an array of objects each with an array");
eq(R("{{#s}}a{{/s}}b{{#s}}c{{/s}}", { s: true }), "abc", "two sections in sequence");

/* ------------------------------------------- it is data, not an evaluator */

throws(() => R("{{f}}", { f: () => "x" }),
       "a function is REFUSED: a template that calls one is an evaluator");
eq(R("{{x}}|", { get x() { return "ran"; } }), "|",
   "an accessor is not read, so it renders as nothing rather than running");
eq(R("{{#s}}x{{/s}}", { get s() { return true; } }), "",
   "in a section too");

/* ------------------------------------------------------------- refusals */

for (const bad of ["{{", "{{a", "{{{a}}", "{{#s}}", "{{/s}}", "{{#a}}{{/b}}",
                   "{{}}", "{{ }}", "{{> partial}}", "{{=<% %>=}}", "{{#}}"])
    throws(() => new Template(bad), "refuses " + JSON.stringify(bad));
throws(() => new Template(42), "the source must be a string");
throws(() => new Template(), "and is required");
throws(() => Template.prototype.render.call({}, {}), "a foreign receiver");
eq(new Template("{{x}}").render(), "", "render with no data is not a crash");
{
    /* Nesting is bounded rather than a blown C stack. */
    let deep = "";
    for (let i = 0; i < 100; i++) deep += "{{#s}}";
    for (let i = 0; i < 100; i++) deep += "{{/s}}";
    throws(() => new Template(deep), "sections nested past the cap are refused");
}
{
    /* A compiled template is reusable -- that is why it is a class. */
    const t = new Template("{{greet}}, {{name}}!");
    eq(t.render({ greet: "Hi", name: "a" }), "Hi, a!", "first render");
    eq(t.render({ greet: "Yo", name: "b" }), "Yo, b!", "second, same program");
}

if (fails) {
    print("test_template: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_template failed");
}
print("test_template: " + n + " assertions, 0 failures");
