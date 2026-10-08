// Logger `dest` takes a Path as well as a string; rollover writes numbered
// files beside dest, and the `symlink` file reads back through realPath.
import { suite } from "./kit.js";
import { Logger } from "dyna:log";
import { Path, makeTempDir, exists, readFile, realPath, removeAll, lstat } from "dyna:file";

const t = suite("log_path_dest");
const dir = makeTempDir("logpath");
t.after(() => removeAll(dir));

t.test("a Path dest writes the same file a string dest does", (a) => {
    const viaPath = dir.join("p.log"), viaString = dir.join("s.log");
    const l1 = new Logger({ dest: viaPath, timestamp: false });
    const l2 = new Logger({ dest: String(viaString), timestamp: false });
    l1.info("hello");
    l2.info("hello");
    l1.flush();
    l2.flush();
    a.ok(exists(viaPath));
    a.eq(readFile(viaPath), readFile(viaString));
    a.match(readFile(viaPath), /"msg":"hello"/);
});

t.test("a non-path dest is still refused by name", (a) => {
    a.throws(() => new Logger({ dest: 42 }), /dest must be/);
    a.throws(() => new Logger({ dest: { toString() { return "/tmp/x"; } } }), /dest must be/);
});

t.test("rollover writes numbered files; dest appears only as the symlink", (a) => {
    const plain = dir.join("roll.log");
    const l = new Logger({ dest: plain, rollover: { size: "1k" } });
    l.info("x");
    l.flush();
    a.eq(exists(plain), false, "dest itself is not created");
    a.eq(exists(dir.join("roll.1.log")), true, "the active file is roll.1.log");

    const linked = dir.join("link.log");
    const l2 = new Logger({ dest: linked, rollover: { size: "1k", symlink: true } });
    l2.info("y");
    l2.flush();
    a.eq(lstat(linked).isSymlink, true);
    a.throws(() => readFile(linked), /symbolic link/i, "readFile refuses the symlink");
    a.match(readFile(realPath(linked)), /"msg":"y"/, "realPath reads it back");
});

await t.run();
