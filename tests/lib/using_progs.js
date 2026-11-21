// tests/lib/using_progs.js -- random program generator for `using` / `await using` plus a reference model:
// the same program hand-desugared to try/catch/finally that follows the spec's DisposeResources step by step
// (reverse order, SuppressedError(new, previous), method captured at the declaration, needsAwait/hasAwaited:
// one Await per async-hint record whose call returned normally, a null `await using` costs at most one Await
// and none once a real Await happened in the same scope). Plain ES module: node runs it unchanged as the oracle
// (V8 implements explicit resource management), dynajs runs it in tests/core/test_using_syntax.js.

export function res(log, id, mode) {
    if (mode === "null") return null;
    if (mode === "undef") return undefined;
    if (mode === "bad") return { id };
    if (mode === "prim") return 7;
    return {
        id,
        [Symbol.dispose]() {
            log.push("d" + id);
            if (mode === "throw") throw new Error("x" + id);
        },
    };
}

export function ares(log, id, mode) {
    if (mode === "null") return null;
    if (mode === "undef") return undefined;
    if (mode === "bad") return { id };
    if (mode === "prim") return 7;
    if (mode === "sync") return { [Symbol.dispose]() { log.push("s" + id); } };
    if (mode === "syncthrow") return { [Symbol.dispose]() { log.push("s" + id); throw new Error("z" + id); } };
    if (mode === "athrow") return { [Symbol.asyncDispose]() { log.push("a" + id); throw new Error("w" + id); } };
    return {
        async [Symbol.asyncDispose]() {
            log.push("a" + id);
            await null;
            log.push("A" + id);
            if (mode === "throw") throw new Error("y" + id);
        },
    };
}

export function ser(e) {
    if (e instanceof SuppressedError) return "S(" + ser(e.error) + "," + ser(e.suppressed) + ")";
    if (e instanceof TypeError) return "TypeError";
    if (e instanceof Error) return e.message;
    return "v:" + String(e);
}

export function prng(seed) {
    let s = seed >>> 0;
    return () => { s ^= s << 13; s >>>= 0; s ^= s >>> 17; s ^= s << 5; s >>>= 0; return s / 4294967296; };
}

const SMODES = ["ok", "ok", "ok", "throw", "null", "undef", "bad", "prim"];
const AMODES = ["ok", "ok", "ok", "throw", "null", "undef", "bad", "sync", "syncthrow", "athrow", "prim"];

export function genProgram(rnd, isAsync) {
    let id = 0, tmp = 0, blk = 0;
    const pick = (a) => a[Math.floor(rnd() * a.length)];
    function decl(aw) { return { id: ++id, aw, mode: pick(aw ? AMODES : SMODES) }; }
    function stmts(depth, inLoop) {
        const n = 1 + Math.floor(rnd() * 4);
        const out = [];
        for (let i = 0; i < n; i++) out.push(stmt(depth, inLoop));
        return out;
    }
    function stmt(depth, inLoop) {
        const r = rnd();
        if (r < 0.32) {
            const aw = isAsync && rnd() < 0.6;
            const k = 1 + Math.floor(rnd() * 2);
            const decls = [];
            for (let j = 0; j < k; j++) decls.push(decl(aw));
            return { t: "using", decls };
        }
        if (r < 0.48) return { t: "log", v: ++id };
        if (r < 0.52 && isAsync) return { t: "await", v: ++id };
        if (r < 0.59) return { t: "throw", v: ++id };
        if (r < 0.64) return { t: "return" };
        if (r < 0.69 && inLoop) return { t: rnd() < 0.5 ? "break" : "continue" };
        if (depth < 3 && r < 0.78) return { t: "block", body: stmts(depth + 1, inLoop) };
        if (depth < 3 && r < 0.85) return { t: "loop", body: stmts(depth + 1, true) };
        if (depth < 3 && r < 0.93) {
            const aw = isAsync && rnd() < 0.6;
            return { t: "forof", aw, items: [decl(aw), decl(aw)], body: stmts(depth + 1, true) };
        }
        if (depth < 3) return { t: "try", body: stmts(depth + 1, inLoop) };
        return { t: "log", v: ++id };
    }
    const init = (d) => (d.aw ? "A" : "R") + "(" + d.id + ",'" + d.mode + "')";
    function common(s, emit) {
        switch (s.t) {
        case "log": return "L.push('l" + s.v + "');";
        case "await": return "await null; L.push('w" + s.v + "');";
        case "throw": return "if (L.length % 3 !== 2) throw new Error('t" + s.v + "');";
        case "return": return "return;";
        case "break": return "break;";
        case "continue": return "continue;";
        case "block": return "{ " + emit(s.body) + " }";
        case "loop": { const i = "i" + (++tmp); return "for (let " + i + " = 0; " + i + " < 2; " + i + "++) { " + emit(s.body) + " }"; }
        case "try": return "try { " + emit(s.body) + " } catch (e) { L.push('c:' + ser(e)); }";
        }
        return null;
    }
    function emitUsing(list) {
        return list.map((s) => {
            if (s.t === "using") return (s.decls[0].aw ? "await using " : "using ") + s.decls.map((d) => "u" + d.id + " = " + init(d)).join(", ") + ";";
            if (s.t === "forof") return "for (" + (s.aw ? "await using" : "using") + " f" + s.items[0].id + " of [" + s.items.map(init).join(",") + "]) { " + emitUsing(s.body) + " }";
            return common(s, emitUsing);
        }).join(" ");
    }
    function declare(d, v, m, src) {
        const typeErr = "{ if (typeof " + v + " !== 'object' && typeof " + v + " !== 'function') throw new TypeError('t'); ";
        if (!d.aw) {
            return "const " + v + " = " + src + "; let " + m + "; if (" + v + " != null) " + typeErr +
                m + " = " + v + "[Symbol.dispose]; if (typeof " + m + " !== 'function') throw new TypeError('m'); }";
        }
        return "const " + v + " = " + src + "; let " + m + "; if (" + v + " != null) " + typeErr +
            m + " = " + v + "[Symbol.asyncDispose]; if (" + m + " == null) { const s = " + v + "[Symbol.dispose]; " +
            "if (typeof s !== 'function') throw new TypeError('m'); " + m + " = function () { return (async () => { s.call(this); })(); }; } " +
            "else if (typeof " + m + " !== 'function') throw new TypeError('m'); }";
    }
    function disposeTail(d, rest, v, m, st, first) {
        const c = "c" + (++tmp), e = "e" + tmp;
        let call;
        if (!d.aw) call = (st ? "if (" + st + ".na && !" + st + ".ha) { await undefined; " + st + ".na = false; } " : "") + "if (" + v + " != null) " + m + ".call(" + v + ");";
        else call = "if (" + v + " != null) { const r = " + m + ".call(" + v + "); try { await r; } finally { " + st + ".ha = true; } } else " + st + ".na = true;";
        return "let " + c + " = false, " + e + "; try { " + rest + " } catch (x) { " + c + " = true; " + e + " = x; } finally { " +
            "try { " + call + " } catch (q) { if (" + c + ") " + e + " = new SuppressedError(q, " + e + "); else { " + c + " = true; " + e + " = q; } } " +
            (first && st ? "if (" + st + ".na && !" + st + ".ha) await undefined; " : "") +
            "if (" + c + ") throw " + e + "; }";
    }
    function hasAwaitUsing(list) { return list.some((s) => s.t === "using" && s.decls[0].aw); }
    function emitModel(list, st, seen) {
        if (st === undefined && hasAwaitUsing(list)) {
            const name = "st" + (++blk);
            return "const " + name + " = { na: false, ha: false }; " + emitModel(list, name, false);
        }
        let out = "";
        for (let i = 0; i < list.length; i++) {
            const s = list[i];
            if (s.t === "using") {
                let first = !seen;
                const nest = (decls) => {
                    if (!decls.length) return emitModel(list.slice(i + 1), st, true);
                    const d = decls[0], v = "u" + d.id, m = "m" + d.id;
                    const f = first;
                    first = false;
                    return declare(d, v, m, init(d)) + " " + disposeTail(d, nest(decls.slice(1)), v, m, st, f);
                };
                return out + nest(s.decls);
            }
            if (s.t === "forof") {
                const d0 = s.items[0], v = "f" + d0.id, m = "fm" + d0.id, it = "fi" + d0.id, sn = "st" + (++blk);
                out += "for (const " + it + " of [" + s.items.map(init).join(",") + "]) { " +
                    (s.aw ? "const " + sn + " = { na: false, ha: false }; " : "") +
                    declare(d0, v, m, it) + " " + disposeTail(d0, emitModel(s.body), v, m, s.aw ? sn : undefined, true) + " } ";
                continue;
            }
            out += common(s, (b) => emitModel(b)) + " ";
        }
        return out;
    }
    const body = stmts(0, false);
    tmp = 0;
    const u = emitUsing(body);
    tmp = 0;
    const m = emitModel(body);
    return { u, m };
}

export function runSync(src) {
    const L = [];
    const R = (id, mode) => res(L, id, mode);
    const f = new Function("L", "R", "ser", src);
    try { f(L, R, ser); L.push("done"); } catch (e) { L.push("E:" + ser(e)); }
    return L.join(" ");
}

const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor;

export async function runAsync(src) {
    const L = [];
    const R = (id, mode) => res(L, id, mode);
    const A = (id, mode) => ares(L, id, mode);
    const f = new AsyncFunction("L", "R", "A", "ser", src);
    const ticker = (async () => { for (let k = 0; k < 40; k++) { L.push("t" + k); await null; } })();
    try { await f(L, R, A, ser); L.push("done"); } catch (e) { L.push("E:" + ser(e)); }
    await ticker;
    return L.join(" ");
}
