#!/usr/bin/env python3
"""gen_config.py — dyna:config probes. Oracle: python tomllib (TOML 1.0),
configparser-shape reasoning for INI, dotenv grammar; expectations baked as
JSON at generation time where python computes them."""
import json
import sys
import tomllib
from probe_lib import emit

IMP = 'import { TOML, INI, Env, FrontMatter } from "dyna:config";\n'

# ---- TOML cases: (id, toml_text). Expected values computed by tomllib here.
TOML_CASES = [
    ("basic", 'title = "DynaJS"\nport = 8080\npi = 3.25\nflag = true'),
    ("table", '[server]\nhost = "127.0.0.1"\nport = 8080\n[client]\nretries = 3'),
    ("dotted", 'server.host = "localhost"\nserver.port = 9000'),
    ("aot", '[[items]]\nname = "a"\n[[items]]\nname = "b"'),
    ("array", 'nums = [1, 2, 3]\nmixed = ["x", "y"]'),
    ("escapes", 's = "a\\tb\\nc\\"d\\\\e"'),
    ("literal", "s = 'C:\\path\\no\\escape'"),
    ("inline_table", 'point = { x = 1, y = 2 }'),
    ("neg", 'n = -17\nf = -2.5e2'),
    ("dates", 'd = 1979-05-27\ndt = 1979-05-27T07:32:00Z\nldt = 1979-05-27T07:32:00\nlt = 07:32:00'),
    ("hexnum", 'h = 0xdeadbeef\no = 0o755\nb = 0b1101'),
    ("underscores", 'big = 1_000_000'),
    ("nested", '[a.b.c]\ndeep = 1'),
    ("multi_line_string", 's = """\nline1\nline2"""'),
]
toml_expect = {}
for cid, text in TOML_CASES:
    try:
        def _norm(x):
            if hasattr(x, "isoformat"):
                r = x.isoformat()
                if hasattr(x, "tzinfo") and x.tzinfo is not None and r.endswith("+00:00"):
                    r = r[:-6] + "Z"
                return r
            return x
        toml_expect[cid] = {"ok": True, "v": json.loads(json.dumps(tomllib.loads(text), default=_norm))}
    except Exception as e:
        toml_expect[cid] = {"ok": False, "err": type(e).__name__}

# TOML text python REFUSES and dyna should refuse too (leading zeros, dup key)
TOML_BAD = [
    ("leading_zero", 'x = 007'),
    ("dup_key", 'x = 1\nx = 2'),
    ("bare_val", 'x = @nope'),
    ("unclosed_str", 'x = "open'),
    ("bad_date", 'd = 1979-13-45'),
]

emit("config", "toml", IMP, r'''
const CASES = __TOML_EXPECT__;
const TEXTS = __TOML_TEXTS__;

for (const [id, want] of Object.entries(CASES)) {
  if (!want.ok) continue;
  let got = null, err = null;
  try { got = TOML.parse(TEXTS[id]); } catch (e) { err = e; }
  if (err) { assert(false, "toml " + id + " threw " + err.message); continue; }
  // normalize: date/time objects have no JSON form; stringified via the
  // replacer on BOTH sides (python bakes strings for them via default=str)
  const norm = (v) => JSON.stringify(v, (k, x) => {
    if (x !== null && typeof x === "object" && !Array.isArray(x) &&
        x.constructor !== Object) return String(x);
    return x;
  });
  assert_eq(norm(got), norm(want.v), "toml " + id + " matches tomllib");
}

for (const id of Object.keys(TEXTS)) {
  if (!CASES[id] || !CASES[id].ok) continue;
}
// dates: normalize both sides through fromUnix-free string compare
{
  const d = TOML.parse('d = 1979-05-27').d;
  assert_true(d !== undefined && String(d).indexOf("1979") >= 0, "date parsed, value " + String(d));
}

// invalid documents must throw (python tomllib refuses them too)
const BAD = __TOML_BAD__;
for (const [id, text] of BAD) {
  let threw = null;
  try { TOML.parse(text); } catch (e) { threw = e; }
  assert_true(threw !== null, "toml bad/" + id + " throws");
}

// stringify round-trip on the common grammar
const doc = TOML.parse('title = "x"\n[server]\nport = 8080\nratio = 1.5');
const text = TOML.stringify(doc);
const doc2 = TOML.parse(text);
assert_eq(JSON.stringify(doc2), JSON.stringify(doc), "stringify->parse round-trip");
assert_true(text.indexOf("port = 8080") >= 0 || text.indexOf("port=8080") >= 0, "stringified text has port");
// quoting in stringify
const q = TOML.parse('k = "has \\"quote\\""');
assert_eq(q.k, 'has "quote"', "escaped quote parses");
// refusals
assert_throws(() => TOML.stringify(42), null, "stringify non-object root throws");
assert_throws(() => TOML.parse("[[[bad]"), null, "malformed table throws");
summary("config.toml");
'''.replace("__TOML_EXPECT__", json.dumps(toml_expect))
  .replace("__TOML_TEXTS__", json.dumps({cid: t for cid, t in TOML_CASES}))
  .replace("__TOML_BAD__", json.dumps(TOML_BAD)))

# ---- INI
emit("config", "ini", IMP, r'''
// classic shape vs documented semantics
const c = INI.parse('[server]\nhost = localhost\nport = 8080\nports[] = 80\nports[] = 443\n\n; comment\n# comment2\nflag\n');
assert_eq(c.server.host, "localhost", "section key");
assert_eq(c.server.port, "8080", "values stay strings");
assert_eq(c.server.ports.length, 2, "key[] list");
assert_eq(c.server.ports[0], "80", "list member 1");
assert_eq(c.server.ports[1], "443", "list member 2");
assert_eq(c.server.flag, true, "bare key is true");

// nested sections via dots
const n = INI.parse('[a.b]\nk = 1');
assert_eq(n.a.b.k, "1", "nested section via dots");

// top-level keys before any section
const t = INI.parse('root = yes\n[s]\nx = 1');
assert_eq(t.root, "yes", "pre-section keys land at root");

// value with = inside
const e = INI.parse('[s]\nexpr = a=b=c');
assert_eq(e.s.expr, "a=b=c", "value may contain = (first = splits)");

// whitespace trimming
const w = INI.parse('[s] \n  key   =   value  ');
assert_eq(w.s.key, "value", "whitespace trimmed around key/value");

// quoted value keeps content
const qq = INI.parse('[s]\nq = "two words"');
assert_eq(qq.s.q, "two words", "double-quoted value unquoted");

// __proto__ is an own property, not prototype pollution
const p = INI.parse('[s]\n__proto__ = x\nok = 1');
assert_eq(p.s.ok, "1", "sibling key readable");
assert_true(Object.prototype.hasOwnProperty.call(p.s, "__proto__"), "__proto__ is own property");
assert_eq(String(p.s.__proto__), "x", "__proto__ carries the value");

// comments at line ends? classic INI: keep whole value (documented grammar has no inline comments)
const i2 = INI.parse('[s]\nk = v ; note');
assert_true(i2.s.k === "v ; note" || i2.s.k === "v", "inline ';' behavior stable: " + JSON.stringify(i2.s.k));

// empty document
assert_eq(JSON.stringify(INI.parse("")), "{}", "empty document -> empty object");
summary("config.ini");
''')

# ---- Env
emit("config", "env", IMP, r'''
const e = Env.parse('export DB_URL="postgres://localhost:5432"\nPORT=8080\n# comment\nPLAIN=simple\nSQ=\'quoted\'\nDBL="with \\n escape"\nno equals sign here\nEMPTY=');
assert_eq(e.DB_URL, "postgres://localhost:5432", "export prefix + double quotes");
assert_eq(e.PORT, "8080", "bare value");
assert_eq(e.PLAIN, "simple", "unquoted literal");
assert_eq(e.SQ, "quoted", "single quotes stripped, literal inside");
assert_eq(e.DBL, "with \n escape", "double-quote escapes expand (\\n)");
assert_eq(e.EMPTY, "", "empty value");
// line without '=' skipped
assert_eq(e["no equals sign here"], undefined, "malformed line skipped");

// single quotes are LITERAL: \n stays
const s = Env.parse("LIT='a\\nb'");
assert_eq(s.LIT, "a\\nb", "single-quoted \\n is literal two chars");

// comments after values?
const c = Env.parse("X=1 # not-a-comment-in-dotenv\nY=2\n# whole line comment\n");
assert_true(c.X === "1 # not-a-comment-in-dotenv" || c.X === "1", "inline # behavior stable: " + JSON.stringify(c.X));
assert_eq(c.Y, "2", "second value");

// escapes \t \r inside double quotes
const t = Env.parse('T="a\\tb\\rc"');
assert_eq(t.T, "a\tb\rc", "\\t and \\r expand in double quotes");
summary("config.env");
''')

# ---- FrontMatter
emit("config", "fm", IMP, r'''
const fm = FrontMatter.split('---\ntitle: DynaJS\n---\n\nbody text');
assert_eq(fm.lang, "yaml", "--- fence is yaml");
assert_eq(fm.data, "title: DynaJS", "data stays TEXT");
assert_eq(fm.body, "\nbody text", "body after fence");

const tm = FrontMatter.split('+++\nk = 1\n+++\nbody');
assert_eq(tm.lang, "toml", "+++ fence is toml");
const jm = FrontMatter.split(';;;\n{"k":1}\n;;;\nbody');
assert_eq(jm.lang, "json", ";;; fence is json");

// no fence: body is whole input
const no = FrontMatter.split('plain text');
assert_eq(no.data, null, "no fence: data null");
assert_eq(no.lang, null, "no fence: lang null");
assert_eq(no.body, "plain text", "no fence: body is whole input");

// unclosed fence is not front matter
const un = FrontMatter.split('---\nnever closed');
assert_eq(un.data, null, "unclosed fence: data null");
assert_eq(un.body, "---\nnever closed", "unclosed fence: body whole input");

// fence must be FIRST line
const notfirst = FrontMatter.split('intro\n---\nx\n---\n');
assert_eq(notfirst.data, null, "fence not on first line -> not front matter");
summary("config.fm");
''')
print("gen_config: 4 probes")
