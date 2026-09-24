/* test_xml_writer.js --: the streaming XmlWriter.
 *
 * The writer exists for documents the 256-deep string builder refuses and
 * for incremental output with no tree in memory: open()/text()/close() push
 * bytes as they come, toString() reads them so far WITHOUT closing
 * anything, finish() closes the remaining open elements and returns the
 * document. Every error is pinned: bad names, bad argument types, strict
 * options, close() with nothing open, and the depth behavior that motivates
 * the class (deep works far past the serializer's 256; the runaway guard
 * throws cleanly). Pure dyna:xml.
 */
import { XmlWriter, XMLParse, XMLStringify } from "dyna:xml";

let n = 0, fails = 0;
const check = (c, m) => { n++; if (!c) { fails++; print("FAIL: " + m); } };
const eq = (a, b, m) => check(JSON.stringify(a) === JSON.stringify(b),
    m + " -- got " + JSON.stringify(a) + ", want " + JSON.stringify(b));
const throws = (fn, m) => {
  let t = false, msg = "";
  try { fn(); } catch (e) { t = true; msg = String(e.message || e); }
  check(t, m + (t ? "" : " -- did NOT throw"));
  return msg;
};

/* ---- argument contracts --------------------------------------------- */
{
  let m = throws(() => new XmlWriter({ indnt: 2 }), "strict options bag");
  check(/indnt/.test(m) && /indent/.test(m), "error names the key and the valid set");
  throws(() => new XmlWriter(42), "non-object options refused");
  const ok1 = new XmlWriter();
  ok1.open("a"); ok1.close();
  eq(ok1.finish(), "<a></a>", "no-arg ctor works");
  const ok2 = new XmlWriter(null);
  ok2.open("a"); ok2.close();
  eq(ok2.finish(), "<a></a>", "null options counts as absent");
  const ok3 = new XmlWriter({ indent: 0 });
  ok3.open("a"); ok3.close();
  eq(ok3.finish(), "<a></a>", "indent 0 (minified) works");
  throws(() => new XmlWriter({ indent: -1 }), "indent -1 refused");
  throws(() => new XmlWriter({ indent: 17 }), "indent 17 refused");

  const w = new XmlWriter();
  throws(() => w.open(), "open() without a name refused");
  throws(() => w.open(42), "open(42) refused");
  throws(() => w.open(""), "empty name refused");
  throws(() => w.open("1bad"), "name starting with a digit refused");
  throws(() => w.open("a b"), "name with a space refused");
  throws(() => w.open("a<b"), "name with '<' refused");
  throws(() => w.open("ok", 42), "non-object attrs refused");
  throws(() => w.open("ok", { "1x": "y" }), "invalid attribute name refused");
  throws(() => w.close(), "close() with nothing open refused");
  throws(() => w.text(42), "text(42) refused");
  throws(() => w.text(), "text() without a string refused");
}

/* ---- bytes as they come --------------------------------------------- */
{
  const w = new XmlWriter();
  w.open("root");
  w.open("item", { id: "7" });
  w.text("hello");
  eq(w.toString(), '<root><item id="7">hello',
     "toString returns the bytes SO FAR and closes nothing");
  w.close();
  eq(w.toString(), '<root><item id="7">hello</item>',
     "close() writes the matching end tag for the innermost open element");
  w.text("!");
  eq(w.finish(), '<root><item id="7">hello</item>!</root>',
     "finish() closes every remaining element and returns the document");
  eq(w.finish(), '<root><item id="7">hello</item>!</root>',
     "finish() is idempotent: one writer, one document, the buffer never truncates");
}

/* ---- escaping ------------------------------------------------------- */
{
  const w = new XmlWriter();
  w.open("r", { a: 'x"y&z<w>', b: "plain" });
  w.text('5 < 6 & 7 > 6 "quoted"');
  w.close();
  const out = w.finish();
  eq(out,
     '<r a="x&quot;y&amp;z&lt;w&gt;" b="plain">5 &lt; 6 &amp; 7 &gt; 6 "quoted"</r>',
     "attr values escape quotes/amp/lt/gt; text escapes amp/lt/gt and keeps quotes");
  const rt = XMLParse(out);
  eq(rt.attrs.a, 'x"y&z<w>', "attr round-trips through XMLParse");
  eq(rt.children[0], '5 < 6 & 7 > 6 "quoted"', "text round-trips");
}

/* ---- indentation ---------------------------------------------------- */
{
  const w = new XmlWriter({ indent: 2 });
  w.open("a");
  w.open("b");
  w.text("t");
  w.close();
  w.close();
  eq(w.finish(), '<a>\n  <b>t\n  </b>\n</a>', "indent 2 pretty-prints");
}

/* ---- depth: the reason this class exists ----------------------------- */
{
  /* The serializer (and every tree round-trip) refuses past 256. The
     writer must not: 300 deep is the DOCUMENT UNDER TEST. */
  const DEPTH = 300;
  const w = new XmlWriter();
  for (let i = 0; i < DEPTH; i++) w.open("d" + (i % 10));
  for (let i = 0; i < DEPTH; i++) w.close();
  const out = w.finish();
  eq((out.match(/<d\d>/g) || []).length, DEPTH, "300 opens all present");
  eq((out.match(/<\/d\d>/g) || []).length, DEPTH, "300 closes all present");
  eq(out.slice(0, 5), "<d0><", "document starts at the outermost element");

  /* the runaway guard is a clean RangeError, never a crash */
  const w2 = new XmlWriter();
  let deep = 0, err = null;
  try {
    for (let i = 0; i < 70000; i++) { w2.open("x"); deep = i + 1; }
  } catch (e) { err = e; }
  check(err instanceof RangeError, "past the cap open() throws a RangeError");
  check(/nesting/.test(String(err && err.message)), "and names the cause");
  check(deep > 300, "but the cap is far past the builder's 256 (at " + deep + ")");
  /* finish() still closes the bounded stack cleanly */
  check(typeof w2.finish() === "string", "finish() works from just under the cap");
}

/* ---- coerce-then-resolve: coercion runs before the handle is touched -- */
{
  /* dyna-nat.h contract: open() must coerce ALL arguments (running the
     attribute getters, which are arbitrary user JS that may reenter or
     dispose the writer) BEFORE the native handle is resolved, with no
     JS-invoking call after. Observable consequence: a getter runs even when
     the writer is already at the depth cap -- the cap is a handle-state
     check, so it fires only after coercion completed (and the getter's
     reentrant opens count toward it). The old shape resolved the handle
     first and refused before running the getter, leaving a window where
     user JS could invalidate the already-resolved handle. */
  const CAP = 65536;
  const w = new XmlWriter();
  for (let i = 0; i < CAP; i++) w.open("d" + (i % 10));
  let ran = 0, m = "";
  try { w.open("over", { get x() { ran++; return "1"; } }); }
  catch (e) { m = String(e.message || e); }
  check(ran === 1, "the attribute getter ran (coercion precedes resolution)");
  check(/nesting exceeds/.test(m), "the cap still refuses after coercion");
  /* and a reentrant write from a getter cannot corrupt the start tag: the
     whole tag is committed atomically, after the getter ran */
  const w3 = new XmlWriter();
  w3.open("root", { get side() { w3.text("MID"); return "1"; } });
  w3.close();
  eq(w3.finish(), 'MID<root side="1"></root>',
     "a getter's reentrant write lands before the start tag, tag intact");
}

/* ---- interop with the tree form -------------------------------------- */
{
  const w = new XmlWriter();
  w.open("book", { id: "1" });
  w.open("title"); w.text("T"); w.close();
  w.open("tag"); w.text("a"); w.close();
  w.close();
  const tree = XMLParse(w.finish());
  const ref = XMLParse('<book id="1"><title>T</title><tag>a</tag></book>');
  eq(tree, ref, "writer output parses to the same tree as the literal");
  const w2 = new XmlWriter();
  w2.open("r", { x: "1" }); w2.text("s"); w2.close();
  eq(w2.finish(), XMLStringify({ name: "r", attrs: { x: "1" }, children: ["s"] }),
     "writer output is byte-compatible with the serializer (minified)");
}

/* ---- atomic open(): a throw leaves the writer exactly as it was ------ */
{
  const w = new XmlWriter();
  throws(() => w.open("a", { ok: "1", "bad name!": "2" }),
         "an invalid attribute name mid-attrs throws");
  w.open("z"); w.close();
  eq(w.finish(), "<z></z>",
     "the failed open wrote NOTHING and pushed nothing (output stays well-formed)");

  const w2 = new XmlWriter();
  const bad = { ok: "1" };
  Object.defineProperty(bad, "boom", { enumerable: true,
    get() { throw new Error("getter"); } });
  throws(() => w2.open("a", bad), "a throwing attribute getter propagates");
  w2.open("z"); w2.close();
  eq(w2.finish(), "<z></z>",
     "and a getter that throws mid-attrs also leaves the writer consistent");
}

/* ---- C0 controls are refused (not legal XML 1.0 characters) ---------- */
{
  const w = new XmlWriter();
  w.open("t");
  let m = throws(() => w.text("z\u0002w"), "text with a C0 control refused");
  check(/XML 1.0/.test(m), "the error names the XML 1.0 character rule");
  throws(() => w.open("u", { a: "x\u0001y" }),
         "an attribute value with a C0 control refused");
  w.open("v"); w.close();
  eq(w.finish(), "<t><v></v></t>",
     "the refused writes left nothing behind (writer consistent)");
  const w3 = new XmlWriter();
  w3.open("t");
  w3.text("line1\nline2");
  w3.close();
  check(w3.finish().indexOf("line1\nline2") >= 0,
        "tab/LF/CR stay legal and raw in text");
}

/* ---- strict option types (no coercion, no truncation) ---------------- */
{
  let m = throws(() => new XmlWriter({ indent: "3" }), "indent \"3\" (string) refused");
  check(/integer/.test(m), "the error names the integer requirement");
  m = throws(() => new XmlWriter({ indent: 3.5 }), "indent 3.5 (fraction) refused");
  check(/integer/.test(m), "the fraction error names the integer requirement");
  throws(() => new XmlWriter({ indent: -0.5 }), "indent -0.5 (fraction) refused");
  throws(() => new XmlWriter({ indent: NaN }), "indent NaN refused");
  m = throws(() => new XmlWriter({ indent: Infinity }), "indent Infinity refused");
  check(/integer/.test(m), "the Infinity error names the integer requirement");
  throws(() => new XmlWriter({ indent: -Infinity }), "indent -Infinity refused");
  const w = new XmlWriter({ indent: 3 });
  w.open("a"); w.open("b"); w.text("t"); w.close(); w.close();
  eq(w.finish(), '<a>\n   <b>t\n   </b>\n</a>', "indent 3 (the integer) accepted");
}

print("test_xml_writer: " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
