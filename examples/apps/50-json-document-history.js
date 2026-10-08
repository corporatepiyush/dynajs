// 50 · Versioned JSON documents — edits are JSON Patches; history, undo, audit and optimistic concurrency follow.
//
// WHAT IT SHOWS
//   - dyna:json Patch (RFC 6902): changes as data that can be validated, stored, replayed and reversed
//   - dyna:json Pointer (RFC 6901): addressing one field, including keys containing "/" or "~"
//   - computing the INVERSE of a patch at apply time, which is what makes undo exact
//   - a "test" operation as optimistic locking: an edit based on stale data is refused
//   - an allow-list of editable paths, so a patch cannot touch fields its author may not change
//
// RUN      dynajs examples/apps/50-json-document-history.js

import { Patch, Pointer } from "dyna:json";

class VersionedDocument {
    constructor(initial, { editable = [""] } = {}) {
        this.current = structuredClone(initial);
        this.history = [];                         // { version, author, at, patch, inverse }
        this.editable = editable;                  // pointer prefixes a patch may touch
    }
    get version() { return this.history.length; }

    // Build the patch that undoes `ops`, by looking at the document before each op.
    #invert(doc, ops) {
        const inverse = [];
        let working = doc;
        for (const op of ops) {
            const had = Pointer.has(working, op.path);
            const before = had ? Pointer.get(working, op.path) : undefined;
            if (op.op === "add") {
                // Adding to an array index inserts; adding to an existing object key replaces.
                const intoArray = Array.isArray(Pointer.get(working, op.path.slice(0, op.path.lastIndexOf("/")) || ""));
                inverse.unshift(had && !intoArray ? { op: "replace", path: op.path, value: before } : { op: "remove", path: this.#resolved(working, op.path) });
            } else if (op.op === "remove") inverse.unshift({ op: "add", path: op.path, value: before });
            else if (op.op === "replace") inverse.unshift({ op: "replace", path: op.path, value: before });
            else if (op.op !== "test") throw new Error(`operation "${op.op}" is not supported by this store`);
            working = Patch.apply(working, [op]);
        }
        return inverse;
    }
    // "/tags/-" means "append"; its inverse must name the real index.
    #resolved(doc, path) {
        if (!path.endsWith("/-")) return path;
        const parent = path.slice(0, -2);
        return `${parent}/${Pointer.get(doc, parent || "").length}`;
    }

    apply(ops, { author, expectedVersion } = {}) {
        // Optimistic concurrency, the cheap way: the caller says which version it read.
        if (expectedVersion !== undefined && expectedVersion !== this.version)
            throw new Error(`conflict: document is at version ${this.version}, edit was based on ${expectedVersion}`);
        for (const op of ops)
            if (op.op !== "test" && !this.editable.some((prefix) => op.path === prefix || op.path.startsWith(prefix + "/")))
                throw new Error(`path ${op.path} is not editable`);

        const inverse = this.#invert(this.current, ops);
        // Patch.apply is all-or-nothing and copy-on-write: if any operation
        // fails (including a "test"), it throws and `current` is untouched.
        this.current = Patch.apply(this.current, ops);
        this.history.push({ version: this.version + 1, author, at: new Date().toISOString(), patch: ops, inverse });
        return this.version;
    }

    undo(author) {
        const last = this.history[this.history.length - 1];
        if (!last) throw new Error("nothing to undo");
        this.current = Patch.apply(this.current, last.inverse);
        this.history.pop();
        return this.version;
    }

    // Rebuild any past version from the first one and the stored patches.
    at(version, initial) {
        return this.history.slice(0, version).reduce((doc, h) => Patch.apply(doc, h.patch), structuredClone(initial));
    }

    // Who changed this field, and when?
    blame(pointer) {
        return this.history.filter((h) => h.patch.some((op) => op.op !== "test" && (op.path === pointer || op.path.startsWith(pointer + "/"))))
                           .map((h) => ({ version: h.version, author: h.author }));
    }
}

// ---- demo / self-test ------------------------------------------------------
const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
const refusal = (fn) => { try { fn(); return ""; } catch (e) { return e.message; } };

const initial = {
    id: "prod-17", title: "Compact keyboard", price: { amount: 12990, currency: "EUR" },
    tags: ["hardware"], stock: 40, owner: "catalogue-team", "notes/internal": "",
};
// Editors may change the listing, not its identity or ownership.
const doc = new VersionedDocument(initial, { editable: ["/title", "/price", "/tags", "/stock", "/notes~1internal"] });

doc.apply([{ op: "replace", path: "/title", value: "Compact 75% keyboard" }], { author: "ada" });
doc.apply([{ op: "add", path: "/tags/-", value: "mechanical" }, { op: "replace", path: "/price/amount", value: 11990 }], { author: "bob" });
check(doc.version === 2 && doc.current.tags.join() === "hardware,mechanical" && doc.current.price.amount === 11990, "patches apply in order");
check(initial.tags.length === 1, "the original object is never mutated");

// A key containing "/" is addressed with the ~1 escape.
doc.apply([{ op: "replace", path: "/" + Pointer.escape("notes/internal"), value: "check supplier" }], { author: "ada" });
check(doc.current["notes/internal"] === "check supplier", "keys with slashes are addressable");

// Stale edit: carol read version 2 and tries to save over version 3.
check(refusal(() => doc.apply([{ op: "replace", path: "/stock", value: 39 }], { author: "carol", expectedVersion: 2 })).startsWith("conflict"),
      "an edit based on an old version is refused");

// The same protection inside the patch itself: "only if the price is still 11990".
const guarded = [{ op: "test", path: "/price/amount", value: 12990 }, { op: "replace", path: "/price/amount", value: 9990 }];
check(refusal(() => doc.apply(guarded, { author: "carol" })) !== "" && doc.current.price.amount === 11990 && doc.version === 3,
      "a failed test aborts the whole patch and records nothing");

// Forbidden fields.
check(refusal(() => doc.apply([{ op: "replace", path: "/owner", value: "mallory" }], { author: "mallory" })).includes("not editable"),
      "a patch outside the editable paths is refused");
check(refusal(() => doc.apply([{ op: "remove", path: "/id" }], { author: "mallory" })).includes("not editable"), "identity cannot be removed");

// History, blame, time travel.
check(doc.blame("/price").map((b) => b.author).join() === "bob", "blame finds who changed a nested field");
check(doc.at(1, initial).title === "Compact 75% keyboard" && doc.at(1, initial).tags.length === 1, "any past version can be rebuilt");
check(JSON.stringify(doc.at(0, initial)) === JSON.stringify(initial), "version 0 is the original");

// Undo walks back exactly, including the array append and the multi-op patch.
doc.undo(); doc.undo();
check(doc.version === 1 && doc.current.tags.join() === "hardware" && doc.current.price.amount === 12990, "undo reverses a multi-operation patch");
doc.undo();
check(JSON.stringify(doc.current) === JSON.stringify(initial), "undoing everything restores the original exactly");
check(refusal(() => doc.undo()) === "nothing to undo", "undo past the beginning is refused");
console.log("self-test passed: apply, conflict detection, guarded edits, blame, time travel, undo");
