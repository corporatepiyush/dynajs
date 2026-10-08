let lines = 0;
function emit(s) { lines++; print(s); }

const HAY = [
    "", "a", "aa", "aaa", "abcabcabc", "mississippi",
    "the quick brown fox jumps over the lazy dog",
    "aaaaaaaaab", "ababababab", "x".repeat(100) + "needle" + "y".repeat(100),
    "éèê",
    "café au lait",
    "日本語",
    "a日b本c",
    "\u{1F600}\u{1F600}x",
    "mixed café 日本 \u{1F600} end",
    "\0embedded\0nul\0",
];
const NEEDLE = [
    "", "a", "aa", "aaa", "ab", "abc", "b", "z", "ss", "issi",
    "needle", "the", "fox", "dog", "é", "日", "\u{1F600}",
    "\0", "nul", "x".repeat(50), "aaaaaaaaab", "lait",
];
const FROM = [undefined, -5, 0, 1, 2, 5, 50, 1000];

for (const h of HAY) {
    for (const n of NEEDLE) {
        for (const f of FROM) {
            const a = f === undefined ? h.indexOf(n) : h.indexOf(n, f);
            const b = f === undefined ? h.lastIndexOf(n) : h.lastIndexOf(n, f);
            const c = f === undefined ? h.includes(n) : h.includes(n, f);
            emit(`i ${JSON.stringify(h)} ${JSON.stringify(n)} ${f} ${a} ${b} ${c}`);
        }
        emit(`s ${JSON.stringify(h.split(n))}`);
        emit(`r ${JSON.stringify(h.replace(n, "<>"))} ${JSON.stringify(h.replaceAll(n, "<>"))}`);
        emit(`x ${JSON.stringify(h.indexOfAll(n))} ${JSON.stringify(h.splitN(n, 2))}`);
        emit(`t ${h.startsWith(n)} ${h.endsWith(n)} ${JSON.stringify(h.trimPrefix(n))} ${JSON.stringify(h.trimSuffix(n))}`);
    }
}

function rng(seed) {
    let s = seed >>> 0;
    return () => { s = (Math.imul(s, 1103515245) + 12345) >>> 0; return s / 4294967296; };
}
{
    const r = rng(20260727);
    const alpha = "aab";
    for (let k = 0; k < 20000; k++) {
        let h = "", n = "";
        const hl = 1 + Math.floor(r() * 40), nl = 1 + Math.floor(r() * 6);
        for (let i = 0; i < hl; i++) h += alpha[Math.floor(r() * alpha.length)];
        for (let i = 0; i < nl; i++) n += alpha[Math.floor(r() * alpha.length)];
        const from = Math.floor(r() * (hl + 2));
        emit(`R ${h.indexOf(n, from)} ${h.lastIndexOf(n)} ${JSON.stringify(h.indexOfAll(n))}`);
    }
}

{
    const r = rng(99);
    const alpha = "ab日";
    for (let k = 0; k < 20000; k++) {
        let h = "", n = "";
        const hl = 1 + Math.floor(r() * 30), nl = 1 + Math.floor(r() * 4);
        for (let i = 0; i < hl; i++) h += alpha[Math.floor(r() * alpha.length)];
        for (let i = 0; i < nl; i++) n += alpha[Math.floor(r() * alpha.length)];
        emit(`W ${h.indexOf(n)} ${h.lastIndexOf(n)} ${JSON.stringify(h.split(n))}`);
    }
}

print("# oracle_string_indexof: " + lines + " cases");
