// tools/peer-proof.js TEST [--base REV | --base-file FILE] [--out DIR] -- the NO-PYTHON conversion proof for one suite. Runs the suite as of
// REV (default HEAD: the Python-peer version) with a python3 shim on PATH that puts tests/lib/tcprec.js in front of every
// peer (server: a port-file argument; client: the port files in a directory argument or a numeric port argument), then runs
// the working-tree suite with RAWPEER_REC so its rawpeer.js peers record themselves. Prints, per peer name (script
// basename), whether the sorted per-connection transcripts are byte-identical (Date, Server and Sec-WebSocket-Key/Accept header values and
// loopback ports normalized: they differ between any two runs or implementations; a request the counterpart pipelined after the peer's full reply, which the
// proxy sees but a closing peer never reads, is tolerated and counted), and the diff of the two pass lists.
// Per-suite options, in the suite's first 30 lines and echoed in the output: "// peer-proof: VERB PEER|* [ARG] -- reason" with VERB
// drop (leave out connections whose request bytes match the regex ARG; out:ARG matches the peer's own sent bytes), join (compare all connections as one stream in
// start order: connection reuse is the client's race), prefix (tolerate a send one side cut short), exempt (report, never fail),
// ports FILE (the port-file argument named FILE holds several ports on one line: one recording proxy each, same order, each
// rewriting the peer's own ports to the proxies' in what it sends, so a redirect to a sibling port stays recorded).
// Exit 0 when every transcript and the pass list match, 1 otherwise. Writes only under DIR (default .obj/peer-proof/<suite>).
import * as file from "dyna:file";
import * as sys from "dyna:sys";

const argv = scriptArgs.slice(1);
const test = argv.find((a) => !a.startsWith("--") && !["--base", "--base-file", "--out"].includes(argv[argv.indexOf(a) - 1]));
const opt = (k, d) => { const i = argv.indexOf(k); return i >= 0 ? argv[i + 1] : d; };
if (!test) { print("usage: peer-proof.js TEST [--base REV | --base-file FILE] [--out DIR]"); throw new Error("peer-proof: no TEST"); }
const base = opt("--base", "HEAD"), baseFile = opt("--base-file", "");
const MULTI = file.readFile(test).split("\n").slice(0, 30).map((l) => /^\/\/ peer-proof: ports (\S+) -- /.exec(l)).filter(Boolean).map((m) => m[1]);
const name = test.replace(/^.*\//, "").replace(/\.js$/, "");
const root = String(sys.Exec("/bin/pwd", []).stdout).trim();
const out = root + "/" + opt("--out", ".obj/peer-proof/" + name);
const sh = (c, env) => sys.Exec("/bin/bash", ["-c", c], env ? { env: Object.assign(sys.env(), env) } : undefined);
const q = (s) => "'" + String(s).replace(/'/g, "'\\''") + "'";
const t0 = Date.now();
sh(`rm -rf ${q(out)} && mkdir -p ${q(out)}/shim ${q(out)}/old-rec ${q(out)}/new-rec`);
const realPy = String(sh("command -v python3").stdout).trim();
const old = test.replace(/[^/]*$/, ".pp_old_" + name + ".js");
const show = baseFile ? sh(`cp ${q(baseFile)} ${q(old)}`) : sh(`git show ${q(base + ":" + test)} > ${q(old)}`);
if (show.code !== 0) { sh(`rm -f ${q(old)}`); throw new Error("peer-proof: " + (baseFile ? "cp " + baseFile : "git show " + base + ":" + test) + " failed"); }
if (baseFile) print("base: " + baseFile + " (one-time proof variant)");
file.writeFile(out + "/shim/python3", `#!/usr/bin/env bash
REAL=${q(realPy)}; REC=${q(out + "/old-rec")}; ROOT=${q(root)}; MULTI=${q(MULTI.join(" "))}
py=peer; for a in "$@"; do case "$a" in *.py) py=\${a##*/}; py=\${py%.py}; break;; esac; done
args=(); ups=(); pids=(); restore=()
rec() { "$ROOT/dynajs" --std "$ROOT/tests/lib/tcprec.js" "$1" "$2" "$REC/$py.rec" $3 $4 & pids+=($!); }
for a in "$@"; do
  b=\${a##*/}
  if [[ "$a" == */* && "$b" == *port* && "$a" != *.py && ! -e "$a" ]]; then args+=("$a.up"); ups+=("$a")
  elif [[ "$a" =~ ^[0-9]+$ && "$a" -ge 1024 && "$a" -le 65535 ]]; then
    d=$(mktemp -d "$REC/n.XXXX"); echo "$a" > "$d/up"; rec "$d/up" "$d/own" client
    for i in $(seq 400); do [ -s "$d/own" ] && break; sleep 0.025; done; args+=("$(cat "$d/own")")
  elif [[ -d "$a" ]]; then
    for f in "$a"/*port*; do
      [[ -f "$f" && "$f" != *.up && "$f" != *.own ]] || continue; grep -qE '^[0-9]+$' "$f" || continue
      cp "$f" "$f.up"; rec "$f.up" "$f.own" client
      for i in $(seq 400); do [ -s "$f.own" ] && break; sleep 0.025; done
      cp "$f.own" "$f"; restore+=("$f"); done
    args+=("$a")
  else args+=("$a"); fi
done
multi() { local u=$1 k=0 j i p prev="" own=() map=()
  for i in $(seq 800); do [ -s "$u.up" ] && [ "$(cat "$u.up")" = "$prev" ] && break; prev=$(cat "$u.up" 2>/dev/null); sleep 0.025; done
  for p in $(cat "$u.up"); do echo "$p" > "$u.m$k"; rec "$u.m$k" "$u.o$k" server "$u.map"; k=$((k+1)); done
  for ((j=0;j<k;j++)); do for i in $(seq 400); do [ -s "$u.o$j" ] && break; sleep 0.025; done; own+=("$(tr -d '\\n' < "$u.o$j")"); map+=("$(cat "$u.m$j")=\${own[$j]}"); done
  echo -n "\${map[*]}" > "$u.mapt" && mv "$u.mapt" "$u.map"
  echo -n "\${own[*]}" > "$u.mt" && mv "$u.mt" "$u"; }
mups=(); for u in "\${ups[@]}"; do if [[ " $MULTI " == *" \${u##*/} "* ]]; then mups+=("$u"); else rec "$u.up" "$u" ""; fi; done
trap 'kill "\${pids[@]}" $c 2>/dev/null; exit 143' TERM INT HUP
"$REAL" "\${args[@]}" & c=$!
for u in "\${mups[@]}"; do multi "$u"; done
wait $c; rc=$?
for f in "\${restore[@]}"; do cp "$f.up" "$f"; done
sleep 0.3; kill "\${pids[@]}" 2>/dev/null; exit $rc
`);
sh(`chmod +x ${q(out)}/shim/python3`);
const run = (file_, env, tag) => {
    const s = Date.now();
    const r = sh(`tools/bounded-run.sh 900 900 ${tag} -- ./dynajs --std ${q(file_)} > ${q(out + "/" + tag + ".out")} 2>&1; echo $?`, env);
    return { rc: parseInt(String(r.stdout).trim(), 10), secs: (Date.now() - s) / 1000 };
};
const ro = run(old, { PATH: out + "/shim:" + sys.env().PATH }, "old");
sh(`rm -f ${q(old)}`);
const rn = run(test, { RAWPEER_REC: out + "/new-rec" }, "new");
const OPTS = [];
for (const l of file.readFile(test).split("\n").slice(0, 30)) { const m = /^\/\/ peer-proof: (exempt|prefix|join|drop|ports) (\S+)(?: (.*?))? -- (.+)$/.exec(l); if (m) OPTS.push({ verb: m[1], peer: m[2], arg: m[3] || "", why: m[4] }); }
const optFor = (peer, verb) => OPTS.filter((o) => o.verb === verb && (o.peer === peer || o.peer === "*"));
for (const o of OPTS) print("option " + o.verb + " " + o.peer + (o.arg ? " " + JSON.stringify(o.arg) : "") + " -- " + o.why);
const recs = (d) => { try { return file.readDir(d).map((e) => e.name).filter((n) => n.endsWith(".rec")); } catch { return []; } };
const unhex = (h) => h.replace(/../g, (x) => String.fromCharCode(parseInt(x, 16)));
const tohex = (s) => s.replace(/[\s\S]/g, (c) => (c.charCodeAt(0) < 16 ? "0" : "") + c.charCodeAt(0).toString(16));
const norm = (s) => s.replace(/\r\nDate: [^\r]*/g, "\r\nDate: #").replace(/\r\nServer: [^\r]*/g, "\r\nServer: #").replace(/\r\nSec-WebSocket-(Key|Accept): [^\r]*/g, "\r\nSec-WebSocket-$1: #").replace(/\b(127\.0\.0\.1|localhost|\[::1\]):\d{2,5}\b/g, "$1:#port");
const canon = (p, peer, st) => {
    let L;
    try { L = file.readFile(p).split("\n").filter(Boolean).map((l) => { const o = JSON.parse(l); return { in: unhex(o.in), out: unhex(o.out), t: o.t || 0 }; }); } catch { return []; }
    for (const d of optFor(peer, "drop")) { const o = d.arg.startsWith("out:"), re = new RegExp(o ? d.arg.slice(4) : d.arg); const n = L.length; L = L.filter((c) => !re.test(o ? c.out : c.in)); st.dropped += n - L.length; }
    if (optFor(peer, "join").length && L.length > 1) { L.sort((x, y) => x.t - y.t); st.joined += L.length; L = [{ in: L.map((c) => c.in).join(""), out: L.map((c) => c.out).join("") }]; }
    return L.map((c) => JSON.stringify({ in: tohex(norm(c.in)), out: tohex(norm(c.out)) })).sort();
};
let bad = 0;
const names = [...new Set([...recs(out + "/old-rec"), ...recs(out + "/new-rec")])].sort();
for (const n of names) {
    const peer = n.replace(/\.rec$/, ""), sa = { dropped: 0, joined: 0 }, sb = { dropped: 0, joined: 0 };
    const a = canon(out + "/old-rec/" + n, peer, sa), b = canon(out + "/new-rec/" + n, peer, sb);
    let tol = 0, cut = 0;
    const prefix = optFor(peer, "prefix").length > 0;
    const late = (x, y) => {
        if (x === y) return true;
        const o = JSON.parse(x), m = JSON.parse(y), pi = unhex(o.in), ji = unhex(m.in);
        if (prefix && o.in === m.in && o.out !== m.out && (o.out.startsWith(m.out) || m.out.startsWith(o.out))) { cut++; return true; }
        if (o.out !== m.out || !pi.startsWith(ji) || !/\r\n\r\n$/.test(ji) || !/^[A-Z]+ \S+ HTTP\/1\.[01]\r\n/.test(pi.slice(ji.length))) return false;
        tol++;
        return true;
    };
    const same = a.length === b.length && a.every((l, i) => late(l, b[i]));
    const exempt = optFor(peer, "exempt");
    if (!same && !exempt.length) bad++;
    let why = "";
    if (!same) {
        const i = a.findIndex((l, k) => l !== b[k]);
        const show1 = (l) => { if (!l) return "(none)"; const o = JSON.parse(l); const d = (h) => h.replace(/../g, (x) => { const c = parseInt(x, 16); return c >= 32 && c < 127 ? String.fromCharCode(c) : "\\x" + x; }); return "in=" + d(o.in).slice(0, 90) + " out=" + d(o.out).slice(0, 90); };
        why = " first diff at conn " + i + "\n      py: " + show1(a[i]) + "\n      js: " + show1(b[i]);
    }
    print("transcript " + n.replace(/\.rec$/, "") + ": " + (same ? "identical" : "DIFFERS") + " (" + a.length + " py / " + b.length + " js connections" + (tol ? "; " + tol + " where the proxy also saw a keep-alive request pipelined after the peer's complete reply, which the peer never read" : "") + ")" + (sa.dropped || sb.dropped ? "; dropped " + sa.dropped + " py / " + sb.dropped + " js connections" : "") + (sa.joined || sb.joined ? "; joined " + sa.joined + " py / " + sb.joined + " js connections into one stream" : "") + (cut ? "; " + cut + " where one side's send was cut short" : "") + (!same && exempt.length ? "; EXEMPT: " + exempt[0].why : "") + why);
}
if (!names.length) print("transcript: none recorded (no peer on either side)");
const passList = (p) => { try { return file.readFile(p).split("\n").filter((l) => /\b(PASS|FAIL|SKIP|ok|passed|failed|skipped)\b/i.test(l)).map((l) => l.replace(/\d+(\.\d+)?\s?(ms|s)\b/g, "#t").replace(/\/[^\s'"]*\/(dj|dyn|tmp)[^\s'"]*/g, "#path").replace(/\b\d{4,5}\b/g, "#n").trim()); } catch { return []; } };
const pa = passList(out + "/old.out"), pb = passList(out + "/new.out");
const onlyA = pa.filter((l) => !pb.includes(l)), onlyB = pb.filter((l) => !pa.includes(l));
print("pass list: " + (onlyA.length || onlyB.length ? "DIFFERS" : "identical") + " (" + pa.length + " py / " + pb.length + " js lines; rc " + ro.rc + " / " + rn.rc + "; " + ro.secs + " s / " + rn.secs + " s)");
for (const l of onlyA.slice(0, 12)) print("  - " + l);
for (const l of onlyB.slice(0, 12)) print("  + " + l);
const exemptAll = OPTS.find((o) => o.verb === "exempt" && o.peer === "*");
if ((onlyA.length || onlyB.length || ro.rc !== rn.rc) && exemptAll) print("pass list: EXEMPT: " + exemptAll.why);
else if (onlyA.length || onlyB.length || ro.rc !== rn.rc) bad++;
print("peer-proof " + name + ": " + (bad ? "MISMATCH" : "MATCH") + (OPTS.length ? " with " + OPTS.length + " option(s)" : "") + " in " + Math.round((Date.now() - t0) / 100) / 10 + " s; logs " + out);
if (bad) throw new Error("peer-proof: " + bad + " mismatch(es)");
