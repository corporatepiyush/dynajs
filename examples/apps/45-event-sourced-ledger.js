// 45 · Event-sourced ledger — an append-only log is the source of truth; balances are rebuilt by replaying it.
//
// WHAT IT SHOWS
//   - an append-only NDJSON journal with a hash chain: any edit to history is detectable
//   - dyna:file FileLock: two processes cannot interleave appends
//   - FileWriter.sync: an acknowledged entry is on disk, not in a buffer
//   - dyna:decimal Money for amounts, and double-entry rules that make "money vanished" impossible
//   - snapshots, so startup does not replay years of events
//
// RUN      dynajs examples/apps/45-event-sourced-ledger.js

import { Money } from "dyna:decimal";
import { SHA256Hex } from "dyna:hash";
import { ndjson, fromFile } from "dyna:stream";
import { FileWriter, FileLock, makeTempDir, removeAll, exists, readFile, writeFile } from "dyna:file";
import { v7 } from "dyna:uuid";

const CURRENCY = "USD";
const GENESIS = "0".repeat(64);

class Ledger {
    constructor(dir) {
        this.journal = dir.join("journal.ndjson");
        this.snapshot = dir.join("snapshot.json");
        this.lock = dir.join("journal.lock");
        this.balances = new Map();                   // account -> minor units
        this.lastHash = GENESIS; this.count = 0;
    }

    // Rebuild state: load the snapshot if there is one, then replay what follows.
    async open() {
        let skip = 0;
        if (exists(this.snapshot)) {
            const snap = JSON.parse(readFile(this.snapshot));
            this.balances = new Map(Object.entries(snap.balances));
            this.lastHash = snap.lastHash; this.count = skip = snap.count;
        }
        if (!exists(this.journal)) return this;
        let seen = 0;
        let prev = GENESIS;
        for await (const entry of ndjson(fromFile(this.journal))) {
            // Every entry commits to the one before it. Changing or deleting
            // any past line breaks the chain from that point on.
            const { hash, ...body } = entry;
            if (body.prev !== prev || SHA256Hex(JSON.stringify(body)) !== hash)
                throw new Error(`journal corrupted at entry ${seen + 1}`);
            prev = hash;
            if (++seen > skip) this.#apply(body);
        }
        if (seen < skip) throw new Error("journal is shorter than its snapshot");
        this.lastHash = prev; this.count = seen;
        return this;
    }

    #apply(event) {
        for (const leg of event.legs)
            this.balances.set(leg.account, (this.balances.get(leg.account) ?? 0) + leg.minor);
    }

    // Record a transfer. Validation happens BEFORE anything is written.
    transfer(from, to, amountText, memo = "") {
        const amount = Money.fromString(amountText, CURRENCY);
        if (amount.amount() <= 0) throw new Error("amount must be positive");
        if (from === to) throw new Error("cannot transfer to the same account");
        // Accounts starting with "equity:" may go negative (they fund the system);
        // everything else must cover its withdrawals.
        if (!from.startsWith("equity:") && (this.balances.get(from) ?? 0) < amount.amount())
            throw new Error(`insufficient funds in ${from}`);

        // Double entry: the legs of every event sum to zero, so the total
        // across all accounts is always zero. Money moves; it never appears.
        const body = {
            id: v7(), at: new Date().toISOString(), memo, prev: this.lastHash,
            legs: [{ account: from, minor: -amount.amount() }, { account: to, minor: amount.amount() }],
        };
        const entry = { ...body, hash: SHA256Hex(JSON.stringify(body)) };

        // The lock serializes writers across processes; sync() makes the line
        // durable before the caller is told it succeeded.
        const lock = new FileLock(this.lock, { retry: 50, retryMs: 10 });
        lock.withLock(() => {
            const writer = new FileWriter(this.journal, { append: true });
            writer.write(JSON.stringify(entry) + "\n");
            writer.sync();
            writer.close();
        });
        this.#apply(body);
        this.lastHash = entry.hash; this.count++;
        return entry.id;
    }

    balance(account) { return new Money(this.balances.get(account) ?? 0, CURRENCY); }
    total() { return [...this.balances.values()].reduce((a, b) => a + b, 0); }

    // Persist current state so the next open() replays only newer entries.
    writeSnapshot() {
        writeFile(this.snapshot, JSON.stringify({ count: this.count, lastHash: this.lastHash, balances: Object.fromEntries(this.balances) }));
    }
}

// ---- self-test -------------------------------------------------------------
const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
const refusal = (fn) => { try { fn(); return ""; } catch (e) { return e.message; } };
const dir = makeTempDir("ledger");

const ledger = await new Ledger(dir).open();
ledger.transfer("equity:opening", "alice", "100.00", "opening balance");
ledger.transfer("equity:opening", "bob", "20.00", "opening balance");
ledger.transfer("alice", "bob", "35.50", "invoice 17");
check(ledger.balance("alice").amount() === 6450 && ledger.balance("bob").amount() === 5550, "balances follow the transfers");
check(ledger.total() === 0, "double entry: all accounts sum to zero");
check(refusal(() => ledger.transfer("bob", "alice", "500.00")).includes("insufficient"), "an overdraft is refused");
check(refusal(() => ledger.transfer("bob", "alice", "1.005")) !== "", "a fraction of a cent is refused");
check(ledger.count === 3, "refused transfers leave no trace in the journal");

// A fresh process reaches the same state purely from the journal.
const replayed = await new Ledger(dir).open();
check(replayed.balance("alice").equals(ledger.balance("alice")) && replayed.count === 3, "replay reproduces the balances");

// Snapshot, add one more event, reopen: only the tail is replayed.
replayed.writeSnapshot();
replayed.transfer("bob", "carol", "5.25", "lunch");
const fromSnapshot = await new Ledger(dir).open();
check(fromSnapshot.balance("carol").amount() === 525 && fromSnapshot.count === 4, "snapshot plus tail equals full replay");

// Tamper with history: make alice's incoming payment larger.
const journalPath = dir.join("journal.ndjson");
const original = readFile(journalPath);
writeFile(journalPath, original.replace('"minor":10000', '"minor":90000'));
const tampered = await new Ledger(dir).open().then(() => "", (e) => e.message);
check(tampered.includes("corrupted at entry 1"), "an edited entry breaks the hash chain: " + tampered);

// Remove a line from the middle instead.
const lines = original.trim().split("\n");
writeFile(journalPath, [lines[0], lines[2], lines[3]].join("\n") + "\n");
check((await new Ledger(dir).open().then(() => "", (e) => e.message)).includes("corrupted at entry 2"), "a deleted entry is detected");
console.log("self-test passed: 4 events, replay, snapshot, tamper detection");
removeAll(dir);
