/* bench_xml.js -- what the XML scanner costs, and what streaming costs on top.
 *
 * The row that matters is the CONTROL: one-byte SAX writes against the same
 * document parsed in one write. Resume overhead is the thing a carry-buffer
 * design can get wrong, and the design's own budget is 2x.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/bench_xml.js
 */
import { XMLParse, XMLStringify, XMLToObject, SAXParser } from "dyna:xml";

let sink = 0;
function bench(name, reps, fn) {
    fn();
    const t0 = performance.now();
    for (let i = 0; i < reps; i++) sink += fn();
    const ms = performance.now() - t0;
    const per = ms * 1e6 / reps;
    print("  " + name.padEnd(40) + per.toFixed(0).padStart(9) + " ns/op");
    return per;
}
function mib(bytes) { return (bytes / (1 << 20)).toFixed(2) + " MiB"; }

/* A config file, a text-heavy feed, an attribute-heavy table, CDATA. */
const CONFIG = '<config><server host="localhost" port="8080"><tls enabled="true"/>'
    + "</server><logging level='info'><file>/var/log/app.log</file></logging></config>";
let feed = "<rss><channel>";
for (let i = 0; i < 2000; i++)
    feed += "<item><title>Item " + i + "</title><description>"
         + "Some reasonably long descriptive text about item " + i
         + " that makes this a text-heavy document, as real feeds are."
         + "</description><link>https://example.com/" + i + "</link></item>";
feed += "</channel></rss>";
let attrs = "<rows>";
for (let i = 0; i < 2000; i++)
    attrs += '<row id="' + i + '" a="1" b="2" c="3" d="4" e="5" f="6" g="7"/>';
attrs += "</rows>";
let cdata = "<doc>";
for (let i = 0; i < 2000; i++)
    cdata += "<c><![CDATA[raw <markup> & entities " + i + " are literal here]]></c>";
cdata += "</doc>";

print("sizes: config " + CONFIG.length + " B, feed " + mib(feed.length)
      + ", attrs " + mib(attrs.length) + ", cdata " + mib(cdata.length));

print("\nXMLParse to a tree");
bench("config, 1 element deep", 5000, () => XMLParse(CONFIG).children.length);
const pf = bench("feed, text-heavy", 20, () => XMLParse(feed).children.length);
print("        = " + (feed.length / (pf / 1e9) / (1 << 20)).toFixed(0) + " MiB/s");
const pa = bench("attrs, 7 per element", 20, () => XMLParse(attrs).children.length);
print("        = " + (attrs.length / (pa / 1e9) / (1 << 20)).toFixed(0) + " MiB/s");
bench("cdata", 20, () => XMLParse(cdata).children.length);

print("\nSAX, no tree built");
const sw = bench("feed, one write", 20, () => {
    let k = 0;
    const p = new SAXParser({ onOpen: () => k++ });
    p.write(feed); p.end();
    return k;
});
const s64 = bench("feed, 64 KiB chunks", 20, () => {
    let k = 0;
    const p = new SAXParser({ onOpen: () => k++ });
    for (let i = 0; i < feed.length; i += 65536) p.write(feed.slice(i, i + 65536));
    p.end();
    return k;
});
print("  chunked/whole ratio " + (s64 / sw).toFixed(2) + "x");

/* THE CONTROL: one byte at a time, on a small document so it finishes. */
let SMALL = "<rss><channel>";
for (let i = 0; i < 90; i++)
    SMALL += "<item><title>Item " + i + "</title><description>"
          + "Some reasonably long descriptive text about item " + i
          + "</description></item>";
SMALL += "</channel></rss>";
{
    const one = bench("20 KB, ONE BYTE per write", 5, () => {
        let k = 0;
        const p = new SAXParser({ onOpen: () => k++ });
        for (let i = 0; i < SMALL.length; i++) p.write(SMALL[i]);
        p.end();
        return k;
    });
    const all = bench("20 KB, one write                        ", 200, () => {
        let k = 0;
        const p = new SAXParser({ onOpen: () => k++ });
        p.write(SMALL); p.end();
        return k;
    });
    print("  one-byte/whole ratio " + (one / all).toFixed(1)
          + "x  (the design's budget for resume overhead is 2x)");
}

/* ONE long text run, chunked: the case a re-scanning carry buffer makes
 * quadratic. 256 resumes over 1 MiB is 128 MiB of re-scan if it restarts. */
{
    const big = "<a>" + "x every text run is a token and this one is long. ".repeat(21000)
              + "</a>";
    print("\none 1 MiB text run, 4 KiB chunks (the quadratic case)");
    bench("1 MiB text, 4 KiB chunks", 5, () => {
        let k = 0;
        const p = new SAXParser({ onText: (t) => (k += t.length) });
        for (let i = 0; i < big.length; i += 4096) p.write(big.slice(i, i + 4096));
        p.end();
        return k;
    });
    bench("1 MiB text, one write                   ", 5, () => {
        let k = 0;
        const p = new SAXParser({ onText: (t) => (k += t.length) });
        p.write(big); p.end();
        return k;
    });
}

print("\nother directions");
{
    const tree = XMLParse(feed);
    bench("XMLStringify, feed", 20, () => XMLStringify(tree).length);
    bench("XMLToObject, feed", 20, () => Object.keys(XMLToObject(tree)).length);
}
print("\nsink " + (sink === 0 ? "ZERO -- the loops were optimised away" : "ok"));
