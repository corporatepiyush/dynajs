import { XMLParse } from "dyna:xml";
let n=0,bad=0; const ok=(c,w)=>{n++;if(!c){bad++;print("FAIL: "+w);}};
const T = (x) => JSON.stringify(XMLParse(x));
/* text spanning the 64-byte probe both ways, entities, and the ]]> guard */
for (const len of [1, 10, 63, 64, 65, 200, 5000]) {
  const body = "a".repeat(len);
  const d = XMLParse("<r>" + body + "</r>");
  ok(JSON.stringify(d).indexOf(body) > 0, "clean text len " + len);
}
ok(T("<r>a&amp;b</r>").indexOf("a&b") > 0, "entity mid-text");
ok(T("<r>" + "x".repeat(100) + "&amp;" + "y".repeat(100) + "</r>").indexOf("&") > 0, "entity after a long run");
ok(T("<r>a]b</r>").indexOf("a]b") > 0, "a lone ] is ordinary text");
let threw = 0; try { XMLParse("<r>a]]>b</r>"); } catch(e) { threw = 1; }
ok(threw === 1, "]]> in text is still refused");
ok(T("<r><![CDATA[" + "z".repeat(300) + "]]></r>").indexOf("z") > 0, "long CDATA");
ok(T("<r><!--" + "c".repeat(300) + "--></r>") !== "", "long comment");
print("xml text: " + n + " assertions, " + bad + " failures");
if (bad) throw new Error("fail");
