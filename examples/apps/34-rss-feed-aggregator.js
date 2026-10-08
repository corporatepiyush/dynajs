// 34 · Feed aggregator — streams RSS and Atom through a SAX parser, deduplicates and merges by date.
//
// WHAT IT SHOWS
//   - dyna:xml SAXParser in pull mode: a feed is read item by item, never held as a tree
//   - handling two formats (RSS 2.0 and Atom) with one small state machine
//   - feeding the parser in arbitrary chunks, as a network would deliver them
//   - dyna:time parseRFC3339 and a hand-rolled RFC 822 date reader; dyna:structures BloomFilter for dedup
//
// RUN      dynajs examples/apps/34-rss-feed-aggregator.js
//          Feeds are inlined so it runs offline; pass fetch() bodies to readFeed in production.

import { SAXParser } from "dyna:xml";
import { parseRFC3339, formatRFC3339 } from "dyna:time";
import { BloomFilter } from "dyna:structures";

const MONTHS = { Jan: 0, Feb: 1, Mar: 2, Apr: 3, May: 4, Jun: 5, Jul: 6, Aug: 7, Sep: 8, Oct: 9, Nov: 10, Dec: 11 };

// "Tue, 03 Mar 2026 09:30:00 +0100" -> unix seconds. RSS uses this older
// format; anything unparseable yields 0 so the item sorts last instead of crashing.
function parseRfc822(text) {
    const m = /(\d{1,2}) (\w{3}) (\d{4}) (\d{2}):(\d{2})(?::(\d{2}))? ?([+-]\d{4}|GMT|UTC|Z)?/.exec(text);
    if (!m || !(m[2] in MONTHS)) return 0;
    const utc = Date.UTC(+m[3], MONTHS[m[2]], +m[1], +m[4], +m[5], +(m[6] ?? 0)) / 1000;
    const zone = m[7] && /^[+-]/.test(m[7]) ? (m[7][0] === "-" ? -1 : 1) * (+m[7].slice(1, 3) * 3600 + +m[7].slice(3) * 60) : 0;
    return utc - zone;
}
const parseDate = (text) => {
    try { return parseRFC3339(text.trim()).sec; } catch (e) { return parseRfc822(text); }
};

// Pull events until the feed ends. `chunks` is any iterable of strings/bytes.
function readFeed(source, chunks) {
    const parser = new SAXParser();                 // no handlers = pull mode
    const items = [];
    let item = null, text = "";
    const isItem = (name) => name === "item" || name === "entry";

    const drain = () => {
        for (let ev; (ev = parser.next()); ) {
            if (ev.event === "open") {
                text = "";
                if (isItem(ev.name)) item = { source, title: "", link: "", id: "", date: 0 };
                // Atom puts the URL in an attribute: <link href="..."/>
                else if (item && ev.name === "link" && ev.attrs.href && (ev.attrs.rel ?? "alternate") === "alternate") item.link = ev.attrs.href;
            } else if (ev.event === "text" || ev.event === "cdata") {
                text += ev.text;
            } else if (ev.event === "close" && item) {
                // Fields are matched by name while an item is open. No element
                // stack is kept, so the reader does not depend on how an empty
                // element such as <link/> is reported.
                const value = text.trim();
                if (ev.name === "title") item.title = value;
                else if (ev.name === "link" && value) item.link = value;
                else if (ev.name === "guid" || ev.name === "id") item.id = value;
                else if (ev.name === "pubDate" || ev.name === "updated" || ev.name === "published") item.date = parseDate(value);
                else if (isItem(ev.name)) { item.id ||= item.link; items.push(item); item = null; }
                text = "";
            }
        }
    };
    for (const chunk of chunks) { parser.write(chunk); drain(); }
    parser.end();
    drain();
    return items;
}

function aggregate(feeds, limit = 10) {
    // A Bloom filter answers "seen before?" in a few bits per item. A false
    // positive would drop one genuinely new item, so it is sized generously.
    const seen = new BloomFilter(1 << 16, 4);
    const merged = [];
    for (const items of feeds)
        for (const it of items) {
            const key = it.link || it.id;
            if (!key || seen.mayContain(key)) continue;
            seen.add(key);
            merged.push(it);
        }
    return merged.sort((a, b) => b.date - a.date).slice(0, limit);
}

// ---- sample feeds ----------------------------------------------------------
const rss = `<?xml version="1.0"?><rss version="2.0"><channel><title>Engineering Blog</title>
<item><title>Shipping the new storage engine</title><link>https://blog.example/storage</link>
  <guid>https://blog.example/storage</guid><pubDate>Tue, 03 Mar 2026 09:30:00 +0100</pubDate></item>
<item><title><![CDATA[Benchmarks & <caveats>]]></title><link>https://blog.example/bench</link>
  <pubDate>Mon, 02 Mar 2026 18:00:00 GMT</pubDate></item>
</channel></rss>`;
const atom = `<?xml version="1.0" encoding="utf-8"?><feed xmlns="http://www.w3.org/2005/Atom"><title>Release Notes</title>
<entry><title>Version 4.2 released</title><link href="https://notes.example/4.2"/><id>urn:rel:4.2</id><updated>2026-03-04T12:00:00Z</updated></entry>
<entry><title>Storage engine, cross-posted</title><link href="https://blog.example/storage"/><id>urn:rel:x</id><updated>2026-03-03T10:00:00Z</updated></entry>
</feed>`;

// Deliver the RSS feed 17 bytes at a time: tags and even the CDATA marker get
// split across chunks, and the parser must carry on regardless.
const drip = (text, n) => Array.from({ length: Math.ceil(text.length / n) }, (_, i) => text.slice(i * n, i * n + n));
const feeds = [readFeed("blog", drip(rss, 17)), readFeed("notes", [atom])];
const latest = aggregate(feeds);
for (const it of latest) console.log(`${formatRFC3339(it.date)}  [${it.source}] ${it.title}`);

// ---- self-test -------------------------------------------------------------
const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
check(feeds[0].length === 2 && feeds[1].length === 2, "both formats yield their items");
check(feeds[0][1].title === "Benchmarks & <caveats>", "CDATA titles survive chunked delivery");
check(feeds[0][0].date === Date.UTC(2026, 2, 3, 8, 30) / 1000, "RFC 822 dates honour the UTC offset");
check(latest.length === 3, "the cross-posted article appears once: " + latest.length);
check(latest.map((i) => i.title)[0] === "Version 4.2 released", "the newest item comes first");
check(latest.every((it, i) => i === 0 || latest[i - 1].date >= it.date), "items are in date order");
let malformed = "";
try { readFeed("bad", ["<rss><channel><item><title>oops</item></channel></rss>"]); } catch (e) { malformed = e.message; }
check(malformed !== "", "a malformed feed is an error, not a silently short list");
console.log("self-test passed");
