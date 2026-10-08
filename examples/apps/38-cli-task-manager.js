// 38 · Command-line task manager — subcommands, typed options, a SQLite store and table output.
//
// WHAT IT SHOWS
//   - dyna:cli Command: subcommands with typed options, required arguments and generated help
//   - dyna:cli Table and StyleText: readable output that degrades cleanly when piped
//   - dyna:net SQLite in a per-user data directory (dyna:file dataDir)
//   - separating "parse the command line" from "do the work", which is what makes a CLI testable
//
// RUN      dynajs examples/apps/38-cli-task-manager.js add "Write report" --priority 2 --due 2026-04-01
//          dynajs examples/apps/38-cli-task-manager.js list --all
//          dynajs examples/apps/38-cli-task-manager.js done 1
//          With no arguments it runs a self-test against an in-memory database.

import { Command, Table, StyleText } from "dyna:cli";
import { SQLite } from "dyna:net";
import { dataDir, makeDir } from "dyna:file";
import { parseDate } from "dyna:time";

// ---- the store: plain functions over a database handle ----------------------
function openStore(path) {
    const db = new SQLite(path);
    db.exec(`CREATE TABLE IF NOT EXISTS tasks(
               id INTEGER PRIMARY KEY AUTOINCREMENT, title TEXT NOT NULL,
               priority INTEGER NOT NULL DEFAULT 3, due TEXT, done INTEGER NOT NULL DEFAULT 0)`);
    return {
        add(title, priority, due) {
            if (!title.trim()) throw new Error("a task needs a title");
            if (!(priority >= 1 && priority <= 5)) throw new Error("priority must be 1 (highest) to 5");
            if (due) parseDate(due);                       // throws on anything but YYYY-MM-DD
            db.exec("INSERT INTO tasks(title, priority, due) VALUES (?, ?, ?)", [title.trim(), priority, due ?? null]);
            return db.lastInsertRowId;
        },
        list(includeDone) {
            // Open tasks first, then by priority, then by due date with undated last.
            return db.query(`SELECT * FROM tasks ${includeDone ? "" : "WHERE done = 0"}
                             ORDER BY done, priority, due IS NULL, due, id`);
        },
        complete(id) { return db.exec("UPDATE tasks SET done = 1 WHERE id = ? AND done = 0", [id]) === 1; },
        remove(id) { return db.exec("DELETE FROM tasks WHERE id = ?", [id]) === 1; },
        close() { db.close(); },
    };
}

// ---- the command line: each subcommand returns the text to print ------------
function buildCli(store, today) {
    const render = (rows) => rows.length === 0 ? "nothing to do" : Table(
        rows.map((t) => [t.id, t.done ? "x" : " ", "P" + t.priority,
                         t.due ? (t.due < today && !t.done ? StyleText("red", t.due) : t.due) : "-", t.title]),
        { head: ["id", "", "pri", "due", "task"], align: ["right", "left", "left", "left", "left"] });

    const add = new Command("add").describe("add a task")
        .argument("title", "what needs doing")
        .option("-p, --priority <n>", "1 (highest) to 5", { type: "number", default: 3 })
        .option("-d, --due <date>", "due date, YYYY-MM-DD", { type: "string" })
        .action((opts, args) => `added task ${store.add(args[0] ?? "", opts.priority, opts.due)}`);

    const list = new Command("list").describe("show tasks")
        .option("-a, --all", "include completed tasks", { type: "boolean" })
        .action((opts) => render(store.list(!!opts.all)));

    const done = new Command("done").describe("mark a task complete").argument("id")
        .action((_o, args) => store.complete(Number(args[0])) ? `task ${args[0]} completed` : `no open task ${args[0]}`);

    const rm = new Command("rm").describe("delete a task").argument("id")
        .action((_o, args) => store.remove(Number(args[0])) ? `task ${args[0]} deleted` : `no task ${args[0]}`);

    return new Command("tasks").describe("a small task manager").version("1.0.0")
        .command(add).command(list).command(done).command(rm);
}

// run() is the whole program as a function of argv: easy to call from a test.
function run(store, argv, today = new Date().toISOString().slice(0, 10)) {
    const cli = buildCli(store, today);
    if (argv.length === 0) return cli.help();
    try {
        // A subcommand's parse result nests inside its parent's: follow
        // `result` down to the value the leaf action returned.
        let out = cli.parse(argv);
        while (out && typeof out === "object" && "result" in out) out = out.result;
        return typeof out === "string" ? out : cli.help();
    } catch (e) {
        return "error: " + e.message;
    }
}

// ---- entry point -----------------------------------------------------------
const argv = scriptArgs.slice(1);
if (argv.length) {
    const dir = dataDir("dynajs-tasks");
    makeDir(dir, { recursive: true });
    const store = openStore(String(dir.join("tasks.db")));
    console.log(run(store, argv));
    store.close();
} else {
    const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
    const store = openStore(":memory:");
    const today = "2026-03-15";
    const exec = (...args) => run(store, args, today);

    check(exec("add", "Write report", "--priority", "2", "--due", "2026-04-01") === "added task 1", "add with options");
    check(exec("add", "Pay invoice", "-p", "1", "-d", "2026-03-01") === "added task 2", "short flags work");
    check(exec("add", "Tidy desk") === "added task 3", "defaults apply");
    check(exec("add", "Bad", "--priority", "9").startsWith("error: priority"), "an out-of-range priority is refused");
    check(exec("add", "Bad", "--due", "next week").startsWith("error:"), "a malformed date is refused");
    check(exec("add", "Bad", "--priorty", "1").startsWith("error:"), "a misspelt flag is an error, not ignored");

    const order = store.list(false).map((t) => t.title).join(" | ");
    check(order === "Pay invoice | Write report | Tidy desk", "tasks sort by priority: " + order);
    check(exec("done", "2") === "task 2 completed" && exec("done", "2") === "no open task 2", "completion is idempotent");
    check(store.list(false).length === 2 && store.list(true).length === 3, "--all includes completed tasks");
    check(exec("rm", "3") === "task 3 deleted" && exec("rm", "3") === "no task 3", "delete reports what happened");
    const table = exec("list", "--all");
    check(table.includes("Write report") && table.includes("Pay invoice"), "the table lists tasks");
    console.log(table);
    console.log(exec().split("\n")[0]);
    console.log("self-test passed");
    store.close();
}
