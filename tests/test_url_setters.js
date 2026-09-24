// flags: --std
/* test_url_setters.js --: the WHATWG URL setters.
 *
 * The expected values are the WHATWG URL Standard's own setter semantics
 * (sec. 4.4 "URL parsing" + the [PutForwards] setter table, and the state
 * override that sends a `?` in a pathname to the query state), cross-checked
 * against a mainstream browser. The vectors are values a browser produces,
 * not values this engine produced -- a round trip through this module would
 * only prove it agrees with itself.
 *
 * Run: dynajs --std tests/test_url_setters.js */

import { URL } from "dyna:url";

let n = 0, fails = 0;
function eq(a, b, msg) {
    n++;
    if (a !== b) { fails++; print("FAIL: " + msg + " (got " + JSON.stringify(a) +
                                  ", want " + JSON.stringify(b) + ")"); }
}
function mk(s) { return new URL(s); }

/* pathname */
eq((() => { const u = mk("https://a.test/x?y=1#z"); u.pathname = "/p"; return u.href; })(),
   "https://a.test/p?y=1#z", "pathname replaces the path, query and fragment stay");
eq((() => { const u = mk("https://a.test/x"); u.pathname = "p"; return u.href; })(),
   "https://a.test/p", "a relative pathname is rooted");
eq((() => { const u = mk("https://a.test/x/y"); u.pathname = ""; return u.href; })(),
   "https://a.test/", "an empty pathname is the root");
eq((() => { const u = mk("https://a.test/x"); u.pathname = "/a b"; return u.href; })(),
   "https://a.test/a%20b", "a space is percent-encoded in the path");
eq((() => { const u = mk("https://a.test/x?old#f"); u.pathname = "/p?new"; return u.href; })(),
   "https://a.test/p?new#f", "a '?' in a pathname starts the query (state override)");
eq((() => { const u = mk("https://a.test/x?old"); u.pathname = "/p#n"; return u.href; })(),
   "https://a.test/p?old#n", "a '#' in a pathname starts the fragment (query stays)");
eq((() => { const u = mk("https://a.test/a/b/../c"); u.pathname = "/d/./e"; return u.pathname; })(),
   "/d/e", "dot segments resolve in a pathname");

/* search */
eq((() => { const u = mk("https://a.test/x?y=1#z"); u.search = "?q=2"; return u.href; })(),
   "https://a.test/x?q=2#z", "search replaces the query, fragment stays");
eq((() => { const u = mk("https://a.test/x?y=1"); u.search = "q=2"; return u.href; })(),
   "https://a.test/x?q=2", "one leading '?' is a delimiter");
eq((() => { const u = mk("https://a.test/x?y=1"); u.search = ""; return u.href; })(),
   "https://a.test/x", "the empty string clears the query");
eq((() => { const u = mk("https://a.test/x"); u.search = "a=1&b=2"; return u.href; })(),
   "https://a.test/x?a=1&b=2", "a multi-pair query survives");
eq((() => { const u = mk("https://a.test/x?y=1"); u.search = "a=1#f"; return u.href; })(),
   "https://a.test/x?a=1#f", "a '#' in a search starts the fragment");

/* hash */
eq((() => { const u = mk("https://a.test/x"); u.hash = "#h"; return u.href; })(),
   "https://a.test/x#h", "hash sets the fragment");
eq((() => { const u = mk("https://a.test/x"); u.hash = "h"; return u.href; })(),
   "https://a.test/x#h", "one leading '#' is a delimiter");
eq((() => { const u = mk("https://a.test/x#h"); u.hash = ""; return u.href; })(),
   "https://a.test/x", "the empty string clears the fragment");
eq((() => { const u = mk("https://a.test/x"); u.hash = "#a#b"; return u.hash; })(),
   "#a#b", "a later '#' is fragment content");

/* host / hostname / port */
eq((() => { const u = mk("https://a.test:8443/x"); u.hostname = "b.test"; return u.href; })(),
   "https://b.test:8443/x", "hostname keeps the port");
eq((() => { const u = mk("https://a.test/x"); u.hostname = "B.TEST"; return u.href; })(),
   "https://b.test/x", "hostname is lowercased");
eq((() => { const u = mk("https://a.test/x"); u.hostname = "b/te"; return u.href; })(),
   "https://a.test/x", "a forbidden host code point makes hostname a no-op");
eq((() => { const u = mk("https://a.test/x"); u.hostname = ""; return u.href; })(),
   "https://a.test/x", "an empty hostname is a no-op");
eq((() => { const u = mk("https://a.test/x"); u.host = "b.test:99"; return u.href; })(),
   "https://b.test:99/x", "host carries the port");
eq((() => { const u = mk("https://a.test/x"); u.host = "b.test:443"; return u.href; })(),
   "https://b.test/x", "the default port is elided");
eq((() => { const u = mk("https://a.test/x"); u.port = "8080"; return u.href; })(),
   "https://a.test:8080/x", "port sets the port");
eq((() => { const u = mk("https://a.test:8443/x"); u.port = ""; return u.href; })(),
   "https://a.test/x", "an empty port clears it");
eq((() => { const u = mk("https://a.test:8443/x"); u.port = "abc"; return u.href; })(),
   "https://a.test:8443/x", "a non-numeric port is a no-op");
eq((() => { const u = mk("mailto:x@y.test"); u.hostname = "z.test"; return u.href; })(),
   "mailto:x@y.test", "a host-less URL has no host to set");

/* searchParams replacement */
eq((() => { const u = mk("https://a.test/x?y=1"); u.searchParams = "a=1&b=2"; return u.href; })(),
   "https://a.test/x?a=1&b=2", "searchParams replaces the query");
eq((() => { const u = mk("https://a.test/x"); u.searchParams = { k: "v v" }; return u.href; })(),
   "https://a.test/x?k=v+v", "a record initializer is encoded");
eq((() => { const u = mk("https://a.test/x"); u.searchParams = [["a", "1"]]; return u.href; })(),
   "https://a.test/x?a=1", "a pair array initializer works");
eq((() => { const u = mk("https://a.test/x?y=1"); u.searchParams = ""; return u.href; })(),
   "https://a.test/x", "an empty searchParams clears the query");

/* the bound searchParams object sees a query the setters replaced */
eq((() => { const u = mk("https://a.test/x?y=1"); const sp = u.searchParams;
            u.search = "a=2"; return sp.get("a"); })(),
   "2", "a bound searchParams revalidates after the search setter");
eq((() => { const u = mk("https://a.test/x?y=1"); const sp = u.searchParams;
            u.search = "a=2"; return String(sp.get("y")); })(),
   "null", "and the replaced pair is gone");
eq((() => { const u = mk("https://a.test/x?y=1"); u.searchParams.append("z", "9");
            return u.href; })(),
   "https://a.test/x?y=1&z=9", "a bound mutation still writes through");

print("test_url_setters: " + (n - fails) + "/" + n + " assertions, " + fails + " failures");
if (fails > 0) throw new Error("test_url_setters failed");
