// covers: dyna:csv.CSVFile
// CSVFile.rows() parses the file once, at the first next(), and cuts every later batch from that snapshot.
// It used to reload and re-parse the whole file on each next() (deep line-read wave D-D, OPT): batch 10
// over 20,000 rows took 2.66 s instead of 5 ms, and a file rewritten mid-iteration changed totalRows and
// the rows under the iterator. Pinned here by the snapshot behaviour (a rewrite after the first batch is
// not seen) and by exact batch boundaries; return() and running to the end both release the table.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_csv_csv_file_rows_snapshot");
import { CSVFile } from "dyna:csv";
import { makeTempDir, writeFile, Path } from "dyna:file";

let n = 0, fails = 0;
const check = (c, m) => { n++; if (!c) { fails++; print("FAIL: " + m); } };
const dir = makeTempDir("csvsnap");
const p = new Path(dir + "/t.csv");
let text = "id,name\n";
for (let i = 0; i < 25; i++) text += i + ",n" + i + "\n";
writeFile(p, text);

await __kitS.section("first batch: 10 of 25", async () => {
    const it = new CSVFile(p).rows({ batch: 10 });
    const first = await it.next();
    check(!first.done && first.value.rows.length === 10 && first.value.totalRows === 25 && first.value.offset === 0, "first batch: 10 of 25");
    writeFile(p, "id,name\n99,changed\n");
    const second = await it.next();
    check(!second.done && second.value.totalRows === 25 && second.value.offset === 10, "the rewrite is not seen: totalRows stays 25");
    check(second.value.rows[0][0] === "10" && second.value.rows[9][1] === "n19", "second batch is rows 10..19 of the snapshot");
    const third = await it.next();
    check(!third.done && third.value.rows.length === 5 && third.value.rows[4][0] === "24", "last batch is the 5-row tail");
    check((await it.next()).done, "then done");
    check((await it.next()).done, "and stays done");
});
writeFile(p, text);
await __kitS.section("return() ends the iteration", async () => {
    const it = new CSVFile(p).rows({ batch: 7 });
    await it.next();
    check((await it.return()).done, "return() ends the iteration");
    check((await it.next()).done, "next() after return() is done");
});
await __kitS.section("for await over batch 3 covers every row once, in order", async () => {
    let seen = 0, last = -1, ok = true;
    for await (const b of new CSVFile(p).rows({ batch: 3 })) {
        if (b.offset !== last + 1) ok = false;
        last = b.offset + b.rows.length - 1;
        seen += b.rows.length;
    }
    check(ok && seen === 25, "for await over batch 3 covers every row once, in order");
});
await __kitS.section("a header-only file is done at once", async () => {
    writeFile(p, "a,b\n");
    const it = new CSVFile(p).rows({ batch: 5 });
    check((await it.next()).done, "a header-only file is done at once");
});
__kitS.end();
print("test_csv_rows_snapshot: " + n + " checks, " + fails + " failed");
if (fails) throw new Error("test_csv_rows_snapshot: " + fails + " failures");
