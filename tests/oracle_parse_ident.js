/* oracle_parse_ident.js -- the ASCII identifier fast path against the general
 * one (CLAUDE.md section 2's differential-oracle rule for a parser rewrite).
 *
 * Compile the same source twice, once with -DJS_PARSE_IDENT_NO_ASCII, and the
 * hash below must match. What the fast path could get wrong:
 *
 *   - the BOUNDARY it stops at. lre_is_id_continue_byte must agree with
 *     lre_js_is_ident_next on every ASCII byte, or an identifier ends in the
 *     wrong place. Every byte 0..127 is tried as the second character here.
 *   - the buffer growth check. The run stops at the same bound the loop's own
 *     realloc uses, so identifiers either side of the 128-byte inline buffer
 *     and of several reallocs are exercised.
 *   - unicode and \u escapes, which must still leave the fast path.
 *   - `#private` names, where a '#' is already in the buffer before the run
 *     starts.
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
let acc = 0, cases = 0, errs = 0;
function feed(src) {
    cases++;
    let out;
    try { out = "V" + String(eval(src)); }
    catch (e) { errs++; out = "E" + e.name; }
    acc = (Math.imul(acc, 31) + fnv(out)) >>> 0;
}

/* 1. EVERY ascii byte as the second character of an identifier. Whatever the
 *    engine decides -- continue the name, end the token, or reject the
 *    program -- both builds must decide the same. */
for (let b = 0; b < 128; b++) {
    const ch = String.fromCharCode(b);
    feed("(function(){ var a" + ch + " = 1; return typeof a" + ch + "; })()");
    feed("(function(){ var a" + ch + "z = 1; return a" + ch + "z; })()");
}

/* 2. every length across the inline buffer boundary (128) and past several
 *    reallocs, so the run's bound is exercised where it actually bites */
for (let len = 1; len <= 300; len++) {
    const name = "v" + "x".repeat(len - 1);
    feed("(function(){ var " + name + " = " + (len % 97) + "; return " + name + "; })()");
}
for (const len of [400, 511, 512, 513, 1000, 4096]) {
    const name = "w" + "y".repeat(len);
    feed("(function(){ var " + name + " = 7; return " + name + "; })()");
}

/* 3. the characters that make an identifier stop being plain ASCII */
const MIX = ["café", "naïve", "Ünï", "日本語", "emoji", "$dollar", "_under",
             "a$b_c0", "$", "_", "$$", "__", "a1234567890"];
for (const nm of MIX)
    feed("(function(){ var " + nm + " = 1; return " + nm + "; })()");

/* 4. \\u escapes inside identifiers must still leave the fast path */
const BS = String.fromCharCode(92);
for (const nm of ["a" + BS + "u0062c", BS + "u0061bc", "ab" + BS + "u0063",
                  BS + "u{62}x", "a" + BS + "u{0063}"])
    feed("(function(){ var " + nm + " = 5; return " + nm + "; })()");

/* 5. private names: a '#' is in the buffer before the run begins */
for (let len = 1; len <= 140; len++) {
    const nm = "p".repeat(len);
    feed("(function(){ class C { #" + nm + " = 3; get(){ return this.#" + nm +
         "; } } return new C().get(); })()");
}

/* 6. keywords and near-keywords -- the atom the run produces is what decides
 *    whether the token is reparsed as a keyword */
for (const kw of ["if", "ifx", "let", "lets", "await", "awaited", "yield",
                  "yields", "class", "classy", "of", "off", "get", "getter",
                  "static", "statically", "constructor", "prototype"])
    feed("(function(){ var " + kw + "_ = 1; return typeof " + kw + "_; })()");

/* 7. many identifiers in one program, so the lexer runs the path repeatedly */
for (let i = 0; i < 100; i++) {
    let src = "(function(){ ";
    for (let j = 0; j < 20; j++) src += "var id_" + i + "_" + j + " = " + j + "; ";
    src += "return id_" + i + "_19; })()";
    feed(src);
}

print("oracle_parse_ident: " + cases + " cases, " + errs + " rejected, hash " +
      acc.toString(16));
