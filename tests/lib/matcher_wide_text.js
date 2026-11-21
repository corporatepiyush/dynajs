export const PIECES = ["ab", "é", "→", "😀", "aa", " x", "a→a", "😀a"];

export function wideText(n, seed = 7) {
    let s = "", x = seed;
    while (s.length < n) {
        x = (x * 1103515245 + 12345) >>> 0;
        s += PIECES[x % PIECES.length];
    }
    return s;
}

export function oracle(text, pats) {
    const out = [];
    pats.forEach((p, index) => {
        for (let at = text.indexOf(p); at !== -1; at = text.indexOf(p, at + 1)) out.push({ index, at });
    });
    return out.sort((u, v) => u.at - v.at || u.index - v.index);
}

export const byAt = (hits) => hits.map((h) => ({ index: h.index, at: h.at })).sort((u, v) => u.at - v.at || u.index - v.index);
