/* oracle_line_col.js -- get_line_col's memchr scan against the byte loop.
 *
 * This function computes the line and column of every syntax error and of
 * every function's debug position, so a rewrite of it is a rewrite of what the
 * engine SAYS about broken code. The hash below is over the reported position
 * of a few thousand errors placed deliberately at boundaries:
 *
 *   - column 0 of a line, and the last column of a line
 *   - immediately before and after a newline
 *   - inside and after multi-byte UTF-8, whose continuation bytes must NOT
 *     count as columns
 *   - a file with no newline at all, and one that is only newlines
 *   - CR, CRLF and lone LF, since only LF is a line terminator here
 *   - the backward-scan path of get_line_col_cached, reached by nesting
 *     functions so positions are visited out of order
 */
function fnv(s) {
    let h = 2166136261 >>> 0;
    for (let i = 0; i < s.length; i++) {
        h ^= s.charCodeAt(i) & 0xff;
        h = Math.imul(h, 16777619) >>> 0;
        h ^= (s.charCodeAt(i) >>> 8);
        h = Math.imul(h, 16777619) >>> 0;
    }
    return h >>> 0;
}
let acc = 0, cases = 0, withPos = 0;

/* The position is what is being pinned, so the message is hashed whole -- it
 * carries "file:line:col" in the stack. */
function feed(src) {
    cases++;
    let out;
    try {
        eval(src);
        out = "OK";
    } catch (e) {
        out = String(e.stack || (e.name + ":" + e.message));
        /* keep only the position, so an unrelated message change is not a
         * false mismatch */
        const m = out.match(/:(\d+):(\d+)/);
        out = m ? ("POS" + m[1] + ":" + m[2]) : ("MSG" + e.name);
        if (m) withPos++;
    }
    acc = (Math.imul(acc, 31) + fnv(out)) >>> 0;
}

const NL = "\n";

/* 1. a bad token at every column of a line, and on every line of a file */
for (let line = 0; line < 12; line++) {
    for (let col = 0; col < 12; col++) {
        let src = "";
        for (let l = 0; l < line; l++) src += "var l" + l + " = 1;" + NL;
        src += " ".repeat(col) + "@";
        feed(src);
    }
}

/* 2. multi-byte UTF-8 before the error: continuation bytes are not columns */
const WIDE = ["é", "€", "😀", "日本語"];
for (const w of WIDE) {
    for (let n = 0; n <= 6; n++) {
        feed('var s = "' + w.repeat(n) + '"; @');
        feed('var s = "' + w.repeat(n) + '";' + NL + "@");
        feed("// " + w.repeat(n) + NL + "@");
    }
}

/* 3. newline shapes. Only LF is a terminator; CR is not. */
for (const sep of [NL, "\r" + NL, "\r", NL + NL, NL + "\r" + NL]) {
    feed("var a = 1;" + sep + "@");
    feed("var a = 1;" + sep + "var b = 2;" + sep + "@");
}

/* 4. degenerate files */
feed("@");                                   /* error at 0:0 */
feed(NL.repeat(20) + "@");                   /* only newlines before it */
feed("x".repeat(500) + NL + "@");            /* one very long line */
feed("var a = 1;".repeat(200) + "@");        /* no newline at all */
feed(NL.repeat(200));                        /* no error at all */

/* 5. the BACKWARD path of get_line_col_cached: nested function definitions
 *    make the parser revisit earlier positions, so the cache walks back. */
for (let depth = 1; depth <= 8; depth++) {
    let src = "";
    for (let d = 0; d < depth; d++)
        src += "function f" + d + "(){" + NL + "  var v" + d + " = " + d + ";" + NL;
    src += "  @" + NL;
    for (let d = 0; d < depth; d++) src += "}" + NL;
    feed(src);
}

/* 6. errors deep inside a long, many-lined program -- the common shape */
for (let n of [10, 50, 200, 500]) {
    let src = "";
    for (let i = 0; i < n; i++) src += "function g" + i + "(a,b){ return a+b; }" + NL;
    src += "@" + NL;
    feed(src);
    /* and with wide characters sprinkled through, so column counting matters */
    let src2 = "";
    for (let i = 0; i < n; i++) src2 += "/* é" + i + " */ function h" + i + "(){}" + NL;
    src2 += "  @" + NL;
    feed(src2);
}

print("oracle_line_col: " + cases + " cases, " + withPos + " positions, hash " +
      acc.toString(16));
