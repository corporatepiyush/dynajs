// flags: --std
// timeout: 60
// covers: dyna:http.App
// App.static containment corpus: every raw request target below has a pinned outcome (status +
// body digest) recorded from the per-component O_NOFOLLOW walk + realpath fallback that shipped
// which is again the only open strategy (the one-call openat2 path was removed). Any
// open strategy must reproduce the table byte for byte: `..`, %2e%2e, encoded slashes,
// backslashes, NUL, absolute paths, dotfiles, symlinks inside the root pointing inside (served)
// and outside (refused), relative and absolute link targets, links through directories, and a
// root that is itself a symlink. Targets are sent raw over TCP, so nothing normalises them first.
// RECORD=1 prints the table instead of checking it.
import * as std from "std";
import { TCPServer } from "dyna:net";
import { App } from "dyna:http";
import { makeTempDir, writeFile, makeDir, symlink, removeAll, realPath } from "dyna:file";
import { suite } from "../lib/kit.js";

let base = null, root = null, outside = null, app = null;
function build() {
base = realPath(makeTempDir("stdiff"));
root = base.join("root"); outside = base.join("outside");
makeDir(root); makeDir(outside); makeDir(root.join("sub")); makeDir(root.join("sub").join("deeper"));
writeFile(root.join("f.txt"), "inside-file");
writeFile(root.join("index.html"), "index-page");
writeFile(root.join(".hidden"), "dot-file");
writeFile(root.join("sub").join("deep.txt"), "deep-file");
writeFile(root.join("sub").join("deeper").join("x.txt"), "deeper-file");
writeFile(root.join("a b.txt"), "space-file");
writeFile(outside.join("secret.txt"), "TOP-SECRET");
symlink(String(outside.join("secret.txt")), root.join("abs_out.txt"));
symlink("../outside/secret.txt", root.join("rel_out.txt"));
symlink(String(outside), root.join("abs_outdir"));
symlink("../outside", root.join("rel_outdir"));
symlink(String(root.join("f.txt")), root.join("abs_in.txt"));
symlink("f.txt", root.join("rel_in.txt"));
symlink("sub", root.join("subalias"));
symlink(String(root.join("sub")), root.join("abs_subalias"));
symlink("../f.txt", root.join("sub").join("up_in.txt"));
symlink("../../outside/secret.txt", root.join("sub").join("up_out.txt"));
symlink("nowhere.txt", root.join("dangling.txt"));
symlink("loop2", root.join("loop1"));
symlink("loop1", root.join("loop2"));
symlink(String(root), base.join("rootlink"));

app = new App({ port: 0, compress: false });
app.static("/s", root, {});
app.static("/l", base.join("rootlink"), {});
app.start();
}

const targets = () => [
  "/s/f.txt", "/s/", "/s", "/s/index.html", "/s/sub/deep.txt", "/s/sub/deeper/x.txt", "/s/sub/", "/s/a%20b.txt",
  "/s/nope.txt", "/s/sub/nope.txt", "/s/.hidden", "/s/sub/../f.txt", "/s/../outside/secret.txt", "/s/..",
  "/s/%2e%2e/outside/secret.txt", "/s/%2E%2E/outside/secret.txt", "/s/.%2e/outside/secret.txt", "/s/%2e./outside/secret.txt",
  "/s/sub%2fdeep.txt", "/s/sub%2Fdeep.txt", "/s/..%2foutside%2fsecret.txt", "/s/%2e%2e%2foutside%2fsecret.txt",
  "/s/sub\\deep.txt", "/s/..\\outside\\secret.txt", "/s/%5c..%5coutside%5csecret.txt",
  "/s/f.txt%00.png", "/s/%00", "/s//etc/passwd", "/s/" + String(outside.join("secret.txt")), "/s//" + String(root.join("f.txt")),
  "/s/abs_out.txt", "/s/rel_out.txt", "/s/abs_outdir/secret.txt", "/s/rel_outdir/secret.txt",
  "/s/abs_in.txt", "/s/rel_in.txt", "/s/subalias/deep.txt", "/s/abs_subalias/deep.txt", "/s/subalias/deeper/x.txt",
  "/s/sub/up_in.txt", "/s/sub/up_out.txt", "/s/dangling.txt", "/s/loop1", "/s/abs_outdir", "/s/subalias",
  "/l/f.txt", "/l/rel_in.txt", "/l/abs_in.txt", "/l/rel_out.txt", "/l/abs_out.txt", "/l/subalias/deep.txt",
];

function fnv(s) { let h = 2166136261; for (let i = 0; i < s.length; i++) h = Math.imul(h ^ s.charCodeAt(i), 16777619) >>> 0; return h.toString(16); }

function ask(target) {
  return new Promise((resolve) => {
    let buf = "";
    const done = () => {
      const m = /^HTTP\/1\.1 (\d{3})/.exec(buf);
      const i = buf.indexOf("\r\n\r\n");
      const body = i >= 0 ? buf.slice(i + 4) : "";
      resolve(m ? m[1] + ":" + body.length + ":" + fnv(body) : "noreply:" + JSON.stringify(buf.slice(0, 40)));
    };
    TCPServer.connect({ host: "127.0.0.1", port: app.port }, {
      connect: (c) => { if (c) c.write("GET " + target + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"); else resolve("noconnect"); },
      data: (c, b) => { buf += String.fromCharCode(...b); },
      close: () => done(),
    });
  });
}

async function askAll(list) {
  const got = {};
  for (const t of list) got[t.replace(String(base), "$BASE")] = await ask(t);
  return got;
}
function teardown() { if (app) app.close(); if (base) removeAll(base); app = null; base = null; }

const want = {
  "/s/f.txt": "200:11:b6c774d6",
  "/s/": "200:10:4598a659",
  "/s": "200:10:4598a659",
  "/s/index.html": "200:10:4598a659",
  "/s/sub/deep.txt": "200:9:ab7eb780",
  "/s/sub/deeper/x.txt": "200:11:3dbefdc5",
  "/s/sub/": "404:21:e43aa670",
  "/s/a%20b.txt": "404:21:e43aa670",
  "/s/nope.txt": "404:21:e43aa670",
  "/s/sub/nope.txt": "404:21:e43aa670",
  "/s/.hidden": "404:21:e43aa670",
  "/s/sub/../f.txt": "403:21:3f06beec",
  "/s/../outside/secret.txt": "403:21:3f06beec",
  "/s/..": "403:21:3f06beec",
  "/s/%2e%2e/outside/secret.txt": "404:21:e43aa670",
  "/s/%2E%2E/outside/secret.txt": "404:21:e43aa670",
  "/s/.%2e/outside/secret.txt": "404:21:e43aa670",
  "/s/%2e./outside/secret.txt": "404:21:e43aa670",
  "/s/sub%2fdeep.txt": "404:21:e43aa670",
  "/s/sub%2Fdeep.txt": "404:21:e43aa670",
  "/s/..%2foutside%2fsecret.txt": "404:21:e43aa670",
  "/s/%2e%2e%2foutside%2fsecret.txt": "404:21:e43aa670",
  "/s/sub\\deep.txt": "404:21:e43aa670",
  "/s/..\\outside\\secret.txt": "404:21:e43aa670",
  "/s/%5c..%5coutside%5csecret.txt": "404:21:e43aa670",
  "/s/f.txt%00.png": "404:21:e43aa670",
  "/s/%00": "404:21:e43aa670",
  "/s//etc/passwd": "404:21:e43aa670",
  "/s/$BASE/outside/secret.txt": "404:21:e43aa670",
  "/s//$BASE/root/f.txt": "404:21:e43aa670",
  "/s/abs_out.txt": "403:21:3f06beec",
  "/s/rel_out.txt": "403:21:3f06beec",
  "/s/abs_outdir/secret.txt": "403:21:3f06beec",
  "/s/rel_outdir/secret.txt": "403:21:3f06beec",
  "/s/abs_in.txt": "200:11:b6c774d6",
  "/s/rel_in.txt": "200:11:b6c774d6",
  "/s/subalias/deep.txt": "200:9:ab7eb780",
  "/s/abs_subalias/deep.txt": "200:9:ab7eb780",
  "/s/subalias/deeper/x.txt": "200:11:3dbefdc5",
  "/s/sub/up_in.txt": "200:11:b6c774d6",
  "/s/sub/up_out.txt": "403:21:3f06beec",
  "/s/dangling.txt": "404:21:e43aa670",
  "/s/loop1": "404:21:e43aa670",
  "/s/abs_outdir": "403:21:3f06beec",
  "/s/subalias": "404:21:e43aa670",
  "/l/f.txt": "200:11:b6c774d6",
  "/l/rel_in.txt": "200:11:b6c774d6",
  "/l/abs_in.txt": "200:11:b6c774d6",
  "/l/rel_out.txt": "403:21:3f06beec",
  "/l/abs_out.txt": "403:21:3f06beec",
  "/l/subalias/deep.txt": "200:9:ab7eb780",
};
if (std.getenv("RECORD") === "1") {
  build();
  const got = await askAll(targets());
  teardown();
  print(JSON.stringify(got, null, 1));
  std.exit(0);
}

const GROUPS = [
  ["regular files, the index and nested paths are served", (k) => /^\/s(\/|$)(f\.txt|index\.html|sub\/deep\.txt|sub\/deeper\/x\.txt)?$/.test(k)],
  ["missing files, directories without an index and dotfiles are 404", (k) => ["/s/sub/", "/s/a%20b.txt", "/s/nope.txt", "/s/sub/nope.txt", "/s/.hidden"].includes(k)],
  ["literal .. segments are refused with 403", (k) => ["/s/sub/../f.txt", "/s/../outside/secret.txt", "/s/.."].includes(k)],
  ["encoded dots, slashes, backslashes and NUL never escape the root", (k) => /%2e|%2E|%2f|%2F|%5c|\\|%00/.test(k)],
  ["absolute paths in the target are not filesystem paths", (k) => k.startsWith("/s//") || k.startsWith("/s/$BASE")],
  ["symlinks resolving outside the root are refused", (k) => k.startsWith("/s/") && /(_out|_outdir)/.test(k)],
  ["symlinks resolving inside the root are served; dangling and looping links are 404", (k) => k.startsWith("/s/") && /(_in\.txt|subalias|dangling|loop1)/.test(k)],
  ["a root that is itself a symlink keeps the same containment", (k) => k.startsWith("/l/")],
];

const t = suite("test_http_static_open_diff");
t.before(() => build());
t.after(() => teardown());
const claimed = new Set();
for (const [name, pick] of GROUPS) {
  const keys = Object.keys(want).filter((k) => !claimed.has(k) && pick(k));
  for (const k of keys) claimed.add(k);
  t.test(name, async (a) => {
    a.ok(keys.length > 0, "the group selects at least one target");
    const got = await askAll(targets().filter((x) => keys.includes(x.replace(String(base), "$BASE"))));
    for (const k of keys) a.eq(got[k], want[k], k);
  });
}
t.test("every pinned target belongs to exactly one case and the corpus size is unchanged", (a) => {
  a.eq(claimed.size, Object.keys(want).length, "every pinned target is checked");
  a.eq(targets().length, Object.keys(want).length, "corpus size");
});

await t.run();
