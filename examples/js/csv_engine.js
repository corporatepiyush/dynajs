// csv_engine.js — a streaming RFC-4180 CSV parser plus a small lazy query
// engine (filter / map / group-by / aggregate).
//
// The parser is a character-level state machine implemented as a `function*`
// generator, so rows stream out one at a time without buffering the whole file.
// The query layer leans on the modern Iterator-helper methods (`.map`,
// `.filter`, `.take`, `.drop`) and `Map.groupBy`.
//
// Engine features exercised: generators + iterator protocol, Iterator helpers,
// Map.groupBy, Object.groupBy, optional chaining, and tagged-value records.

import { test, run, assert, assertEqual, deepEqual } from "./harness.js";

const State = Object.freeze({
  FIELD_START: 0,
  IN_FIELD: 1,
  IN_QUOTED: 2,
  AFTER_QUOTE: 3, // just saw a closing quote inside a quoted field
});

/**
 * Streaming CSV tokenizer. Yields arrays of string cells, one per record.
 * Handles quoted fields, escaped quotes (""), embedded commas and newlines,
 * and both LF and CRLF line endings.
 *
 * @param {string} text
 * @param {{ delimiter?: string }} [opts]
 * @returns {Generator<string[]>}
 */
export function* parseRows(text, { delimiter = "," } = {}) {
  let state = State.FIELD_START;
  let field = "";
  let row = [];
  let sawAnyChar = false;

  const pushField = () => { row.push(field); field = ""; };
  const endRow = function* () {
    pushField();
    yield row;
    row = [];
  };

  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    sawAnyChar = true;
    switch (state) {
      case State.FIELD_START:
        if (c === '"') { state = State.IN_QUOTED; }
        else if (c === delimiter) { pushField(); }
        else if (c === "\n") { yield* endRow(); state = State.FIELD_START; }
        else if (c === "\r") { /* wait for \n */ }
        else { field += c; state = State.IN_FIELD; }
        break;

      case State.IN_FIELD:
        if (c === delimiter) { pushField(); state = State.FIELD_START; }
        else if (c === "\n") { yield* endRow(); state = State.FIELD_START; }
        else if (c === "\r") { /* swallow, expect \n */ }
        else { field += c; }
        break;

      case State.IN_QUOTED:
        if (c === '"') { state = State.AFTER_QUOTE; }
        else { field += c; } // commas, newlines, everything literal
        break;

      case State.AFTER_QUOTE:
        if (c === '"') { field += '"'; state = State.IN_QUOTED; } // escaped quote
        else if (c === delimiter) { pushField(); state = State.FIELD_START; }
        else if (c === "\n") { yield* endRow(); state = State.FIELD_START; }
        else if (c === "\r") { /* expect \n */ }
        else { field += c; state = State.IN_FIELD; } // lenient: text after quote
        break;
    }
  }

  // Emit the final record unless the input ended cleanly on a row boundary.
  if (state !== State.FIELD_START || field !== "" || row.length > 0) {
    if (sawAnyChar) yield* endRow();
  }
}

/**
 * Parse CSV into an array of plain objects keyed by the header row.
 * @param {string} text
 * @param {{ delimiter?: string, headers?: string[] }} [opts]
 */
export function parseObjects(text, { delimiter = ",", headers } = {}) {
  const rows = parseRows(text, { delimiter });
  const iter = rows[Symbol.iterator]();
  const cols = headers ?? iter.next().value;
  if (!cols) return [];
  const out = [];
  for (let r = iter.next(); !r.done; r = iter.next()) {
    const record = {};
    cols.forEach((name, i) => { record[name] = r.value[i] ?? ""; });
    out.push(record);
  }
  return out;
}

/**
 * A tiny fluent query over an iterable of records. Each combinator returns a
 * fresh Query wrapping a lazy iterator, so nothing is computed until a terminal
 * (`toArray`, `groupBy`, `count`, `reduce`) is called.
 */
export class Query {
  #iterable;

  constructor(iterable) {
    this.#iterable = iterable;
  }

  static from(iterable) { return new Query(iterable); }

  where(pred) {
    // Iterator.prototype.filter — a modern iterator helper.
    return new Query(Iterator.from(this.#iterable[Symbol.iterator]()).filter(pred));
  }

  select(fn) {
    return new Query(Iterator.from(this.#iterable[Symbol.iterator]()).map(fn));
  }

  take(n) {
    return new Query(Iterator.from(this.#iterable[Symbol.iterator]()).take(n));
  }

  drop(n) {
    return new Query(Iterator.from(this.#iterable[Symbol.iterator]()).drop(n));
  }

  toArray() { return [...this.#iterable]; }

  count() {
    let n = 0;
    for (const _ of this.#iterable) n++;
    return n;
  }

  reduce(fn, init) {
    let acc = init;
    for (const v of this.#iterable) acc = fn(acc, v);
    return acc;
  }

  /** Group by a key function using the built-in Map.groupBy. */
  groupBy(keyFn) {
    return Map.groupBy(this.toArray(), keyFn);
  }
}

/** Aggregate helper: sum a numeric projection over a group. */
export function sumBy(items, fn) {
  return items.reduce((s, x) => s + fn(x), 0);
}

// --- tests -------------------------------------------------------------------

const SAMPLE = `name,dept,salary,note
"Ada, Lovelace",eng,120,"first, programmer"
Grace Hopper,eng,115,"loves ""bugs"""
Katherine,science,110,
Alan,eng,130,"multi
line note"
Dorothy,science,105,`;

test("parses quoted fields, escapes, and embedded newlines", () => {
  const rows = [...parseRows(SAMPLE)];
  assertEqual(rows[0], ["name", "dept", "salary", "note"]);
  assertEqual(rows[1], ["Ada, Lovelace", "eng", "120", "first, programmer"]);
  assertEqual(rows[2][3], 'loves "bugs"'); // escaped quotes collapsed
  assertEqual(rows[4][3], "multi\nline note"); // embedded newline preserved
  assertEqual(rows.length, 6); // header + 5 data rows
});

test("handles CRLF and missing trailing newline", () => {
  assertEqual([...parseRows("a,b\r\n1,2\r\n3,4")], [["a", "b"], ["1", "2"], ["3", "4"]]);
  assertEqual([...parseRows("x,y")], [["x", "y"]]);
  assertEqual([...parseRows("")], []);
  assertEqual([...parseRows("a,,c")], [["a", "", "c"]]); // empty middle field
});

test("parseObjects keys by header", () => {
  const objs = parseObjects("id,city\n1,NYC\n2,LA");
  assertEqual(objs, [{ id: "1", city: "NYC" }, { id: "2", city: "LA" }]);
});

test("lazy query: where + select + take via iterator helpers", () => {
  const people = parseObjects(SAMPLE);
  const result = Query.from(people)
    .where((p) => p.dept === "eng")
    .select((p) => ({ name: p.name, pay: Number(p.salary) }))
    .toArray();
  assertEqual(result.length, 3);
  assertEqual(result[0], { name: "Ada, Lovelace", pay: 120 });

  const firstTwoNames = Query.from(people).select((p) => p.name).take(2).toArray();
  assertEqual(firstTwoNames, ["Ada, Lovelace", "Grace Hopper"]);
});

test("group-by department with Map.groupBy + aggregation", () => {
  const people = parseObjects(SAMPLE);
  const byDept = Query.from(people).groupBy((p) => p.dept);
  assert(byDept instanceof Map);
  assertEqual(byDept.get("eng").length, 3);
  assertEqual(byDept.get("science").length, 2);

  // Average eng salary.
  const eng = byDept.get("eng");
  const avg = sumBy(eng, (p) => Number(p.salary)) / eng.length;
  assertEqual(avg, (120 + 115 + 130) / 3);
});

test("Object.groupBy parity for small keyspaces", () => {
  const nums = [1, 2, 3, 4, 5, 6];
  const grouped = Object.groupBy(nums, (n) => (n % 2 === 0 ? "even" : "odd"));
  assertEqual(grouped.even, [2, 4, 6]);
  assertEqual(grouped.odd, [1, 3, 5]);
});

test("streaming stays lazy: take short-circuits an infinite source", () => {
  function* naturals() { let i = 1; while (true) yield i++; }
  const firstFive = Query.from(naturals()).where((n) => n % 2 === 1).take(3).toArray();
  assertEqual(firstFive, [1, 3, 5]);
});

await run("CSV engine");
