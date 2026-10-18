// Error positions in LARGE sources. Above 32 KiB the compiler answers "which
// line and column is this byte" from a block index instead of walking from the
// last answer; the result must be the definition: line = newlines before the
// position, column = code points since the last newline. The expected values
// here are computed from the source TEXT by that definition, never by asking
// the engine twice.
let n = 0, failed = 0;
function ok(cond, what, detail) {
    n++;
    if (!cond) { failed++; print("  FAIL  " + what + (detail === undefined ? "" : "  [" + detail + "]")); }
}

// where does the engine point for `throw new Error(...)`? Learn the offset
// from a tiny source (far below the index threshold), then demand the same
// offset everywhere in the big ones.
function positionOf(err) {
    const m = /:(\d+):(\d+)\)?\s*$/m.exec(String(err.stack).split("\n")[0]);
    return m ? [Number(m[1]), Number(m[2])] : null;
}
function lineCol(src, offset) {
    let line = 1, col = 1;
    for (const ch of src.slice(0, offset)) {
        if (ch === "\n") { line++; col = 1; } else col++;
    }
    return [line, col];
}
const MARK = "throw new Error";
let dLine, dCol;
{
    const src = "var probe = function () {\n   " + MARK + "('c');\n};\nprobe;";
    let pos;
    try { (0, eval)(src)(); } catch (e) { pos = positionOf(e); }
    const want = lineCol(src, src.indexOf(MARK));
    ok(pos !== null, "an error thrown from eval'd code carries line:col", pos);
    dLine = pos[0] - want[0];
    dCol = pos[1] - want[1];
    ok(dLine === 0, "the reported line is the line of the throw statement", dLine);
}

// A generator of big sources: K functions, nested D deep, each throwing at a
// known place, separated by filler of the given flavour.
function build(k, depth, filler, nlEvery, nlRun) {
    const NL = "\n".repeat(nlRun || 1);
    let src = "var fns = [];\n";
    const sites = [];
    for (let i = 0; i < k; i++) {
        let open = "";
        for (let d = 0; d < depth; d++) open += "(function(){ var v" + d + " = " + JSON.stringify(filler(i, d)) + ";" + (nlEvery && (i + d) % nlEvery === 0 ? NL : " ");
        src += open + "fns.push(function f" + i + "(){ ";
        sites.push(src.length);
        src += MARK + "('s" + i + "'); });";
        for (let d = 0; d < depth; d++) src += " })();";
        src += nlEvery && i % nlEvery === 0 ? NL : " ";
    }
    return [src + "\nfns;", sites];
}
const flavours = {
    "ascii, one enormous line": [(i, d) => "x".repeat(40 + (i * 7 + d) % 90), 0],
    "ascii, many lines": [(i, d) => "y".repeat(10 + (i + d) % 60), 1],
    "2-byte text, one line": [(i, d) => "é".repeat(20 + (i * 3 + d) % 70), 0],
    "3-byte and 4-byte text, sparse newlines": [(i, d) => ("日本" + "😀").repeat(5 + (i + d) % 30) + "z".repeat(i % 67), 9],
    "mixed widths around block boundaries": [(i, d) => "a".repeat((i * 13 + d * 5) % 67) + "ñ".repeat((i * 5) % 9) + "😀".repeat(i % 3), 4],
    "runs of blank lines (several newlines inside one index block)": [(i, d) => "q".repeat(560 + (i + d) % 9), 1, 5],
};
for (const [name, [filler, nlEvery, nlRun]] of Object.entries(flavours)) {
    const [src, sites] = build(260, 3, filler, nlEvery, nlRun);
    ok(src.length > 48 * 1024, name + ": the source is past the index threshold", src.length);
    let fns;
    try { fns = (0, eval)(src); } catch (e) { ok(false, name + ": the generated source compiles", e.message); continue; }
    let bad = 0, first = "";
    for (let i = 0; i < fns.length; i++) {
        let pos = null;
        try { fns[i](); } catch (e) { pos = positionOf(e); }
        const want = lineCol(src, sites[i]);
        const exp = [want[0] + dLine, want[1] + dCol];
        if (!pos || pos[0] !== exp[0] || pos[1] !== exp[1]) {
            if (!bad) first = "site " + i + ": got " + pos + " want " + exp;
            bad++;
        }
    }
    ok(bad === 0, name + ": all " + fns.length + " throw sites report the defined line:col", bad + " wrong; " + first);
}

// Syntax errors are positioned by the same code; one near the end of a big
// single-line source must still name the right column.
{
    const pad = "var a=1;".repeat(9000);
    const src = pad + "var b = ;";
    let msg = "";
    try { (0, eval)(src); } catch (e) { msg = String(e.stack || e); }
    const m = /:(\d+):(\d+)/.exec(msg);
    ok(m && Number(m[1]) === 1 && Math.abs(Number(m[2]) - (pad.length + 9)) <= 1, "a syntax error at byte " + (pad.length + 8) + " of one long line is placed there", m && m[0]);
}

print("test_parse_linecol_index: " + (n - failed) + " passed, " + failed + " failed");
if (failed) throw new Error("test_parse_linecol_index: " + failed + " failures");
