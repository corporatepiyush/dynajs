// Shape queries: each reads the raw text of function bodies. Regexes are the
// reference tool's RE2 patterns with \s spelled [\t\n\f\r ] (RE2's set).
import * as fs from "dyna:file";
import { readBytesString } from "./pp.js";
import { trimSpace, runeAt, runeWidth, isUniSpace } from "./text.js";
import { likeMatch } from "./util.js";
import { scanRoot } from "./config.js";
const scanRootArg = () => scanRoot(scriptArgs.slice(1));

const S = "[\\t\\n\\f\\r ]";
const re = (src, flags = "") => new RegExp(src.replaceAll("\\s", S), flags);
const reG = (src, flags = "") => re(src, flags + "g");
const quoteMeta = (s) => s.replace(/[\\.+*?()|[\]{}^$]/g, "\\$&");

const API_CALL = reG(String.raw`\b(JS_[A-Za-z0-9_]*|js_[a-z0-9_]+)\s*\(`);

export function dynBodyEnd(src, start) {
  let i = start, depth = 0, open = -1;
  const n = src.length;
  while (i < n) {
    const c = src.charCodeAt(i);
    if (c === 47 && i + 1 < n && src.charCodeAt(i + 1) === 47) {
      const j = src.indexOf("\n", i);
      i = j < 0 ? n : j;
      continue;
    }
    if (c === 47 && i + 1 < n && src.charCodeAt(i + 1) === 42) {
      const j = src.indexOf("*/", i + 2);
      i = j < 0 ? Math.max(i + 2, n - 1) + 2 : j + 2;
      continue;
    }
    if (c === 34 || c === 39) {
      i++;
      while (i < n && src.charCodeAt(i) !== c) {
        if (src.charCodeAt(i) === 92) i++;
        if (i < n && src.charCodeAt(i) === 10 && c === 39) break;
        i++;
      }
      i++;
      continue;
    }
    if (c === 59 && depth === 0 && open < 0) return [-1, i + 1];
    if (c === 123) { if (depth === 0 && open < 0) open = i; depth++; }
    else if (c === 125) { depth--; if (depth === 0 && open >= 0) return [open, i + 1]; }
    i++;
  }
  return [open, n];
}

let apiCache = null;
export function dynAPI(g) {
  if (apiCache) return apiCache;
  const t = { bySym: new Map(), body: new Map(), bodyLine: new Map(), files: 0, names: 0, miss: 0 };
  const byFile = new Map();
  for (const s of g.symbols) {
    if (s.Kind !== "function" || s.LineStart <= 0) continue;
    let a = byFile.get(s.FileID);
    if (!a) byFile.set(s.FileID, (a = []));
    a.push(s);
  }
  for (const f of g.files) {
    const fns = byFile.get(f.id);
    if (!fns || !f.parsed || f.isTest) continue;
    let src;
    try { src = readBytesString(g.abs + "/" + f.path); } catch (e) { t.miss++; continue; }
    t.files++;
    const lineOff = [0];
    for (let j = src.indexOf("\n"); j >= 0; j = src.indexOf("\n", j + 1)) lineOff.push(j + 1);
    for (const s of fns) {
      const ls = s.LineStart;
      if (ls < 1 || ls > lineOff.length) continue;
      const [open, end] = dynBodyEnd(src, lineOff[ls - 1]);
      if (open < 0 || end > src.length) continue;
      const body = src.slice(open, end);
      t.body.set(s.ID, body);
      t.bodyLine.set(s.ID, ls + countNL(src, lineOff[ls - 1], open));
      const counts = new Map();
      API_CALL.lastIndex = 0;
      for (const m of body.matchAll(API_CALL)) {
        if (m[1].startsWith("JS_VALUE_GET_")) continue;
        counts.set(m[1], (counts.get(m[1]) || 0) + 1);
      }
      if (counts.size) { t.bySym.set(s.ID, counts); t.names += counts.size; }
    }
  }
  return (apiCache = t);
}

function countNL(s, a, b) {
  let n = 0;
  for (let j = s.indexOf("\n", a); j >= 0 && j < b; j = s.indexOf("\n", j + 1)) n++;
  return n;
}

export function dynArgs(body, open) {
  const args = [];
  let depth = 0, start = open + 1;
  for (let i = open; i < body.length; i++) {
    const c = body[i];
    if (c === '"' || c === "'") {
      i++;
      while (i < body.length && body[i] !== c) { if (body[i] === "\\") i++; i++; }
    } else if (c === "(" || c === "[" || c === "{") depth++;
    else if (c === ")" || c === "]" || c === "}") {
      depth--;
      if (depth === 0) {
        const a = trimSpace(body.slice(start, i));
        if (a !== "" || args.length > 0) args.push(a);
        return [args, i + 1];
      }
    } else if (c === "," && depth === 1) {
      args.push(trimSpace(body.slice(start, i)));
      start = i + 1;
    }
  }
  return [args, body.length];
}

// strings.Fields joined by one space, then cut to n bytes
function fieldsJoin(s) {
  const out = [];
  let i = 0, cur = -1;
  while (i < s.length) {
    const w = runeWidth(s, i);
    if (isUniSpace(runeAt(s, i))) { if (cur >= 0) { out.push(s.slice(cur, i)); cur = -1; } }
    else if (cur < 0) cur = i;
    i += w;
  }
  if (cur >= 0) out.push(s.slice(cur));
  return out.join(" ");
}
const dynClip = (s, n) => { s = fieldsJoin(s); return s.length > n ? s.slice(0, n) : s; };

const I = (n) => ({ i: n });
const Str = (s) => ({ s });
const B = (b) => ({ i: b ? 1 : 0 });

const fileOf = (g, s) => g.files[s.FileID - 1];
const at = (g, s) => { const f = fileOf(g, s); return f ? f.path + ":" + s.LineStart : ""; };
function dynSite(g, t, s, off) {
  const f = fileOf(g, s);
  if (!f) return at(g, s);
  const b = t.body.get(s.ID);
  if (off > b.length) off = b.length;
  return f.path + ":" + (t.bodyLine.get(s.ID) + countNL(b, 0, off));
}
const modName = (g, id) => (id <= 0 || id > g.modules.length ? "" : g.modules[id - 1].name);
const dynFns = (g, p) => g.symbols.filter((s) => s.Kind === "function" && likeMatch(modName(g, s.ModuleID), p.mod));
const limit = (rows, p) => (p.lim >= 0 && rows.length > p.lim ? rows.slice(0, p.lim) : rows);
const cmpI = (a, b) => (a < b ? -1 : a > b ? 1 : 0);
const desc = (...cols) => (a, b) => { for (const c of cols) { const d = cmpI(b[c].i, a[c].i); if (d) return d; } return 0; };
const indexAll = (rx, s) => { rx.lastIndex = 0; return [...s.matchAll(rx)]; };

let taintCache = null;
function dynTainted(g) {
  if (taintCache) return taintCache;
  const t = new Uint8Array(g.symbols.length + 1);
  const out = new Map();
  for (const { CallerID: a, CalleeID: b } of g.edges) { let l = out.get(a); if (!l) out.set(a, (l = [])); l.push(b); }
  const st = [];
  for (const s of g.symbols) if (s.Kind === "function" && s.NIO > 0) { t[s.ID] = 1; st.push(s.ID); }
  while (st.length) {
    const id = st.pop();
    for (const c of out.get(id) || []) if (c >= 1 && c < t.length && !t[c]) { t[c] = 1; st.push(c); }
  }
  return (taintCache = t);
}

const RE_JSCALL_ASSIGN = reG(String.raw`\b(\w+)\s*=\s*JS_(?:Call|CallConstructor|Eval|EvalThis|Invoke)\s*\(`);
const RE_JSCALL_FREED = reG(String.raw`JS_FreeValue(?:RT)?\s*\([^;()]*,\s*JS_(?:Call|CallConstructor|Eval|EvalThis|Invoke)\s*\(`);
const RE_EXC_HANDLED = re(String.raw`\b(?:JS_GetException|JS_HasException|dyn_net_handler_result|dyn_net_dns_callback_result|dyn_call_drop|js_std_dump_error|dyn_\w*(?:swallow|unhandled|report_exc)\w*)\s*\(`);
const RE_RESOLVER = /resolve|reject|resolving_funcs|settle/;

function pendingExceptionDropped(g, p) {
  const cols = ["name", "dropped", "resolver_only", "first_callee", "site", "at"];
  const rows = [];
  const t = dynAPI(g);
  for (const s of dynFns(g, p)) {
    const body = t.body.get(s.ID);
    if (!body || RE_EXC_HANDLED.test(body)) continue;
    let dropped = 0, resolvers = 0, first = "", firstOff = -1;
    const note = (open, off) => {
      const [args] = dynArgs(body, open);
      const callee = args.length > 1 ? args[1] : "";
      dropped++;
      if (RE_RESOLVER.test(callee)) resolvers++;
      if (firstOff < 0) { first = callee; firstOff = off; }
    };
    for (const m of indexAll(RE_JSCALL_ASSIGN, body)) {
      const v = quoteMeta(m[1]);
      const end = m.index + m[0].length;
      const rest = body.slice(end);
      const checked = re(String.raw`JS_IsException\s*\(\s*` + v + String.raw`\s*\)|\breturn\s+` + v + String.raw`\s*;|=\s*` + v + String.raw`\s*;`).test(rest);
      const freed = re(String.raw`JS_FreeValue(?:RT)?\s*\([^;()]*,\s*` + v + String.raw`\s*\)`).test(rest);
      if (freed && !checked) note(end - 1, m.index);
    }
    for (const m of indexAll(RE_JSCALL_FREED, body)) note(m.index + m[0].length - 1, m.index);
    if (dropped === 0) continue;
    rows.push([Str(s.Name), I(dropped), B(resolvers === dropped), Str(dynClip(first, 40)), Str(dynSite(g, t, s, firstOff)), Str(at(g, s))]);
  }
  rows.sort((a, b) => cmpI(a[2].i, b[2].i) || cmpI(b[1].i, a[1].i));
  return [cols, limit(rows, p)];
}

const RE_ROUND_CALL = reG(String.raw`-\s*(?:floor|trunc|rint|round|nearbyint|ceil)[fl]?\s*\(`);
const RE_FMA = re(String.raw`\bfmal?f?\s*\(`);

function fmaDependent(g, p) {
  const cols = ["name", "sites", "has_fma_call", "expr", "site", "at"];
  const rows = [];
  const t = dynAPI(g);
  for (const s of dynFns(g, p)) {
    const body = t.body.get(s.ID) ?? "";
    let n = 0, first = -1, expr = "";
    for (const m of indexAll(RE_ROUND_CALL, body)) {
      const [, end] = dynArgs(body, m.index + m[0].length - 1);
      let k = end;
      while (k < body.length && (body[k] === " " || body[k] === "\t" || body[k] === "\n")) k++;
      if (k >= body.length || body[k] !== "*") continue;
      if (n === 0) {
        first = m.index;
        const lo = Math.max(0, m.index - 24), hi = Math.min(body.length, k + 12);
        expr = dynClip(body.slice(lo, hi), 60);
      }
      n++;
    }
    if (n === 0) continue;
    rows.push([Str(s.Name), I(n), B(RE_FMA.test(body)), Str(expr), Str(dynSite(g, t, s, first)), Str(at(g, s))]);
  }
  rows.sort((a, b) => cmpI(a[2].i, b[2].i) || cmpI(b[1].i, a[1].i));
  return [cols, limit(rows, p)];
}

const GREP_COLS = ["name", "calls", "first", "site", "at"];
function grepRows(g, p, rx, unless, keep) {
  const rows = [];
  const t = dynAPI(g);
  for (const s of dynFns(g, p)) {
    const body = t.body.get(s.ID);
    if (!body || (keep && !keep(s))) continue;
    const ms = indexAll(rx, body);
    if (!ms.length || (unless && unless.test(body))) continue;
    rows.push([Str(s.Name), I(ms.length), Str(dynClip(ms[0][0], 44)), Str(dynSite(g, t, s, ms[0].index)), Str(at(g, s))]);
  }
  rows.sort(desc(1));
  return rows;
}

const RE_RING_INIT = reG(String.raw`\bio_uring_queue_init(?:_params)?\s*\(`);
const rawRingOpen = (g, p) => [GREP_COLS, limit(grepRows(g, p, RE_RING_INIT, null, (s) => s.Name !== "dyn_uring_ring_open"), p)];

const RE_REACTOR_ACQ = reG(String.raw`\bdyn_net_reactor_acquire\s*\(`);
const RE_SHARED_NO_REACT = re(String.raw`\bdyn_net_throw_no_reactor\s*\(`);
const reactorErrorShape = (g, p) => [GREP_COLS, limit(grepRows(g, p, RE_REACTOR_ACQ, RE_SHARED_NO_REACT,
  (s) => s.Name !== "dyn_net_reactor_acquire" && !s.Name.startsWith("dyn_net_reactor_")), p)];

const RE_SET_STR = reG(String.raw`\bJS_SetPropertyStr\s*\(`);
const RE_SET_ATOM = reG(String.raw`\bJS_SetProperty\s*\(`);

function setPropertyDynamicKey(g, p) {
  const cols = ["name", "dynamic_keys", "literal_keys", "on_frontier", "first_key", "site", "at"];
  const taint = dynTainted(g);
  const rows = [];
  const t = dynAPI(g);
  for (const s of dynFns(g, p)) {
    const body = t.body.get(s.ID) ?? "";
    let dyn = 0, lit = 0, first = "", off = -1;
    const scan = (rx, atom) => {
      for (const m of indexAll(rx, body)) {
        const [args] = dynArgs(body, m.index + m[0].length - 1);
        if (args.length < 3) continue;
        const key = args[2];
        if ((!atom && key.startsWith('"')) || (atom && key.startsWith("JS_ATOM_"))) { lit++; continue; }
        dyn++;
        if (off < 0) { first = key; off = m.index; }
      }
    };
    scan(RE_SET_STR, false);
    scan(RE_SET_ATOM, true);
    if (dyn === 0) continue;
    rows.push([Str(s.Name), I(dyn), I(lit), B(taint[s.ID]), Str(dynClip(first, 32)), Str(dynSite(g, t, s, off)), Str(at(g, s))]);
  }
  rows.sort(desc(3, 1));
  return [cols, limit(rows, p)];
}

const RE_NEW_TYPED = reG(String.raw`\bJS_NewTypedArray\s*\(`);
const RE_BUFFER_NAME = /buf|buffer|^ab$|_ab$|arraybuffer/i;

function typedArrayShortArgs(g, p) {
  const cols = ["name", "argc", "argv", "site", "at"];
  const rows = [];
  const t = dynAPI(g);
  for (const s of dynFns(g, p)) {
    const body = t.body.get(s.ID) ?? "";
    for (const m of indexAll(RE_NEW_TYPED, body)) {
      const [args] = dynArgs(body, m.index + m[0].length - 1);
      if (args.length < 3 || (args[1] !== "1" && args[1] !== "2")) continue;
      let v = args[2].replace(/^[&(]+/, "");
      if (v.startsWith("JSValueConst*)")) v = v.slice(14);
      v = trimSpace(v);
      const nm = /^\w+/.exec(v);
      if (!nm) continue;
      const name = nm[0];
      const fromBuffer = re(String.raw`\b` + quoteMeta(name) + String.raw`(?:\s*\[\s*0\s*\])?\s*=\s*JS_NewArrayBuffer\w*\s*\(`).test(body);
      if (!fromBuffer && !RE_BUFFER_NAME.test(name)) continue;
      rows.push([Str(s.Name), Str(args[1]), Str(dynClip(args[2], 30)), Str(dynSite(g, t, s, m.index)), Str(at(g, s))]);
    }
  }
  return [cols, limit(rows, p)];
}

const RE_REACTOR_FILE = /^dyna-(?:aio|aio-uring|evloop|http|net|net-[a-z0-9]+|tls|scrape)\.(?:c|inc\.c)$/;
const RE_BLOCKING = reG(String.raw`\b(?:sleep|usleep|nanosleep|getaddrinfo|gethostbyname2?|system|popen)\s*\(|\bpoll\s*\(\s*NULL\b|\bwaitpid\s*\([^;]*,\s*0\s*\)`);
const baseName = (p) => p.slice(p.lastIndexOf("/") + 1);
const reactorBlockingCall = (g, p) => [GREP_COLS, limit(grepRows(g, p, RE_BLOCKING, null, (s) => {
  const f = fileOf(g, s);
  return !!f && RE_REACTOR_FILE.test(baseName(f.path));
}), p)];

const RE_FSYNC = reG(String.raw`\bf(?:data)?sync\s*\(`);
const RE_FULLFSYNC = /\bF_FULLFSYNC\b/;
const fsyncNotDurable = (g, p) => [GREP_COLS, limit(grepRows(g, p, RE_FSYNC, RE_FULLFSYNC, null), p)];

const RE_LOCALE_NUM = reG(String.raw`\b(?:strtod|strtof|strtold|atof)\s*\(|\bv?f?s?scanf\s*\([^;]*%[0-9*]*l?L?[fFeEgGaA]`);
function localeNumberParse(g, p) {
  const cols = ["name", "calls", "on_frontier", "first", "site", "at"];
  const taint = dynTainted(g);
  const rows = [];
  const t = dynAPI(g);
  for (const s of dynFns(g, p)) {
    const body = t.body.get(s.ID) ?? "";
    const ms = indexAll(RE_LOCALE_NUM, body);
    if (!ms.length) continue;
    rows.push([Str(s.Name), I(ms.length), B(taint[s.ID]), Str(dynClip(ms[0][0], 30)), Str(dynSite(g, t, s, ms[0].index)), Str(at(g, s))]);
  }
  rows.sort(desc(2, 1));
  return [cols, limit(rows, p)];
}

const RE_DISCARDED_ARM = reG(String.raw`\(\s*void\s*\)\s*(?:dyn_evloop_(?:add|mod|set_timer)|dyn_aio_(?:recv|recvfrom|accept|connect|listen|tls_start|tls_attach|offload\w*)|dyn_timer_add|dyn_pool_submit\w*|io_uring_register\w*|io_uring_submit\w*|pthread_create|epoll_ctl|kevent|timerfd_settime|eventfd)\s*\(`);
const discardedArmResult = (g, p) => [GREP_COLS, limit(grepRows(g, p, RE_DISCARDED_ARM, null, null), p)];

const RE_OFFLOAD = reG(String.raw`\bdyn_fiber_offload\s*\(`);
const RE_CTX_CALL = reG(String.raw`\b([A-Za-z_]\w*)\s*\(\s*(?:ctx|\w+->ctx)\s*[,)]`);
const RE_ONJS = re(String.raw`\bdyn_fiber_onjs\s*\(`);
const RE_ESCAPE = /\b(?:return|goto)\b/;
const RE_NET_MACRO = re(String.raw`^\s*#\s*define\s+\w+\(\.\.\.\)\s*\(dyn_fiber_onjs\(\)`, "m");
const RE_DEFINE_CALL = reG(String.raw`^\s*#\s*define\s+([A-Za-z_]\w*)\(`, "m");
const RE_LOCAL_INC = /^#include "([\w-]+\.h)"/gm;
const OFFLOAD_EXEMPT = new Set(["JS_NewCModule", "JS_AddModuleExport", "JS_AddModuleExportList", "JS_SetModuleExportList",
  "dyn_register_class", "dyn_register_plain_class", "if", "while", "for", "switch", "return", "sizeof"]);

const joinPath = (a, b) => {
  const parts = [];
  for (const seg of (a + "/" + b).split("/")) {
    if (seg === "" || seg === ".") continue;
    if (seg === "..") { if (parts.length && parts[parts.length - 1] !== "..") parts.pop(); else if (!a.startsWith("/")) parts.push(".."); continue; }
    parts.push(seg);
  }
  const r = parts.join("/");
  return a.startsWith("/") ? "/" + r : r || ".";
};
const readOr = (p) => { try { return readBytesString(p); } catch (e) { return null; } };

function offloadUnguarded(g, p) {
  const cols = ["name", "unguarded", "first_callee", "site", "at"];
  const t = dynAPI(g);
  const root = scanRootArg();
  const info = new Map();
  const fns = dynFns(g, p);
  for (const s of fns) {
    let fi = info.get(s.FileID);
    if (!fi) {
      fi = { offloads: false, netted: false, local: new Set(), wrapped: new Set(), base: "", src: null, id: s.FileID };
      info.set(s.FileID, fi);
      const f = fileOf(g, s);
      if (f) {
        const src = readOr(joinPath(root, f.path));
        if (src !== null) {
          for (const m of indexAll(RE_DEFINE_CALL, src)) fi.wrapped.add(m[1]);
          fi.netted = RE_NET_MACRO.test(src);
          fi.base = baseName(f.path);
          fi.src = src;
        }
      }
    }
    fi.local.add(s.Name);
    RE_OFFLOAD.lastIndex = 0;
    if (RE_OFFLOAD.test(t.body.get(s.ID) ?? "")) fi.offloads = true;
  }
  const hdrCache = new Map(), dirOffloads = new Set(), fileDir = new Map();
  for (const [id, fi] of info) {
    if (id >= 1 && id <= g.files.length) {
      fileDir.set(fi, pathDirGo(g.files[id - 1].path));
      if (fi.offloads) dirOffloads.add(fileDir.get(fi));
    }
  }
  // Go map iteration order is random; every step below is order-independent
  // except the union of name sets, which only grows.
  for (const fi of info.values()) {
    if (fi.src === null) continue;
    for (const m of indexAll(RE_LOCAL_INC, fi.src)) {
      const hp = joinPath(joinPath(root, fileDir.get(fi)), m[1]);
      let hs = hdrCache.get(hp);
      if (hs === undefined) { hs = readOr(hp); hdrCache.set(hp, hs); }
      if (hs === null || !RE_NET_MACRO.test(hs)) continue;
      fi.netted = true;
      for (const o of info.values()) if (fileDir.get(o) === fileDir.get(fi)) for (const k of o.local) fi.local.add(k);
      for (const d of indexAll(RE_DEFINE_CALL, hs)) fi.wrapped.add(d[1]);
      if (dirOffloads.has(fileDir.get(fi))) fi.offloads = true;
    }
  }
  for (const fi of info.values()) {
    if (fi.netted || !fi.base.endsWith(".inc.c")) continue;
    const inc = '#include "' + fi.base + '"';
    for (const host of info.values()) {
      if (host.netted && host.src !== null && host.src.includes(inc)) {
        fi.netted = true;
        fi.offloads = fi.offloads || host.offloads;
        for (const k of host.wrapped) fi.wrapped.add(k);
        for (const k of host.local) fi.local.add(k);
      }
    }
  }
  const rows = [];
  for (const s of fns) {
    const fi = info.get(s.FileID);
    const body = t.body.get(s.ID);
    if (!fi || !fi.offloads || !body) continue;
    if (!fi.netted) {
      let n = 0, first = "", firstOff = -1;
      for (const o of indexAll(RE_OFFLOAD, body)) {
        const oEnd = o.index + o[0].length;
        const tail = body.slice(oEnd);
        const e = RE_ONJS.exec(tail);
        let what = "no dyn_fiber_onjs() after the offload";
        if (e) {
          const seg = tail.slice(0, e.index);
          what = "";
          RE_CTX_CALL.lastIndex = 0;
          const m = RE_CTX_CALL.exec(seg);
          if (m) what = m[1];
          else { const x = RE_ESCAPE.exec(seg); if (x) what = x[0] + " inside the offloaded section"; }
        }
        if (what !== "") { n++; if (firstOff < 0) { first = what; firstOff = o.index; } }
      }
      if (n > 0) rows.push([Str(s.Name), I(n), Str(first), Str(dynSite(g, t, s, firstOff)), Str(at(g, s))]);
      continue;
    }
    let n = 0, first = "", firstOff = -1;
    for (const m of indexAll(RE_CTX_CALL, body)) {
      const callee = m[1];
      if (fi.local.has(callee) || fi.wrapped.has(callee) || OFFLOAD_EXEMPT.has(callee) ||
        callee.startsWith("dyn_fiber_") || callee.startsWith("dyn_ml_")) continue;
      const c0 = m.index;
      if (c0 > 0 && (body[c0 - 1] === "." || body[c0 - 1] === ">")) continue;
      n++;
      if (firstOff < 0) { first = callee; firstOff = m.index; }
    }
    if (n > 0) rows.push([Str(s.Name), I(n), Str(first), Str(dynSite(g, t, s, firstOff)), Str(at(g, s))]);
  }
  rows.sort(desc(1));
  return [cols, limit(rows, p)];
}
const pathDirGo = (p) => { const i = p.lastIndexOf("/"); return i < 0 ? "." : p.slice(0, i); };

export const SHAPE_RUNNERS = {
  "offload-unguarded-runtime-call": offloadUnguarded,
  "pending-exception-dropped": pendingExceptionDropped,
  "fma-dependent-exactness": fmaDependent,
  "raw-uring-ring-open": rawRingOpen,
  "reactor-acquire-error-shape": reactorErrorShape,
  "set-property-dynamic-key": setPropertyDynamicKey,
  "typed-array-short-args": typedArrayShortArgs,
  "reactor-blocking-call": reactorBlockingCall,
  "fsync-without-fullfsync": fsyncNotDurable,
  "locale-number-parse": localeNumberParse,
  "discarded-arm-result": discardedArmResult,
};
