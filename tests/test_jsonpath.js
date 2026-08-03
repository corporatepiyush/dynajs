/* test_jsonpath.js -- the RFC 9535 subset in dyna:encoding (design 09).
 *
 * Two oracles rather than a table of expected answers:
 *   - slices are diffed against Array.prototype.slice over every (start, end)
 *     pair in [-7, 7], which is an INDEPENDENT implementation of the same
 *     bounds algorithm rather than a copy of mine;
 *   - `$..*` is diffed against a recursive walker written here from the RFC's
 *     definition, compared by identity so a wrong grouping cannot pass.
 *
 * The decisive safety case is that a getter is SKIPPED, never called: a query
 * is a data read, and a document that runs code when you look at it is the
 * hole this module exists not to have.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_jsonpath.js
 */
import { JSONPath } from "dyna:encoding";

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
const q = (e) => new JSONPath(e);
const all = (e, v) => q(e).all(v);
/* Compare as JSON so a wrong ORDER fails, not just a wrong set. */
function same(e, v, want, msg) {
    eq(JSON.stringify(all(e, v)), JSON.stringify(want), msg);
}

const doc = {
    store: {
        book: [
            { category: "reference", author: "Nigel Rees",
              title: "Sayings of the Century", price: 8.95 },
            { category: "fiction", author: "Evelyn Waugh",
              title: "Sword of Honour", price: 12.99 },
            { category: "fiction", author: "Herman Melville",
              title: "Moby Dick", isbn: "0-553-21311-3", price: 8.99 },
            { category: "fiction", author: "J. R. R. Tolkien",
              title: "The Lord of the Rings", isbn: "0-395-19395-8", price: 22.99 }
        ],
        bicycle: { color: "red", price: 399 }
    }
};

/* ------------------------------------------------------- child selectors */

same("$", 42, [42], "the root of a scalar is the scalar");
same("$.store.bicycle.color", doc, ["red"], "a dotted path");
same("$['store']['bicycle']['color']", doc, ["red"], "the bracket spelling");
same('$["store"]["bicycle"]["color"]', doc, ["red"], "double quotes too");
same("$.store.nothing", doc, [], "a missing member selects nothing");
same("$.store.book[1].title", doc, ["Sword of Honour"], "an index step");
same("$.store.book[-1].title", doc, ["The Lord of the Rings"], "a negative index");
same("$.store.book[9]", doc, [], "an index past the end selects nothing");
same("$.store.book[-9]", doc, [], "and before the start");
same("$.store.book.title", doc, [], "a name selector on an array selects nothing");
/* A name is a name even when it looks like an operator or a number. */
same("$['a-b']", { "a-b": 1 }, [1], "a name with a dash needs brackets");
same("$['2']", { "2": "s" }, ["s"], "a numeric-looking name in quotes is a name");
same("$['a\\'b']", { "a'b": 7 }, [7], "an escaped quote inside a name");
same("$['\\u00e9']", { "é": 1 }, [1], "a \\u escape in a name");
same("$['\\ud83d\\ude00']", { "\u{1F600}": 1 }, [1], "a surrogate pair is one code point");
same("$.café", { "café": 1 }, [1], "a non-ASCII shorthand name");

/* --------------------------------------------------------------- wildcard */

same("$.store.book[*].author", doc,
     ["Nigel Rees", "Evelyn Waugh", "Herman Melville", "J. R. R. Tolkien"],
     "wildcard over an array");
same("$.store.*", doc, [doc.store.book, doc.store.bicycle], "wildcard over an object");
eq(all("$.*", { a: 1, b: 2 }).length, 2, "the .* spelling");
same("$[*]", [1, 2, 3], [1, 2, 3], "wildcard over the root array");
same("$[*]", 5, [], "wildcard over a scalar selects nothing");

/* ------------------------------------------------------------------ union */

same("$.store.book[0,2].title", doc, ["Sayings of the Century", "Moby Dick"],
     "an index union, in the order WRITTEN");
same("$.store.book[2,0].title", doc, ["Moby Dick", "Sayings of the Century"],
     "which is not sorted");
same("$['a','b']", { a: 1, b: 2 }, [1, 2], "a name union");
same("$[0,0]", [7], [7, 7], "a repeated selector yields the node twice");

/* ------------------------------------------------------------------ slice */

/* THE ORACLE: Array.prototype.slice is an independent implementation of the
 * same bounds algorithm for step 1, including every negative combination. */
{
    const a = ["a", "b", "c", "d", "e"];
    let checked = 0, bad = 0;
    for (let i = -7; i <= 7; i++) {
        for (let j = -7; j <= 7; j++) {
            const got = all("$[" + i + ":" + j + "]", a);
            const want = a.slice(i, j);
            checked++;
            if (JSON.stringify(got) !== JSON.stringify(want)) {
                bad++;
                if (bad < 4)
                    print("  slice [" + i + ":" + j + "] got " + JSON.stringify(got)
                          + " want " + JSON.stringify(want));
            }
        }
    }
    assert(bad === 0, "every [i:j] matches Array.slice (" + (checked - bad)
                      + "/" + checked + ")");
    assert(checked === 225, "the slice sweep ran all 225 pairs");
}
{
    const a = [0, 1, 2, 3, 4, 5];
    same("$[:3]", a, [0, 1, 2], "an omitted start");
    same("$[3:]", a, [3, 4, 5], "an omitted end");
    same("$[:]", a, a, "both omitted");
    same("$[::2]", a, [0, 2, 4], "a step");
    same("$[1:5:2]", a, [1, 3], "start, end and step");
    same("$[::-1]", a, [5, 4, 3, 2, 1, 0], "a negative step reverses");
    same("$[5:1:-2]", a, [5, 3], "a negative step with bounds");
    same("$[::0]", a, [], "a zero step selects nothing");
    same("$[1:1]", a, [], "an empty range");
    same("$[4:1]", a, [], "a backwards range with a positive step");
    same("$[ 1 : 3 ]", a, [1, 2], "whitespace inside the brackets");
}
same("$[0:2]", { a: 1 }, [], "a slice over an object selects nothing");

/* ------------------------------------------------------------- descendant */

same("$..author", doc,
     ["Nigel Rees", "Evelyn Waugh", "Herman Melville", "J. R. R. Tolkien"],
     "descendant name");
same("$.store..price", doc, [8.95, 12.99, 8.99, 22.99, 399],
     "descendant under a subtree, in document order");
same("$..book[2].title", doc, ["Moby Dick"], "a descendant segment then a child");
same("$..[0].title", doc, ["Sayings of the Century"], "a descendant index");
same("$..nothing", doc, [], "a descendant name that is absent");

/* THE SECOND ORACLE: the RFC's definition of a descendant segment spelled out
 * in JS -- visit the node and its descendants in preorder, and apply the
 * selector to each. That ORDER is the part an implementation gets wrong. */
{
    const preorder = (v, out) => {
        out.push(v);
        if (Array.isArray(v)) for (const x of v) preorder(x, out);
        else if (v && typeof v === "object")
            for (const k of Object.keys(v)) preorder(v[k], out);
        return out;
    };
    const children = (v) => Array.isArray(v) ? v.slice()
        : (v && typeof v === "object" ? Object.keys(v).map((k) => v[k]) : []);
    const want = [];
    for (const nd of preorder(doc, [])) for (const c of children(nd)) want.push(c);
    const got = all("$..*", doc);
    eq(got.length, want.length, "$..* visits every non-root node");
    let bad = 0;
    for (let i = 0; i < Math.min(got.length, want.length); i++)
        if (got[i] !== want[i]) bad++;
    assert(bad === 0, "and in the same order, compared by IDENTITY (" + bad + " differ)");
    assert(want.length === 27, "the reference walker itself found 27 nodes");
}

/* ----------------------------------------------------------------- filter */

eq(all("$..book[?@.isbn]", doc).length, 2, "an existence test");
eq(all("$..book[?!@.isbn]", doc).length, 2, "negated");
same("$..book[?@.price<10].title", doc, ["Sayings of the Century", "Moby Dick"],
     "a numeric comparison");
same("$..book[?(@.price < 10)].title", doc,
     ["Sayings of the Century", "Moby Dick"], "the parenthesised spelling");
same("$..book[?@.category=='reference'].title", doc, ["Sayings of the Century"],
     "a string comparison");
same("$..book[?@.category!='fiction'].title", doc, ["Sayings of the Century"],
     "and its negation");
eq(all("$..book[?@.price<10 && @.isbn].title", doc).length, 1, "&&");
eq(all("$..book[?@.price>20 || @.category=='reference'].title", doc).length, 2, "||");
eq(all("$..book[?(@.price>20 || @.category=='reference') && @.author].title", doc).length,
   2, "parentheses group");
same("$[?@>2]", [1, 2, 3, 4], [3, 4], "a bare @ compares the element itself");
same("$[?@=='b']", ["a", "b"], ["b"], "against a string");
same("$[?@==null]", [null, 0, false], [null], "null is only equal to null");
same("$[?@==true]", [true, 1, "true"], [true], "and true only to true");
/* A filter over an object filters its MEMBER VALUES. */
same("$[?@>1]", { a: 1, b: 2, c: 3 }, [2, 3], "a filter over an object");
/* $ inside a filter is the whole document, not the current node. */
same("$.store.book[?@.price > $.store.bicycle.price].title", doc, [],
     "a $-rooted operand resolves against the root");
eq(all("$.store.book[?@.price < $.store.bicycle.price]", doc).length, 4,
   "and selects when it should");
/* Ordering is defined for numbers and strings only. */
same("$[?@<'b']", ["a", "c", 1], ["a"], "strings order by code point");
same("$[?@<2]", [1, "a", null, true], [1], "a number does not order against a string");
same("$[?@.x==@.y]", [{ x: 1, y: 1 }, { x: 1, y: 2 }, {}],
     [{ x: 1, y: 1 }, {}], "two paths compare, and Nothing equals Nothing");

/* --------------------------------------------------------- first, paths */

eq(q("$..author").first(doc), "Nigel Rees", "first() takes the first in document order");
eq(q("$..nothing").first(doc), undefined, "first() of nothing is undefined");
{
    const p = q("$.store.book[?@.price<10].title").paths(doc);
    eq(p.length, 2, "paths() returns one path per result");
    eq(p[0], "$['store']['book'][0]['title']", "a normalized path");
    eq(p[1], "$['store']['book'][2]['title']", "and the second");
}
eq(q("$").paths(7)[0], "$", "the root path is $");
eq(q("$..*").paths({ "a'b": [1] })[0], "$['a\\'b']", "a quote in a name is escaped");
eq(q("$..*").paths({ "a\nb": 1 })[0], "$['a\\nb']", "and a newline");
eq(q("$..*").paths({ 'a"b': 1 })[0], "$['a\"b']",
   'a double quote is NOT escaped -- normalized paths are single-quoted');
{
    /* Every path must lead back to its value, which is what a path is for. */
    const vals = all("$..*", doc), paths = q("$..*").paths(doc);
    eq(vals.length, paths.length, "paths() and all() agree on the count");
    let bad = 0;
    for (let i = 0; i < paths.length; i++)
        if (q(paths[i]).first(doc) !== vals[i]) bad++;
    assert(bad === 0, "every normalized path re-selects its own node (" + bad + " bad)");
}

/* -------------------------------------------- data only: no code is run */

{
    /* THE CASE THIS MODULE EXISTS FOR. A getter is an accessor, not data, so a
     * query must skip it rather than call it. If it is ever called the throw
     * escapes and the assertion below fails. */
    let called = 0;
    const trap = {
        safe: 1,
        get boom() { called++; throw new Error("a getter ran"); }
    };
    same("$.safe", trap, [1], "a data property beside a getter still reads");
    same("$.boom", trap, [], "a getter is NOT invoked by a name selector");
    same("$.*", trap, [1], "nor by a wildcard");
    same("$..*", trap, [1], "nor by a descendant segment");
    same("$[?@]", trap, [1], "nor by a filter");
    eq(called, 0, "the getter was never called");
}
{
    /* An own non-enumerable data property: readable by name, invisible to a
     * wildcard -- the same rule Object.keys follows. */
    const o = {};
    Object.defineProperty(o, "hidden", { value: 5, enumerable: false });
    same("$.hidden", o, [5], "a non-enumerable own property reads by name");
    same("$.*", o, [], "and is invisible to a wildcard");
}
same("$.__proto__", {}, [], "__proto__ is not an own property, so it selects nothing");
same("$.toString", {}, [], "nor is an inherited method");
{
    const o = Object.create({ inherited: 1 });
    o.own = 2;
    same("$.inherited", o, [], "an inherited data property is not selected");
    same("$.own", o, [2], "an own one is");
}

/* --------------------------------------------------------------- refusals */

for (const bad of ["", "store", "$store", "$.", "$..", "$[", "$[]", "$[1", "$['a]",
                   "$[a]", "$[1:2:3:4]", "$.book[?]", "$[?@.a <]", "$[?'x']",
                   "$[?@.a === 1]", "$..[?@.a", "$[1,]", "$['a'"])
    throws(() => q(bad), "refuses " + JSON.stringify(bad));
throws(() => q(42), "the expression must be a string");
throws(() => q("$" + ".a".repeat(3000)), "and is bounded in length");
throws(() => q("$[?" + "(".repeat(64) + "@.a" + ")".repeat(64) + "]"),
       "filter nesting is bounded");
throws(() => q("$.a").all(), "a query needs a value");
throws(() => JSONPath.prototype.all.call({}, 1), "all() refuses a foreign receiver");

/* A compiled query is reusable -- that is the whole point of compiling it. */
{
    const c = q("$..price");
    eq(c.all(doc).length, 5, "first use");
    eq(c.all(doc).length, 5, "second use gives the same answer");
    eq(c.all({ price: 1 }).length, 1, "and it applies to a different document");
}

if (fails) {
    print("test_jsonpath: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_jsonpath failed");
}
print("test_jsonpath: " + n + " assertions, 0 failures");
