/* bench_string_ext.js — native String methods vs hand-written JS.
 * RATIO (native / JS) is the metric to trust under emulation; goal native < 1.0x.
 * Run: dynajs tests/bench_string_ext.js */

function best(fn, iters = 7) {
    let b = Infinity, acc = 0;
    for (let k = 0; k < iters; k++) {
        const t0 = performance.now();
        acc ^= fn() | 0;
        const dt = performance.now() - t0;
        if (dt < b) b = dt;
    }
    return [b, acc];
}

function row(name, native, js) {
    const [tn] = best(native), [tj] = best(js);
    const ratio = tn / tj;
    print(
        name.padEnd(16) +
        " native=" + tn.toFixed(2).padStart(8) + "ms" +
        " js=" + tj.toFixed(2).padStart(8) + "ms" +
        " ratio=" + ratio.toFixed(3) + (ratio < 1 ? "  (native faster)" : "  (JS faster)")
    );
}

// A long narrow (Latin1) string and a long wide (has an astral/BMP-wide char) string.
const base = "The quick brown fox jumps over the lazy dog.  ";
const narrow = base.repeat(1500);            // ~69 KB, Latin1
const wide = ("π " + base).repeat(1500);      // forces wide (UTF-16) storage
const messy = "   a   b\t\tc\n\n   d   ".repeat(2000);
const ITER = 60;

print("=== String `_` methods vs JS  (narrow len=" + narrow.length + ") ===");

row("reverse(narrow)",
    () => { let s = 0; for (let i = 0; i < ITER; i++) s ^= narrow.reverse().length; return s; },
    () => { let s = 0; for (let i = 0; i < ITER; i++) s ^= narrow.split("").reverse().join("").length; return s; });

row("reverse(wide)",
    () => { let s = 0; for (let i = 0; i < ITER; i++) s ^= wide.reverse().length; return s; },
    () => { let s = 0; for (let i = 0; i < ITER; i++) s ^= wide.split("").reverse().join("").length; return s; });

row("compact",
    () => { let s = 0; for (let i = 0; i < ITER; i++) s ^= messy.compact().length; return s; },
    () => { let s = 0; for (let i = 0; i < ITER; i++) s ^= messy.replace(/\s+/g, " ").trim().length; return s; });

row("isBlank",
    () => { let s = 0; for (let i = 0; i < ITER*20; i++) s ^= (narrow.isBlank()?1:0); return s; },
    () => { let s = 0; for (let i = 0; i < ITER*20; i++) s ^= (/^\s*$/.test(narrow)?1:0); return s; });

row("chars",
    () => { let s = 0; for (let i = 0; i < ITER; i++) s ^= base.repeat(200).chars().length; return s; },
    () => { let s = 0; for (let i = 0; i < ITER; i++) s ^= base.repeat(200).split("").length; return s; });

row("codes",
    () => { let s = 0; for (let i = 0; i < ITER; i++) s ^= base.repeat(200).codes().length; return s; },
    () => { let s = 0; for (let i = 0; i < ITER; i++) s ^= [...base.repeat(200)].map(c=>c.charCodeAt(0)).length; return s; });

print("=== HTML methods (SIMD scan) vs JS regex ===");
const htmlDoc = ('<div class="x">Hello & welcome to <b>Speed</b> &amp; <i>SIMD</i>! ' +
                 'Values: a < b > c, and "quotes" too. </div>\n').repeat(600);  // ~60KB narrow
print("htmlDoc len=" + htmlDoc.length);

row("escapeHTML",
    () => { let s = 0; for (let i = 0; i < ITER; i++) s ^= htmlDoc.escapeHTML().length; return s; },
    () => { let s = 0; for (let i = 0; i < ITER; i++) s ^= htmlDoc.replace(/&/g,"&amp;").replace(/</g,"&lt;").replace(/>/g,"&gt;").length; return s; });

const escaped = htmlDoc.escapeHTML();
row("unescapeHTML",
    () => { let s = 0; for (let i = 0; i < ITER; i++) s ^= escaped.unescapeHTML().length; return s; },
    () => { let s = 0; for (let i = 0; i < ITER; i++) s ^= escaped.replace(/&amp;/g,"&").replace(/&lt;/g,"<").replace(/&gt;/g,">").length; return s; });

row("stripTags",
    () => { let s = 0; for (let i = 0; i < ITER; i++) s ^= htmlDoc.stripTags().length; return s; },
    () => { let s = 0; for (let i = 0; i < ITER; i++) s ^= htmlDoc.replace(/<[^]+?>/g,"").length; return s; });

print("=== words / lines (SIMD scan) + base64 (SIMD kernel) ===");
const prose = ("The quick brown fox jumps over the lazy dog and then some more words here.\n").repeat(800); // ~60KB
print("prose len=" + prose.length);

row("words",
    () => { let s = 0; for (let i = 0; i < ITER; i++) s ^= prose.words().length; return s; },
    () => { let s = 0; for (let i = 0; i < ITER; i++) s ^= (prose.match(/\S+/g) || []).length; return s; });

row("lines",
    () => { let s = 0; for (let i = 0; i < ITER; i++) s ^= prose.lines().length; return s; },
    () => { let s = 0; for (let i = 0; i < ITER; i++) s ^= prose.trim().split("\n").length; return s; });

const b64src = prose.slice(0, 30000);
const jsB64 = (s) => { /* naive JS base64 of char codes (ASCII slice) — reference only */
    const T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let o = "", i = 0;
    for (; i + 2 < s.length; i += 3) {
        const n = (s.charCodeAt(i) << 16) | (s.charCodeAt(i+1) << 8) | s.charCodeAt(i+2);
        o += T[(n>>18)&63] + T[(n>>12)&63] + T[(n>>6)&63] + T[n&63];
    }
    return o;
};
row("encodeBase64",
    () => { let s = 0; for (let i = 0; i < ITER; i++) s ^= b64src.encodeBase64().length; return s; },
    () => { let s = 0; for (let i = 0; i < ITER; i++) s ^= jsB64(b64src).length; return s; });

print("done");
