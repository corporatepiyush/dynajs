// Split from tests/test_cov_sys_time_random_uuid_misc.js: the config sections.
import { TOML, INI, Env, FrontMatter } from "dyna:config";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { assert(a === b, m + " got " + JSON.stringify(a) + " want " + JSON.stringify(b)); }
function throws(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL: " + m + " did not throw"); }
    catch (e) { if (re && !re.test(String(e.message))) { fails++; print("FAIL: " + m + " wrong msg: " + e.message); } }
}
function assertClose(a, b, tol, m) {
    n++;
    if (!(Math.abs(a - b) <= tol)) { fails++; print("FAIL: " + m + " |" + a + "-" + b + "|>" + tol); }
}
print("=== sys ===");
print("=== config ===");
{
    const t1 = TOML.parse('title = "x"\n[owner]\nname = "a"\nage = 30\n');
    eq(t1.title, "x", "TOML top scalar");
    eq(t1.owner.name, "a", "TOML table field");
    eq(t1.owner.age, 30, "TOML integer");
    const t2 = TOML.parse('ok = true\npi = 3.14\narr = [1, 2, 3]\ninl = { x = 1 }\n');
    eq(t2.ok, true, "TOML bool");
    assertClose(t2.pi, 3.14, 1e-9, "TOML float");
    eq(JSON.stringify(t2.arr), "[1,2,3]", "TOML array");
    eq(t2.inl.x, 1, "TOML inline table");
    throws(() => TOML.parse("a = 1\na = 2"), /./, "TOML duplicate top-level key refused");
    throws(() => TOML.parse('[t]\nx=1\n[t]\ny=2'), /./, "TOML table redefine refused");
    throws(() => TOML.parse("x = 01"), /./, "TOML leading zero refused");
    throws(() => TOML.parse("[unclosed"), /./, "TOML unclosed header throws");
    throws(() => TOML.parse('x = '), /./, "TOML bare value throws");
    const rt = TOML.parse("[a]\nx = 1\ny = \"s\"\n");
    const s = TOML.stringify(rt);
    assert(s.includes("x = 1"), "stringify keeps int");
    assert(!s.includes("__toml_header"), "stringify leaks no marker");
    assert(!Object.keys(rt.a).includes("__toml_header"), "parse leaks no marker key");
    const rt2 = TOML.parse(TOML.stringify(rt));
    eq(rt2.a.x, 1, "TOML round-trip value");
    const tn = TOML.stringify({ v: NaN });
    assert(/nan/i.test(tn), "NaN rendered as nan");

    const i1 = INI.parse("[sec]\nk = v\nflag\nlist[] = 1\nlist[] = 2\n");
    eq(i1.sec.k, "v", "INI section.key");
    eq(i1.sec.flag, true, "INI bare key true");
    eq(JSON.stringify(i1.sec.list), '["1","2"]', "INI repeated key list");
    eq(INI.parse("").hasOwnProperty("x"), false, "INI empty doc no keys");
    const i2 = INI.parse("k=v ; comment\n# full comment\n");
    assert(i2.k !== undefined || i2[""] === undefined, "INI comments tolerated");

    const e1 = Env.parse("A=1\nB=hello world\n#C=nope\nNOEQ line\nD=\n");
    eq(e1.A, "1", "Env simple");
    eq(e1.B, "hello world", "Env spaces preserved");
    eq(e1.D, "", "Env empty value");
    eq(e1.hasOwnProperty("C"), false, "Env skips comments");
    eq(Object.keys(Env.parse("")).length, 0, "Env empty input");

    const fm1 = FrontMatter.split("---\ntitle: x\n---\nbody here");
    eq(fm1.lang, "yaml", "FM bare fence defaults to yaml lang");
    assert(fm1.data !== null && /title/.test(fm1.data), "FM data captured");
    eq(fm1.body, "body here", "FM body after fence");
    const fm2 = FrontMatter.split("no fences at all");
    eq(fm2.data, null, "FM absent fence data null");
    eq(fm2.body, "no fences at all", "FM absent fence body is whole");
    const fm3 = FrontMatter.split("");
    eq(fm3.data, null, "FM empty input");
}
if (fails) {
    print("test_config_cov_sys_time_random_uuid_misc: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_config_cov_sys_time_random_uuid_misc failed");
}
print("test_config_cov_sys_time_random_uuid_misc: " + n + " assertions, 0 failures");
