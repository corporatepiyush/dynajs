// parser_combinators.js — a small parser-combinator library and two grammars
// built with it: an arithmetic calculator (with precedence) and a JSON-subset
// parser. Includes a tagged-template `rule` for writing grammar productions
// with interpolated sub-parsers.
//
// A Parser is just a pure function `(State) -> Result`. Combinators are
// higher-order functions that take parsers and return new parsers, so grammars
// compose out of tiny pieces.
//
// Engine features exercised: closures / higher-order functions, tagged template
// literals, sticky RegExps, and recursion via a `lazy` combinator.

import { test, run, assert, assertEqual, assertThrows, deepEqual } from "./harness.js";

/**
 * @typedef {{ input: string, pos: number }} State
 * @typedef {{ ok: true, value: any, pos: number } |
 *           { ok: false, pos: number, expected: string }} Result
 * @typedef {(state: State) => Result} Parser
 */

const ok = (value, pos) => ({ ok: true, value, pos });
const fail = (pos, expected) => ({ ok: false, pos, expected });

/** Run a parser over a whole string, requiring it to consume everything. */
export function parse(parser, input) {
  const r = seq(parser, eof)({ input, pos: 0 });
  if (!r.ok) {
    const { line, col } = lineCol(input, r.pos);
    throw new SyntaxError(`expected ${r.expected} at ${line}:${col} (offset ${r.pos})`);
  }
  return r.value[0];
}

function lineCol(input, pos) {
  let line = 1, col = 1;
  for (let i = 0; i < pos && i < input.length; i++) {
    if (input[i] === "\n") { line++; col = 1; } else col++;
  }
  return { line, col };
}

// --- primitive parsers -------------------------------------------------------

/** Match an exact string. */
export const str = (s) => (st) =>
  st.input.startsWith(s, st.pos) ? ok(s, st.pos + s.length) : fail(st.pos, JSON.stringify(s));

/** Match a sticky RegExp, returning the matched text (or a chosen group). */
export const regex = (re, group = 0, label = re.source) => {
  const sticky = new RegExp(re.source, re.flags.includes("y") ? re.flags : re.flags + "y");
  return (st) => {
    sticky.lastIndex = st.pos;
    const m = sticky.exec(st.input);
    return m ? ok(m[group], st.pos + m[0].length) : fail(st.pos, label);
  };
};

/** Succeeds only at end of input. */
export const eof = (st) =>
  st.pos >= st.input.length ? ok(null, st.pos) : fail(st.pos, "end of input");

// --- combinators -------------------------------------------------------------

/** Transform a parser's successful value. */
export const map = (p, fn) => (st) => {
  const r = p(st);
  return r.ok ? ok(fn(r.value), r.pos) : r;
};

/** Run parsers in sequence; value is the array of their values. */
export const seq = (...ps) => (st) => {
  const values = [];
  let pos = st.pos;
  for (const p of ps) {
    const r = p({ input: st.input, pos });
    if (!r.ok) return r;
    values.push(r.value);
    pos = r.pos;
  }
  return ok(values, pos);
};

/** Ordered choice: first parser that succeeds wins; reports furthest failure. */
export const alt = (...ps) => (st) => {
  let furthest = fail(st.pos, "nothing");
  for (const p of ps) {
    const r = p(st);
    if (r.ok) return r;
    if (r.pos >= furthest.pos) furthest = r;
  }
  return furthest;
};

/** Zero or more; never fails (returns []). */
export const many = (p) => (st) => {
  const values = [];
  let pos = st.pos;
  for (;;) {
    const r = p({ input: st.input, pos });
    if (!r.ok) break;
    if (r.pos === pos) break; // guard against non-consuming infinite loops
    values.push(r.value);
    pos = r.pos;
  }
  return ok(values, pos);
};

export const many1 = (p) => map(seq(p, many(p)), ([first, rest]) => [first, ...rest]);

export const opt = (p, fallback = null) => (st) => {
  const r = p(st);
  return r.ok ? r : ok(fallback, st.pos);
};

/** p separated by `sep`; returns array of p-values (possibly empty). */
export const sepBy = (p, sep) =>
  opt(map(seq(p, many(map(seq(sep, p), ([, v]) => v))), ([first, rest]) => [first, ...rest]), []);

/** open p close -> value of p. */
export const between = (open, p, close) => map(seq(open, p, close), ([, v]) => v);

/** Defer construction for recursive grammars. */
export const lazy = (thunk) => {
  let cached;
  return (st) => (cached ??= thunk())(st);
};

/** Left-associative binary operator chain (avoids left recursion). */
export const chainl1 = (term, op) => (st) => {
  let left = term(st);
  if (!left.ok) return left;
  let pos = left.pos;
  let acc = left.value;
  for (;;) {
    const o = op({ input: st.input, pos });
    if (!o.ok) break;
    const right = term({ input: st.input, pos: o.pos });
    if (!right.ok) break;
    acc = o.value(acc, right.value);
    pos = right.pos;
  }
  return ok(acc, pos);
};

/** Skip surrounding ASCII whitespace around a parser. */
const ws = regex(/[ \t\r\n]*/, 0, "whitespace");
export const token = (p) => between(ws, p, ws);

// --- tagged-template grammar rules ------------------------------------------

/**
 * A tagged template for writing a production as literal tokens interleaved with
 * interpolated sub-parsers. Literal text is matched as whitespace-delimited
 * tokens (discarded); the result is the array of interpolated parsers' values.
 *
 *   const parenExpr = rule`( ${expr} )`;   // parses "( <expr> )" -> [exprValue]
 */
export function rule(strings, ...parsers) {
  const parts = [];
  strings.forEach((lit, i) => {
    for (const symbol of lit.trim().split(/\s+/).filter(Boolean)) {
      parts.push({ kind: "lit", parser: token(str(symbol)) });
    }
    if (i < parsers.length) parts.push({ kind: "hole", parser: parsers[i] });
  });
  const combined = seq(...parts.map((p) => p.parser));
  return map(combined, (values) =>
    values.filter((_, i) => parts[i].kind === "hole"),
  );
}

// --- grammar 1: arithmetic calculator ---------------------------------------

const number = token(map(regex(/-?\d+(?:\.\d+)?/, 0, "number"), Number));
const addOp = token(alt(map(str("+"), () => (a, b) => a + b), map(str("-"), () => (a, b) => a - b)));
const mulOp = token(alt(map(str("*"), () => (a, b) => a * b), map(str("/"), () => (a, b) => a / b)));

const expr = lazy(() => chainl1(term, addOp));
const term = lazy(() => chainl1(factor, mulOp));
const factor = lazy(() => alt(number, map(rule`( ${expr} )`, ([v]) => v)));

export const calculate = (src) => parse(expr, src);

// --- grammar 2: JSON subset --------------------------------------------------

const jsonValue = lazy(() =>
  token(alt(
    map(str("true"), () => true),
    map(str("false"), () => false),
    map(str("null"), () => null),
    number,
    jsonString,
    jsonArray,
    jsonObject,
  )));

const jsonString = map(
  regex(/"(?:[^"\\]|\\.)*"/, 0, "string"),
  (s) => JSON.parse(s), // reuse the engine for escape handling
);

const jsonArray = map(
  seq(token(str("[")), sepBy(jsonValue, token(str(","))), token(str("]"))),
  ([, items]) => items,
);

const jsonPair = map(seq(token(jsonString), token(str(":")), jsonValue), ([k, , v]) => [k, v]);

const jsonObject = map(
  seq(token(str("{")), sepBy(jsonPair, token(str(","))), token(str("}"))),
  ([, pairs]) => Object.fromEntries(pairs),
);

export const parseJSON = (src) => parse(jsonValue, src);

// --- tests -------------------------------------------------------------------

test("primitive parsers", () => {
  assertEqual(parse(str("hello"), "hello"), "hello");
  assertEqual(parse(regex(/\d+/), "12345"), "12345");
  assertThrows(() => parse(str("hi"), "bye"), /expected/);
});

test("calculator respects precedence and parentheses", () => {
  assertEqual(calculate("1 + 2 * 3"), 7);
  assertEqual(calculate("(1 + 2) * 3"), 9);
  assertEqual(calculate("10 - 2 - 3"), 5); // left-assoc
  assertEqual(calculate("2 * 3 + 4 * 5"), 26);
  assertEqual(calculate(" ( 1 + ( 2 * ( 3 + 4 ) ) ) "), 15);
  assertEqual(calculate("-3 + 4"), 1);
});

test("calculator reports errors with position", () => {
  assertThrows(() => calculate("1 + "), /expected .* at 1:/);
  assertThrows(() => calculate("(1 + 2"), /expected/);
  assertThrows(() => calculate("1 2"), /expected end of input/);
});

test("tagged-template rule extracts holes", () => {
  const braced = rule`{ ${number} }`;
  assertEqual(parse(braced, "{ 42 }"), [42]);
  const pair = rule`< ${number} , ${number} >`;
  assertEqual(parse(pair, "<1,2>"), [1, 2]);
});

test("JSON subset parses nested structures", () => {
  assertEqual(parseJSON("true"), true);
  assertEqual(parseJSON("  -12.5 "), -12.5);
  assertEqual(parseJSON('"a\\nb"'), "a\nb");
  assertEqual(parseJSON("[1, 2, [3, 4]]"), [1, 2, [3, 4]]);
  assert(deepEqual(
    parseJSON('{ "name": "ada", "nums": [1, 2, 3], "ok": true, "meta": null }'),
    { name: "ada", nums: [1, 2, 3], ok: true, meta: null },
  ));
  assertEqual(parseJSON("[]"), []);
  assertEqual(parseJSON("{}"), {});
});

test("sepBy handles empty and singleton lists", () => {
  const csv = sepBy(number, token(str(",")));
  assertEqual(parse(csv, ""), []);
  assertEqual(parse(csv, "5"), [5]);
  assertEqual(parse(csv, "1, 2, 3"), [1, 2, 3]);
});

await run("parser combinators");
