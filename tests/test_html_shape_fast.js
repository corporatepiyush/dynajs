// HTML elements and attribute objects are built through JS_NewObjectFromProps
// (one shape cache for elements, one for attribute objects of up to 32
// attributes). The oracle is independent of the parser: documents are
// rendered from a tree spec and must parse back to that tree -- own-key
// ORDER, property attributes and prototype included.
import { HTMLParse } from "dyna:html";

let n = 0, fails = 0;
function ok(c, m, extra) { n++; if (!c) { fails++; print("FAIL: " + m + (extra !== undefined ? " -- " + extra : "")); } }

function same(a, b, path) {
    if (a === null || typeof a !== "object") return Object.is(a, b) ? null : path + ": " + JSON.stringify(a) + " vs " + JSON.stringify(b);
    if (b === null || typeof b !== "object") return path + ": object vs " + JSON.stringify(b);
    if (Array.isArray(a) !== Array.isArray(b)) return path + ": array-ness differs";
    if (Object.getPrototypeOf(a) !== Object.getPrototypeOf(b)) return path + ": prototype differs";
    const ka = Reflect.ownKeys(a), kb = Reflect.ownKeys(b);
    if (ka.length !== kb.length) return path + ": key count " + ka.length + " vs " + kb.length + " [" + kb.join(",") + "]";
    for (let i = 0; i < ka.length; i++) {
        if (ka[i] !== kb[i]) return path + ": key " + i + " is " + String(kb[i]) + ", want " + String(ka[i]);
        const da = Object.getOwnPropertyDescriptor(a, ka[i]), db = Object.getOwnPropertyDescriptor(b, kb[i]);
        if (!Array.isArray(a) && (!db.writable || !db.enumerable || !db.configurable || db.get || db.set))
            return path + "." + String(ka[i]) + ": not a plain writable/enumerable/configurable data property";
        const r = same(da.value, db.value, path + "." + String(ka[i]));
        if (r) return r;
    }
    if (!Object.isExtensible(b)) return path + ": result is not extensible";
    return null;
}

function lcg(seed) { let s = seed >>> 0; return () => (s = (Math.imul(s, 1664525) + 1013904223) >>>  0) / 4294967296; }

function mapOf(keys, mk) { const o = {}; for (const k of keys) Object.defineProperty(o, k, { value: mk(k), writable: true, enumerable: true, configurable: true }); return o; }

// ---------------------------------------------------------------- HTML
function esc(s) { return s.replace(/&/g, "&amp;").replace(/"/g, "&quot;").replace(/</g, "&lt;"); }
function render(node) {
    if (typeof node === "string") return esc(node);
    const at = Object.keys(node.attrs).map((k) => " " + k + "=\"" + esc(node.attrs[k]) + "\"").join("");
    return "<" + node.name + at + ">" + node.children.map(render).join("") + "</" + node.name + ">";
}
function el(name, attrs, children) {
    return { name, attrs: mapOf(Object.keys(attrs), (k) => attrs[k]), children };
}
function htmlRound(nodes, label) {
    const text = nodes.map(render).join("");
    let back, err = null;
    try { back = HTMLParse(text); } catch (e) { err = e; }
    const r = err ? "threw " + err.message : same(nodes, back, "$");
    ok(r === null, "html " + label, r);
}
function attrsN(count, prefix) { const o = {}; for (let i = 0; i < count; i++) o[prefix + i] = "v" + i; return o; }

for (const count of [0, 1, 2, 31, 32, 33, 34, 40, 64]) {
    htmlRound([el("div", attrsN(count, "data-a"), ["t"])], count + " attributes");
    const rows = []; for (let r = 0; r < 5; r++) rows.push(el("p", attrsN(count, "x"), ["r" + r]));
    htmlRound(rows, "5 siblings with " + count + " attributes");
}
{
    const rows = [];
    for (let r = 0; r < 30; r++) {
        const shape = r % 5;
        const a = shape === 0 ? { class: "row", id: "r" + r } : shape === 1 ? { id: "r" + r, class: "row" }
            : shape === 2 ? { class: "row" } : shape === 3 ? {} : { class: "row", id: "r" + r, title: "t" };
        rows.push(el(shape === 3 ? "span" : "div", a, [el("a", { href: "/u/" + r }, ["user" + r]), "tail"]));
    }
    htmlRound([el("section", {}, rows)], "mixed sibling attribute shapes");
}
{
    let v = el("i", { class: "leaf" }, ["x"]);
    for (let d = 0; d < 30; d++) v = el(d % 2 ? "div" : "span", { class: "c" + (d % 3) }, [v, "s" + d]);
    htmlRound([v], "nested 30 deep");
}
{
    const rnd = lcg(777);
    const tags = ["div", "span", "b", "em", "section", "i", "article"];
    const names = ["class", "id", "href", "title", "data-x", "data-y", "lang", "role", "aria-label", "name"];
    const gen = (depth) => {
        const kids = [];
        const nk = depth > 3 ? 0 : Math.floor(rnd() * 4);
        for (let i = 0; i < nk; i++) kids.push(rnd() < 0.3 && typeof kids[kids.length - 1] !== "string" ? "t" + i : gen(depth + 1));
        const a = {};
        const na = Math.floor(rnd() * 12);
        for (let i = 0; i < na; i++) a[names[Math.floor(rnd() * names.length)]] = "v" + Math.floor(rnd() * 9);
        return el(tags[Math.floor(rnd() * tags.length)], a, kids);
    };
    for (let i = 0; i < 200; i++) htmlRound([gen(0), gen(0)], "generated #" + i);
}
{
    // a repeated attribute keeps the first value (HTML5), in and past the batch
    const t = HTMLParse("<div a=\"1\" b=\"2\" a=\"3\"></div><div " + Object.keys(attrsN(40, "z")).map((k) => k + "=\"1\"").join(" ") + " z0=\"dup\"></div>");
    ok(t[0].attrs.a === "1" && Reflect.ownKeys(t[0].attrs).join() === "a,b", "duplicate attribute keeps the first");
    ok(t[1].attrs.z0 === "1" && Reflect.ownKeys(t[1].attrs).length === 40, "duplicate past 32 attributes keeps the first");
    const u = HTMLParse("<P CLASS=\"x\" Id=\"y\"></P>");
    ok(u[0].name === "p" && Reflect.ownKeys(u[0].attrs).join() === "class,id", "names are lower-cased before the shape lookup");
    u[0].attrs.extra = 1;
    const w = HTMLParse("<p class=\"x\" id=\"y\"></p>");
    ok(!("extra" in w[0].attrs), "a parsed tree shares no storage with an earlier one");
}

{
    // HTMLStringify reads name/attrs/children through a slot cache: nodes
    // built in other key orders, with extra keys, with `name` inherited or
    // behind a getter must all serialize exactly as their plain twins
    const { HTMLStringify } = await import("dyna:html");
    const variants = [
        (n, a, c) => ({ name: n, attrs: a, children: c }),
        (n, a, c) => ({ children: c, name: n, attrs: a }),
        (n, a, c) => ({ attrs: a, children: c, name: n, extra: 1 }),
        (n, a, c) => Object.assign(Object.create({ name: n }), { attrs: a, children: c }),
        (n, a, c) => { const o = { attrs: a, children: c }; Object.defineProperty(o, "name", { get() { return n; }, enumerable: true }); return o; },
    ];
    let good = true;
    for (let round = 0; round < 3; round++) {
        const kids = [];
        for (let i = 0; i < 60; i++) {
            const mk = variants[(i * 7 + round) % variants.length];
            kids.push(mk(i % 2 ? "span" : "b", i % 3 ? { class: "c" + i } : {}, ["t" + i]));
        }
        const root = variants[round % variants.length]("div", { id: "root" }, kids);
        const want = "<div id=\"root\">" + kids.map((k, i) => "<" + (i % 2 ? "span" : "b") + (i % 3 ? " class=\"c" + i + "\"" : "") + ">t" + i + "</" + (i % 2 ? "span" : "b") + ">").join("") + "</div>";
        const got = HTMLStringify(root);
        if (got !== want) { good = false; print("  round " + round + ": " + got.slice(0, 200)); }
    }
    ok(good, "HTMLStringify reads every node shape the same way");
}

print("test_html_shape_fast: " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
