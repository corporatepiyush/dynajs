// flags: --std
import * as std from "std";
import { Watcher, Path } from "dyna:file";
import * as file from "dyna:file";

let n = 0, bad = 0, done = false;
function ok(c, what) { n++; if (!c) { bad++; print("FAIL: " + what); } }

const ROOT = `${std.getenv("TMPDIR") || "/tmp"}/dj_watch_${Date.now()}_${Math.floor(Math.random() * 1e9)}`;
const P = (x) => new Path(x);
file.makeDir(P(ROOT), { recursive: true });
file.makeDir(P(ROOT + "/sub"), { recursive: true });
file.makeDir(P(ROOT + "/skipme"), { recursive: true });
file.writeFile(P(ROOT + "/a.txt"), "one");

const events = [];
const w = new Watcher(P(ROOT), { debounceMs: 30, ignore: ["skipme"] });
w.start((e) => { events.push(e.kind + " " + e.path); });

const s0 = w.stats();
ok(s0.entries === 2,
   "baseline is sub + a.txt with skipme ignored, got " + s0.entries);
ok(events.length === 0, "the baseline emits nothing for existing files");
ok(s0.truncated === false, "the entry cap was not hit");

function has(type, path) {
    return events.indexOf(type + " " + path) >= 0;
}

setTimeout(() => {
    file.writeFile(P(ROOT + "/b.txt"), "new file");
    file.makeDir(P(ROOT + "/sub/deep"), { recursive: true });

    setTimeout(() => {
        ok(has("add", "b.txt"), "a created file is 'add', saw: " + events.join(", "));
        ok(has("addDir", "sub/deep"), "a created directory is 'addDir'");
        ok(!has("add", "skipme"), "an ignored path is not reported");

        const before = events.length;
        file.writeFile(P(ROOT + "/a.txt"), "one two three");

        setTimeout(() => {
            ok(has("change", "a.txt"),
               "a modified file is 'change', not 'add': " +
               events.slice(before).join(", "));
            ok(!has("add", "a.txt"), "an existing file is never re-reported as add");

            file.remove(P(ROOT + "/b.txt"));

            setTimeout(() => {
                ok(has("unlink", "b.txt"), "a deleted file is 'unlink'");

                file.writeFile(P(ROOT + "/sub/deep/c.txt"), "deep");

                setTimeout(() => {
                    ok(has("add", "sub/deep/c.txt"),
                       "a file in a NEWLY created subtree is seen: " +
                       events.join(", "));

                    const s = w.stats();
                    ok(s.events >= 5, "stats counted the events: " + s.events);
                    ok(s.directories >= 1 || s.entries > 0,
                       "stats report the watched set");

                    done = true;
                    w.close();
                    file.removeAll(P(ROOT));
                    print("test_watch: " + n + " assertions, " + bad + " failures");
                    if (bad) throw new Error(bad + " failures");
                }, 400);
            }, 400);
        }, 400);
    }, 500);
}, 200);

setTimeout(() => {
    if (done) return;
    print("FAIL: the watcher never completed its event chain");
    print("  events seen: " + events.join(", "));
    throw new Error("watcher did not report");
}, 9000);

(async () => {
    const n0 = n;
    const root2 = ROOT + "_close_in_cb";
    file.makeDir(P(root2), { recursive: true });
    const w2 = new Watcher(P(root2), { debounceMs: 10 });
    let closedInCb = false;
    w2.start((e) => {
        if (!closedInCb) { closedInCb = true; w2.close(); }
    });
    file.writeFile(P(root2 + "/x.txt"), "1");
    await new Promise((r) => setTimeout(r, 300));
    file.writeFile(P(root2 + "/y.txt"), "1");
    await new Promise((r) => setTimeout(r, 300));
    ok(closedInCb && w2.closed, "close inside the callback took effect");
    file.writeFile(P(root2 + "/z.txt"), "1");
    await new Promise((r) => setTimeout(r, 200));
    file.removeAll(P(root2));
    print("test_watch (close-in-callback): " + (n - n0) + " assertions done");
})();
