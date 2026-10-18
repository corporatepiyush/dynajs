// A script that dies while a SAXParser holds its event templates must exit with
// the error, not abort in the runtime's teardown.
import { Exec, cwd } from "dyna:sys";
import { makeTempDir, writeFile } from "dyna:file";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; print("FAIL " + w + (d ? "  [" + d + "]" : "")); } };
const dir = makeTempDir("saxdown");
const file = dir.join("sax.js");
writeFile(file, `
    import { SAXParser } from "dyna:xml";
    const p = new SAXParser();
    p.write('<a x="1"><b>t</b><c/></a>');
    for (let e; (e = p.next()) !== null;) globalThis.last = e;
    const q = new SAXParser({ onOpen() {}, onText() {}, onClose() {} });
    q.write('<r><s k="v">x</s></r>');
    await null;
    throw new Error("boom");`);
const r = Exec(cwd() + "/dynajs", [file.toString()], { timeoutMs: 20000 });
const err = String(r.stderr);
ok(r.code === 1 && r.signal === null, "exits 1 with no signal", `code=${r.code} signal=${r.signal}`);
ok(/Error: boom/.test(err), "the error is reported", err.slice(-300));
ok(!/Assertion failed/.test(err), "no teardown assertion", err.slice(-300));
print(`test_xml_sax_teardown: ${pass} passed, ${fail} failed`);
if (fail) throw new Error(`${fail} failure(s)`);
