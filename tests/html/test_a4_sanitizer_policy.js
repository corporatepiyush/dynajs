import { Sanitizer } from "dyna:html";

let n = 0, fails = 0;
function assert(c, msg) {
    n++;
    if (!c) {
        fails++;
        print("FAIL: " + msg);
    }
}

const san = new Sanitizer({
    allow: { a: ["href"] },
    protocols: { "a.href": ["HTTPS", "HTTP"] },
});
const out = (h) => san.clean(h);

assert(out('<a href="https://ok.test/">x</a>').includes("https://ok.test/"),
    "protocol entries match case-insensitively (stored uppercase)");
assert(out('<a href="http://ok.test/">x</a>').includes("http://ok.test/"),
    "second protocol entry matches");
assert(!out('<a href="ftp://ok.test/">x</a>').includes("ftp://ok.test/"),
    "a scheme outside the rule is dropped");
assert(!out('<a href="javascript:alert(1)">x</a>').includes("javascript"),
    "javascript: is still dropped");
assert(!out("<a href='java\tscript:alert(1)'>x</a>").includes("script:"),
    "embedded tab scheme is still dropped");
assert(out('<a href="/rel">x</a>').includes('href="/rel"'),
    "relative URL without a scheme still passes");

const san2 = new Sanitizer({ allow: { a: ["href"] } });
assert(san2.clean('<a href="https://a.test/">x</a>').includes("https://a.test/"),
    "default safe scheme list allows https");
assert(!san2.clean('<a href="javascript:alert(1)">x</a>').includes("javascript"),
    "default safe scheme list drops javascript");

print("test_a4_sanitizer_policy: " + (n - fails) + " passed, " + fails + " failed");
if (fails)
    throw new Error("test_a4_sanitizer_policy: " + fails + " failures");
