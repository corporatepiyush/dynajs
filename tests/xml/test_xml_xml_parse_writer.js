// covers: dyna:xml.XMLParse
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_xml_xml_parse_writer");
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

__kitS.section("5 < 6 & 7 > 6 \"quoted\"", () => {
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
});
__kitS.end();

print("test_xml_writer: " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
