// App.static and symlinks: a link that leaves the document root is never
// served (file or directory component), a link that stays inside resolves to
// its target, and a root that is itself a symlink works. The fast open path
// and the resolving fallback must agree on every case.
import { suite } from "./kit.js";
import { App } from "dyna:net";
import { makeTempDir, writeFile, makeDir, symlink, removeAll, realPath } from "dyna:file";

const t = suite("static_symlink");
const base = realPath(makeTempDir("stsym"));
const root = base.join("root"), outside = base.join("outside");
makeDir(root); makeDir(outside); makeDir(root.join("sub"));
writeFile(root.join("f.txt"), "inside-file");
writeFile(root.join("sub").join("deep.txt"), "deep-file");
writeFile(outside.join("secret.txt"), "TOP-SECRET");
symlink(String(outside.join("secret.txt")), root.join("leak.txt"));
symlink(String(outside), root.join("leakdir"));
symlink("../outside/secret.txt", root.join("relleak.txt"));
symlink(String(root.join("f.txt")), root.join("alias.txt"));
symlink("sub", root.join("subalias"));
symlink(String(root), base.join("rootlink"));

const app = new App({ port: 0 });
app.static("/real", root, {});
app.static("/linked", base.join("rootlink"), {});
app.start();
t.after(() => { app.close(); removeAll(base); });
const B = "http://127.0.0.1:" + app.port;
const get = async (p) => { const r = await fetch(B + p); return [r.status, await r.text()]; };

t.test("plain files are served through both mounts", async (a) => {
    a.deep(await get("/real/f.txt"), [200, "inside-file"]);
    a.deep(await get("/real/sub/deep.txt"), [200, "deep-file"]);
    a.deep(await get("/linked/f.txt"), [200, "inside-file"], "a root that is a symlink");
    a.deep(await get("/linked/sub/deep.txt"), [200, "deep-file"]);
});

for (const p of ["/real/leak.txt", "/real/relleak.txt", "/real/leakdir/secret.txt", "/linked/leak.txt", "/linked/leakdir/secret.txt"]) {
    t.test("a link out of the root is refused: " + p, async (a) => {
        const [status, text] = await get(p);
        a.ok(status === 403 || status === 404, "status " + status);
        a.ok(!text.includes("TOP-SECRET"), "the outside file's bytes never leave");
    });
}

t.test("a link that stays inside the root resolves to its target", async (a) => {
    a.deep(await get("/real/alias.txt"), [200, "inside-file"]);
    a.deep(await get("/real/subalias/deep.txt"), [200, "deep-file"]);
});

t.test("missing files and directories are 404", async (a) => {
    a.eq((await get("/real/nope.txt"))[0], 404);
    a.eq((await get("/real/sub"))[0], 404);
    a.eq((await get("/real/f.txt/extra"))[0], 404);
});

await t.run();
