/* Robots (design 28). The cases below are the ones robots parsers get wrong,
   not the happy path: Allow longer than Disallow, $ anchoring, an empty
   Disallow meaning allow-all, and %2F which RFC 9309 says stays encoded. */
import { Robots } from "dyna:scrape";
let n = 0, bad = 0;
const ok = (c, w) => { n++; if (!c) { bad++; print("FAIL: " + w); } };

const r1 = new Robots("User-agent: *\nDisallow: /private\nAllow: /private/ok\n");
ok(r1.allows("/public") === true, "unlisted path allowed");
ok(r1.allows("/private") === false, "disallowed prefix");
ok(r1.allows("/private/x") === false, "under a disallowed prefix");
ok(r1.allows("/private/ok") === true, "LONGER Allow beats Disallow");

/* an empty Disallow means allow everything -- the classic inversion */
ok(new Robots("User-agent: *\nDisallow:\n").allows("/anything") === true,
   "empty Disallow is allow-all");
ok(new Robots("User-agent: *\nDisallow: /\n").allows("/anything") === false,
   "Disallow: / blocks everything");

/* wildcards and the end anchor */
const r2 = new Robots("User-agent: *\nDisallow: /*.pdf$\n");
ok(r2.allows("/a/b.pdf") === false, "*.pdf$ matches");
ok(r2.allows("/a/b.pdf.html") === true, "$ anchors to the end");
ok(new Robots("User-agent: *\nDisallow: /a/*/c\n").allows("/a/b/c") === false,
   "mid-pattern wildcard");

/* a specific group beats the wildcard group */
const r3 = new Robots(
  "User-agent: *\nDisallow: /\n\nUser-agent: dynabot\nDisallow: /nope\n",
  { agent: "dynabot" });
ok(r3.allows("/yes") === true, "the named group replaces *");
ok(r3.allows("/nope") === false, "the named group's own rule applies");
const r3b = new Robots(
  "User-agent: *\nDisallow: /\n\nUser-agent: dynabot\nDisallow: /nope\n",
  { agent: "other" });
ok(r3b.allows("/yes") === false, "an unmatched agent falls back to *");

/* case-insensitive directives, comments, CRLF, and Sitemap collection */
const r4 = new Robots(
  "# a comment\r\nUSER-AGENT: *\r\nDISALLOW: /x   # trailing\r\n" +
  "Crawl-delay: 2.5\r\nSitemap: https://h.test/s1.xml\r\n");
ok(r4.allows("/x") === false, "case-insensitive keys, CRLF, trailing comment");
ok(r4.crawlDelay() === 2.5, "Crawl-delay parsed: " + r4.crawlDelay());
ok(r4.sitemaps().length === 1, "sitemap collected");
ok(new Robots("User-agent: *\n").crawlDelay() === null, "absent Crawl-delay is null");

/* Crawl-delay is parsed STRICTLY: the value is a number, and a malformed line
   is IGNORED (the directive falls back to the group default of null) -- atof
   used to truncate "2x" to 2, silently honouring a value the site never sent.
   A later well-formed line must still apply. */
ok(new Robots("User-agent: *\nCrawl-delay: 2x\n").crawlDelay() === null,
   "malformed Crawl-delay '2x' is ignored, not truncated at 2");
ok(new Robots("User-agent: *\nCrawl-delay: abc\n").crawlDelay() === null,
   "non-numeric Crawl-delay is ignored");
ok(new Robots("User-agent: *\nCrawl-delay:\n").crawlDelay() === null,
   "empty Crawl-delay is ignored");
ok(new Robots("User-agent: *\nCrawl-delay: 2x\nCrawl-delay: 3\n").crawlDelay() === 3,
   "only the malformed line is dropped; a later value applies");
ok(new Robots("User-agent: *\nCrawl-delay: 2.5\n").crawlDelay() === 2.5,
   "well-formed fractional value parses");

/* RFC 9309 compares octets: %2F must NOT decode to '/' */
const r5 = new Robots("User-agent: *\nDisallow: /a%2Fb\n");
ok(r5.allows("/a/b") === true, "%2F is not '/'");

/* bounds: a rules bomb must be capped, not grow without limit */
let bomb = "User-agent: *\n";
for (let i = 0; i < 5000; i++) bomb += "Disallow: /p" + i + "\n";
ok(new Robots(bomb).ruleCount <= 1000, "rule cap enforced: " + new Robots(bomb).ruleCount);

print("test_scrape_robots: " + n + " assertions, " + bad + " failures");
if (bad) throw new Error(bad + " failures");
