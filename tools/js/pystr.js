const SURR = /[\uD800-\uDFFF]/;
export function pySlice(s, n) {
    if (s.length <= n || !SURR.test(s)) return s.slice(0, n);
    return Array.from(s).slice(0, n).join("");
}
