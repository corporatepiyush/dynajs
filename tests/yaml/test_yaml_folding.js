// flags: --std
import { Parse } from "dyna:yaml";
import { Path, writeFile, makeDir, removeAll } from "dyna:file";
import * as os from "os";
import * as std from "std";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
const sh = (cmd) => os.exec(["/bin/sh", "-c", cmd]);

const TMP = std.getenv("TMPDIR") || "/tmp";
const dir = new Path(TMP, "yaml-fold-" + (Date.now() % 10000000));
removeAll(dir);
makeDir(dir, { recursive: true });

const have = sh("python3 -c 'import yaml; assert yaml.__version__ == \"6.0.3\"' 2>/dev/null") === 0;
let captureSeq = 0;
function capture(cmd) {
    const out = String(dir) + "/capture." + (++captureSeq) + ".out";
    if (sh(cmd + " > " + out + " 2>/dev/null") !== 0) return null;
    const f = std.open(out, "r");
    if (!f) return null;
    const s = f.readAsString();
    f.close();
    return s;
}
if (!have) {
    if (std.getenv("DYNAJS_REQUIRE_TOOLS") === "1")
        throw new Error("REQUIRED oracle missing: python3 + PyYAML 6.0.3 -- the differential cannot run");
    print("test_yaml_folding: SKIPPED LOUDLY -- no python3 + PyYAML 6.0.3; " +
          "the differential half did NOT run (DYNAJS_REQUIRE_TOOLS=1 makes this a failure)");
}

const TAB_DIVERGENT = ["sq_tab_indent_cont", "dq_tab_indent_cont"];
const DOCS = {
    sq_single_break: 'k: \'one\n two\'\n',
    dq_single_break: 'k: "one\n two"\n',
    sq_two_breaks: 'k: \'one\n\n two\'\n',
    dq_two_breaks: 'k: "one\n\n two"\n',
    sq_three_breaks: 'k: \'one\n\n\n two\'\n',
    dq_three_breaks: 'k: "one\n\n\n two"\n',
    sq_mixed_paragraphs: 'k: \'a\n\n b\n\n\n c\'\n',
    dq_mixed_paragraphs: 'k: "a\n\n b\n\n\n c"\n',

    sq_trailing_spaces: 'k: \'one   \n two\'\n',
    dq_trailing_spaces: 'k: "one   \n two"\n',
    sq_trailing_tab: 'k: \'one\t\n two\'\n',
    dq_trailing_tab: 'k: "one\t\n two"\n',

    sq_deep_indent: 'k: \'one\n      two\'\n',
    dq_deep_indent: 'k: "one\n      two"\n',
    sq_tab_indent_cont: 'k: \'one\n\t two\'\n',
    dq_tab_indent_cont: 'k: "one\n\t two"\n',

    sq_leading_break: 'k: \'\n one\'\n',
    dq_leading_break: 'k: "\n one"\n',
    sq_leading_breaks: 'k: \'\n\n one\'\n',
    sq_trailing_break: 'k: \'one\n\'\n',
    sq_trailing_breaks: 'k: \'one\n\n\'\n',
    sq_close_on_own_line: 'k: \'abc\n\'\n',
    sq_breaks_only: 'k: \'\n\n\'\n',
    sq_break_then_space: 'k: \'one\n  x\'\n',

    dq_esc_break: 'k: "one \\\n two"\n',
    dq_esc_break_tight: 'k: "one\\\ntwo"\n',
    dq_esc_break_blank: 'k: "one \\\n\n two"\n',
    dq_esc_break_blanks2: 'k: "one \\\n\n\n two"\n',
    dq_two_esc_breaks: 'k: "a \\\n b \\\n c"\n',
    dq_esc_space_then_break: 'k: "one\\ \n two"\n',
    dq_raw_spaces_esc_break: 'k: "one  \\\n two"\n',

    dq_escapes: 'k: "a\\tb\\nc\\\\d\\"e"\n',
    dq_literal_dquote_in_sq: "k: 'a\n b\"c'\n",
    sq_doubled_quote: "k: 'a\n b''c'\n",
    dq_quote_inside: 'k: "a\n b\'c"\n',

    sq_crlf: 'k: \'one\r\n two\'\r\n',
    dq_crlf: 'k: "one\r\n\r\n two"\r\n',
    dq_esc_break_crlf: 'k: "one \\\r\n two"\r\n',

    sq_mid_tab: 'k: \'a\tb\n c\'\n',
    dq_mid_spaces: 'k: "a  b\n c"\n',

    dq_docsep_dash4_cont: 'k: "a\n---- b"\n',
    dq_docsep_dashx_cont: 'k: "a\n---x"\n',
    dq_docsep_dotx_cont: 'k: "a\n...x"\n',
    dq_docsep_dots4_cont: 'k: "a\n.... b"\n',
    dq_docsep_dash_final: 'k: "a\n---"\n',
    sq_docsep_dash_final: 'k: \'a\n---\'\n',
    dq_docsep_dash4_final: 'k: "a\n----"\n',
    dq_docsep_dots4_final: 'k: "a\n...."\n',
    dq_docsep_dash_marker_sp: 'k: "a\n--- b"\nj: 1\n',
    dq_docsep_dash_marker_eol: 'k: "a\n---\nb"\nj: 1\n',
    dq_docsep_dots_marker_eol: 'k: "a\n...\nb"\nj: 1\n',
    dq_docsep_dash_marker_tab: 'k: "a\n---\tx"\nj: 1\n',

    dq_esc_break_tab_cont: 'k: "a\\\n\tb"\n',
    dq_esc_break_sp_tab_cont: 'k: "a\\\n \tb"\n',
    dq_esc_break_tab_sp_cont: 'k: "a\\\n\t b"\n',

    sq_ws_only_line: 'k: \'a\n   \n b\'\n',
    dq_ws_only_line: 'k: "a\n   \n b"\n',

    nested: 'm:\n  a: "x\n  y"\n  b: 2\nlist:\n  - \'p\n\n q\'\n  - tail\n',
    two_keys: 'first: "a\n b"\nsecond: \'c\n d\'\n',
    hash_in_scalar: "k: 'a\n # b'\n",
    colon_in_scalar: 'k: "a: b\n c"\n',
};

{
    eq(Parse('k: "one\n  two"').k, "one two", "anchor: one break folds to a space");
    eq(Parse('k: "one\n\n two"').k, "one\ntwo", "anchor: two breaks fold to a line feed");
    eq(Parse('k: "one \\\n two"').k, "one two", "anchor: an escaped break joins");
    eq(Parse('k: "one\\\ntwo"').k, "onetwo", "anchor: escaped break eats the lead whitespace");
    eq(Parse("k: 'abc\n'").k, "abc ", "anchor: a trailing single break still folds to a space");
    eq(Parse('k: "a\n---- b"').k, "a ---- b",
       "anchor: `---- b` is content (the marker is not terminated at 3 bytes)");
    eq(Parse('k: "a\n---x"').k, "a ---x",
       "anchor: `---x` is content");
    eq(Parse('k: "a\n...x"').k, "a ...x",
       "anchor: `...x` is content");
    eq(Parse('k: "a\n.... b"').k, "a .... b",
       "anchor: `.... b` is content");
    eq(Parse('k: "a\n---"').k, "a ---",
       "anchor: a scalar-final `---\"` (closing quote follows) is content");
    eq(Parse('k: "a\\\n\tb"').k, "ab",
       "anchor: an escaped break eats a tab-prefixed continuation's whitespace");
}

{
    let got = "";
    try { Parse('k: "one\n  two'); } catch (e) { got = String(e.message); }
    assert(got.length > 0, "an unterminated multi-line quoted scalar is refused");
    got = "";
    try { Parse('k: "one\n--- two"\n'); } catch (e) { got = String(e.message); }
    assert(/separator/i.test(got),
           "a document separator at a continuation head is refused by name (got: " + got + ")");
    got = "";
    try { Parse('k: "one\n... two"\n'); } catch (e) { got = String(e.message); }
    assert(/separator/i.test(got),
           "the ... form of it is refused too (got: " + got + ")");
    got = "";
    try { Parse('k: "one\n---\ntwo"\n'); } catch (e) { got = String(e.message); }
    assert(/separator/i.test(got),
           "a TERMINATED marker at end-of-line is refused too (got: " + got + ")");
    got = "";
    try { Parse('k: "one\n...\ntwo"\n'); } catch (e) { got = String(e.message); }
    assert(/separator/i.test(got),
           "the terminated ... form at end-of-line is refused too (got: " + got + ")");
    got = "";
    try { Parse('k: "one\n  two"\n  trailing: junk\n'.replace("trailing: junk", "  \"still open"));
    } catch (e) { got = String(e.message); }
    assert(got.length > 0, "a scalar that never closes is refused");
}

if (have) {
    const py = new Path(dir, "oracle.py");
    writeFile(py, [
        "import sys, json, yaml",
        "src = open(sys.argv[1], encoding='utf-8').read()",
        "try:",
        "    v = yaml.safe_load(src)",
        "    print(json.dumps({'ok': True, 'value': v}))",
        "except Exception as e:",
        "    print(json.dumps({'ok': False}))",
        "",
    ].join("\n"));
    let diff = 0;
    for (const [name, doc] of Object.entries(DOCS)) {
        const f = new Path(dir, name + ".yaml");
        writeFile(f, doc);
        const raw = capture("python3 " + py + " " + f + " 2>/dev/null");
        let out;
        try {
            out = JSON.parse(raw);
        } catch (e) {
            assert(false, name + ": oracle output parses (got: " + raw + ")");
            continue;
        }
        if (!out.ok) {
            let threw = false;
            try { Parse(doc); } catch (e) { threw = true; }
            assert(threw, name + ": PyYAML refuses and so does the engine");
            continue;
        }
        if (TAB_DIVERGENT.indexOf(name) >= 0) {
            let got = "";
            try { Parse(doc); } catch (e) { got = String(e.message); }
            assert(/tab/i.test(got),
                   name + ": pinned divergence -- the engine refuses the tab " +
                   "indent while PyYAML folds it away (got: " + got + ")");
            continue;
        }
        let mine;
        try {
            mine = Parse(doc);
        } catch (e) {
            diff++;
            print("DIFF " + name + ": engine REFUSED " + String(e.message) +
                  " but PyYAML parsed " + JSON.stringify(out.value));
            assert(false, name + ": differential agreement");
            continue;
        }
        const a = JSON.stringify(mine);
        const b = JSON.stringify(out.value);
        if (a !== b) {
            diff++;
            print("DIFF " + name + ": engine " + a + " vs PyYAML " + b);
        }
        assert(a === b, name + ": engine and PyYAML 6.0.3 agree");
    }
    eq(diff, 0, "no differential mismatches across " + Object.keys(DOCS).length + " shapes");
}

removeAll(dir);
print("test_yaml_folding: " + (n - fails) + "/" + n + " assertions" +
      (fails ? " -- " + fails + " FAILURES" : " all passed") +
      (have ? "" : " (DIFFERENTIAL SKIPPED: no PyYAML 6.0.3)"));
if (fails) throw new Error(fails + " failures");
