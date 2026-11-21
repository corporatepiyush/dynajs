// flags: --std
// covers: dyna:sqlite.SQLite
// A writable SQLite file opens in WAL with synchronous=NORMAL unless { wal: false }: the journal
// mode is a property of the FILE, so each case reads it back with PRAGMA rather than trusting
// the option. Read-only opens and in-memory databases keep the mode SQLite gives them.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_sqlite_sq_lite_sqlite_wal", { chain: true });
import { SQLite } from "dyna:sqlite";
import * as os from "os";
import * as std from "std";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { print("FAIL: " + m); fails++; } }

const dir = (std.getenv("TMPDIR") || "/tmp").replace(/\/$/, "") + "/sqlite-wal-" + os.getpid() + "-" + Math.floor(Math.random() * 1e9);
os.mkdir(dir, 0o700);
const mode = (db) => db.query("PRAGMA journal_mode")[0].journal_mode;
const sync = (db) => db.query("PRAGMA synchronous")[0].synchronous;

__kitS.section("a writable file database defaults to WAL", () => {
    const db = new SQLite(dir + "/default.db");
    check(mode(db) === "wal", "a writable file database defaults to WAL, got " + mode(db));
    check(sync(db) === 1, "WAL runs with synchronous=NORMAL (1), got " + sync(db));
    db.exec("create table t(x)");
    db.exec("insert into t values (?)", [42]);
    const other = new SQLite(dir + "/default.db");
    check(other.query("select x from t")[0]?.x === 42, "a second connection sees the committed row");
    other.close();
    db.close();
    const again = new SQLite(dir + "/default.db");
    check(again.query("select x from t")[0]?.x === 42, "the row survives close and reopen");
    check(mode(again) === "wal", "the file stays in WAL");
    again.close();
});
__kitS.section("{ wal: false } keeps the rollback journal", () => {
    const db = new SQLite(dir + "/rollback.db", { wal: false });
    check(mode(db) === "delete", "{ wal: false } keeps the rollback journal, got " + mode(db));
    check(sync(db) === 2, "{ wal: false } keeps synchronous=FULL (2), got " + sync(db));
    db.exec("create table t(x)");
    db.close();
});
__kitS.section("a read-only open does not switch the file to WAL", () => {
    const ro = new SQLite(dir + "/rollback.db", { readonly: true });
    check(mode(ro) === "delete", "a read-only open does not switch the file to WAL, got " + mode(ro));
    ro.close();
    const ro2 = new SQLite(dir + "/default.db", { readonly: true });
    check(mode(ro2) === "wal", "a read-only open of a WAL file reads it, got " + mode(ro2));
    check(ro2.query("select x from t")[0]?.x === 42, "a read-only open of a WAL file sees the data");
    ro2.close();
});
__kitS.section("an in-memory database stays in memory mode", () => {
    const mem = new SQLite(":memory:");
    check(mode(mem) === "memory", "an in-memory database stays in memory mode, got " + mode(mem));
    check(sync(mem) === 2, "synchronous is untouched when WAL did not take, got " + sync(mem));
    mem.close();
});
__kitS.end();

for (const f of ["default.db", "default.db-wal", "default.db-shm", "rollback.db"]) os.remove(dir + "/" + f);
os.remove(dir);
print(`test_net_sqlite_wal: ${n - fails} passed, ${fails} failed`);
std.exit(fails ? 1 : 0);
