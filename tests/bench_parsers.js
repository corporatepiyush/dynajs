/* bench_parsers.js -- the throughput of the parsers and codecs, in MiB/s.
 *
 * This file exists because eleven of fourteen modules shipped with NO
 * benchmark at all, which meant nobody could say what any of them cost. The
 * hash rows deliberately print a TUNED implementation beside the new one: an
 * absolute number says nothing, and 571 against 2079 MiB/s says where the work
 * is. The decimal rows are the slowest per byte in the tree and are here to
 * stay visible.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/bench_parsers.js
 */
import { HTMLParse, MarkdownToHTML, Sanitizer, Template } from "dyna:html";
import { Parse as YParse } from "dyna:yaml";
import { MsgPackEncode, MsgPackDecode } from "dyna:serialize";
import { SHA3_256, SHA256, XXHash64 } from "dyna:hash";
import { Decimal } from "dyna:decimal";

function mb(name, bytes, reps, fn) {
    fn();
    const t0 = performance.now();
    for (let i = 0; i < reps; i++) fn();
    const ms = performance.now() - t0;
    const mbs = (bytes * reps) / (ms / 1000) / (1 << 20);
    print("  " + name.padEnd(34) + (ms * 1e6 / reps).toFixed(0).padStart(9) + " ns   "
          + mbs.toFixed(0).padStart(6) + " MiB/s");
    return mbs;
}
/* Realistic markup: long text runs between tags, as a real page has. */
let html = "<html><body>";
for (let i = 0; i < 400; i++)
    html += '<div class="row"><p>' + "Some ordinary sentence of prose that a real page contains, repeated. ".repeat(3)
          + '</p><a href="/x/' + i + '">link</a></div>';
html += "</body></html>";
let md = "";
for (let i = 0; i < 300; i++)
    md += "## Heading " + i + "\n\nA paragraph of *ordinary* prose with a [link](https://e.com) in it, "
        + "long enough that the scanner has real runs to cross.\n\n";
let yaml = "";
for (let i = 0; i < 800; i++) yaml += "key" + i + ": value number " + i + "\n";
const obj = {};
for (let i = 0; i < 300; i++) obj["field" + i] = "a string value of ordinary length " + i;
const mp = MsgPackEncode(obj);
const big = new Uint8Array(1 << 20);
for (let i = 0; i < big.length; i++) big[i] = i & 0xff;

print("markup parsers (bytes in, tree/HTML out)");
mb("HTMLParse", html.length, 30, () => HTMLParse(html).length);
mb("MarkdownToHTML", md.length, 30, () => MarkdownToHTML(md).length);
mb("YAML Parse", yaml.length, 30, () => Object.keys(YParse(yaml)).length);
{
    const san = new Sanitizer({ allow: { div: ["class"], p: [], a: ["href"] } });
    mb("Sanitizer.clean", html.length, 30, () => san.clean(html).length);
    const t = new Template("{{#rows}}<li>{{name}}</li>{{/rows}}");
    const rows = []; for (let i = 0; i < 2000; i++) rows.push({ name: "item " + i });
    mb("Template.render (2k rows)", 2000 * 20, 50, () => t.render({ rows }).length);
}
print("\nbinary codecs");
mb("MsgPackEncode (300 fields)", mp.length, 500, () => MsgPackEncode(obj).length);
mb("MsgPackDecode", mp.length, 500, () => Object.keys(MsgPackDecode(mp)).length);

print("\nhashes over 1 MiB -- SHA256 and XXHash64 here are TUNED implementations");
const a = mb("SHA3_256 (mine)", big.length, 20, () => SHA3_256(big).length);
const b = mb("SHA256 (existing)", big.length, 20, () => SHA256(big).length);
const c = mb("XXHash64 (existing)", big.length, 50, () => XXHash64(big));
print("  SHA3 is " + (b / a).toFixed(1) + "x slower than SHA256, "
      + (c / a).toFixed(0) + "x slower than XXHash64");

print("\ndecimal: the digit-array representation the design did NOT ask for");
{
    const x = new Decimal("123456789012345678901234"), y = new Decimal("987654321098765432109876");
    mb("Decimal mul, 24 digits", 24, 20000, () => x.mul(y).digits());
    mb("Decimal div, 34 digits", 34, 5000, () => x.div(y).digits());
}
