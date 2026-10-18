import { XMLParse, XMLStringify } from "dyna:xml";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function throwsSyntax(fn, msg) {
    let caught = null;
    try { fn(); } catch (e) { caught = e; }
    assert(caught !== null, msg + " (expected throw)");
    assert(/XMLParse/.test(String(caught.message)),
        msg + " (XMLParse SyntaxError, got " + caught + ")");
}

{
    const roots = XMLParse('<a x="1"/><b/><c/>', { multiple: true });
    assert(Array.isArray(roots), "multiple returns an array");
    assert(roots.length === 3, "every root element is returned");
    assert(roots.map((e) => e.name).join(",") === "a,b,c",
        "roots in document order");

    assert(roots[0].attrs.x === "1", "attributes survive on the roots");
    assert(XMLStringify(roots[0]) === '<a x="1"/>', "roots round-trip");
    const withKids = XMLParse("<wrap><t>x</t></wrap><u/>", { multiple: true });
    assert(withKids.length === 2 &&
        withKids[0].children[0].children[0] === "x",
        "subtree survives on a non-first root");

    const one = XMLParse("<a/>", { multiple: true });
    assert(Array.isArray(one) && one.length === 1 && one[0].name === "a",
        "single-root document -> one-element array");

    const mixed = XMLParse('<a/> <!-- c --> <?pi x?> <b/>', { multiple: true });
    assert(mixed.length === 2 && mixed[0].name === "a" && mixed[1].name === "b",
        "comments/PIs between roots are not roots");

    const spaced = XMLParse("   <a/>\n<b/>   ", { multiple: true });
    assert(spaced.length === 2, "surrounding whitespace ignored");

    const cd = XMLParse("<a><![CDATA[x<y]]></a><b/>", { multiple: true });
    assert(cd.length === 2 && cd[0].children[0] === "x<y",
        "CDATA handled inside a multi-root document");
}

{
    const single = XMLParse("<a/>");
    assert(!Array.isArray(single) && single.name === "a",
        "default: the root element itself, not an array");
    assert(XMLParse("<a/>", { multiple: false }).name === "a",
        "explicit multiple: false behaves like the default");

    throwsSyntax(() => XMLParse("<a/><b/>"),
        "multi-root without the option is a SyntaxError");
    throwsSyntax(() => XMLParse("<a/><b/>", {}),
        "options object without multiple keeps the refusal");

    throwsSyntax(() => XMLParse("<a>", { multiple: true }),
        "unclosed element still throws in multiple mode");
    throwsSyntax(() => XMLParse("</a>", { multiple: true }),
        "stray close tag still throws in multiple mode");
    throwsSyntax(() => XMLParse("", { multiple: true }),
        "no element at all still throws in multiple mode");
}

print("test_xml_upgrade: all tests passed (" + n + " assertions)");
