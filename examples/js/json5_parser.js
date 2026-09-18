// json5_parser.js — a hand-written JSON5 tokenizer + recursive-descent parser.
//
// JSON5 (https://json5.org) is a superset of JSON that adds: comments, trailing
// commas, single-quoted and unquoted-key strings, hex numbers, leading/trailing
// decimal points, +/-Infinity, NaN, and string line-continuations.
//
// Engine features exercised: ES classes with #private fields, a `function*`
// generator lexer yielding token objects, iterator protocol, labeled tokens via
// a frozen enum, optional chaining, tagged-template-free string handling, and
// BigInt-free numeric coercion. Self-verified against a suite of inputs.

import { test, run, assert, assertEqual, assertThrows, deepEqual } from "./harness.js";

/** Token kinds. Frozen so typos throw in strict mode. */
const T = Object.freeze({
  PUNCT: "punct",
  STRING: "string",
  NUMBER: "number",
  IDENT: "ident", // true/false/null/Infinity/NaN or unquoted key
  EOF: "eof",
});

const PUNCTUATORS = new Set(["{", "}", "[", "]", ":", ","]);

/** Thrown for any malformed input, carrying a byte offset. */
class JSON5Error extends Error {
  constructor(message, index) {
    super(`${message} (at index ${index})`);
    this.name = "JSON5Error";
    this.index = index;
  }
}

/**
 * Lexer — a generator that lazily yields tokens. Whitespace and comments are
 * skipped. Keeping this a generator means the parser pulls tokens on demand and
 * we never materialise the whole token stream.
 */
class Lexer {
  #src;
  #i = 0;

  constructor(src) {
    this.#src = src;
  }

  *[Symbol.iterator]() {
    const s = this.#src;
    while (this.#i < s.length) {
      this.#skipTrivia();
      if (this.#i >= s.length) break;
      const start = this.#i;
      const c = s[this.#i];

      if (PUNCTUATORS.has(c)) {
        this.#i++;
        yield { kind: T.PUNCT, value: c, index: start };
      } else if (c === '"' || c === "'") {
        yield { kind: T.STRING, value: this.#readString(c), index: start };
      } else if (c === "-" || c === "+" || c === "." || this.#isDigit(c)) {
        yield { kind: T.NUMBER, value: this.#readNumber(), index: start };
      } else if (this.#isIdentStart(c)) {
        yield { kind: T.IDENT, value: this.#readIdent(), index: start };
      } else {
        throw new JSON5Error(`unexpected character ${JSON.stringify(c)}`, start);
      }
    }
    yield { kind: T.EOF, value: null, index: this.#i };
  }

  #skipTrivia() {
    const s = this.#src;
    for (;;) {
      const c = s[this.#i];
      if (c === " " || c === "\t" || c === "\n" || c === "\r" || c === "\f" || c === "\v" || c === " ") {
        this.#i++;
      } else if (c === "/" && s[this.#i + 1] === "/") {
        this.#i += 2;
        while (this.#i < s.length && s[this.#i] !== "\n") this.#i++;
      } else if (c === "/" && s[this.#i + 1] === "*") {
        const open = this.#i;
        this.#i += 2;
        while (this.#i < s.length && !(s[this.#i] === "*" && s[this.#i + 1] === "/")) this.#i++;
        if (this.#i >= s.length) throw new JSON5Error("unterminated block comment", open);
        this.#i += 2;
      } else {
        return;
      }
    }
  }

  #readString(quote) {
    const s = this.#src;
    let out = "";
    this.#i++; // consume opening quote
    while (this.#i < s.length) {
      const c = s[this.#i++];
      if (c === quote) return out;
      if (c === "\\") {
        out += this.#readEscape();
      } else if (c === "\n") {
        throw new JSON5Error("unescaped newline in string", this.#i - 1);
      } else {
        out += c;
      }
    }
    throw new JSON5Error("unterminated string", this.#i);
  }

  #readEscape() {
    const s = this.#src;
    const c = s[this.#i++];
    switch (c) {
      case "n": return "\n";
      case "t": return "\t";
      case "r": return "\r";
      case "b": return "\b";
      case "f": return "\f";
      case "v": return "\v";
      case "0": return "\0";
      case "\n": return ""; // line continuation
      case "\r":
        if (s[this.#i] === "\n") this.#i++;
        return "";
      case "x": {
        const hex = s.slice(this.#i, this.#i + 2);
        if (!/^[0-9a-fA-F]{2}$/.test(hex)) throw new JSON5Error("bad \\x escape", this.#i);
        this.#i += 2;
        return String.fromCharCode(parseInt(hex, 16));
      }
      case "u": {
        const hex = s.slice(this.#i, this.#i + 4);
        if (!/^[0-9a-fA-F]{4}$/.test(hex)) throw new JSON5Error("bad \\u escape", this.#i);
        this.#i += 4;
        return String.fromCharCode(parseInt(hex, 16));
      }
      default:
        return c; // \\  \/  \'  \"  etc.
    }
  }

  #readNumber() {
    const s = this.#src;
    const start = this.#i;
    // Hex literals.
    if (s[this.#i] === "0" && (s[this.#i + 1] === "x" || s[this.#i + 1] === "X")) {
      this.#i += 2;
      while (this.#i < s.length && /[0-9a-fA-F]/.test(s[this.#i])) this.#i++;
      return parseInt(s.slice(start + 2, this.#i), 16);
    }
    // Sign + Infinity/NaN handled through the ident path below when needed.
    let sign = 1;
    if (s[this.#i] === "+" || s[this.#i] === "-") {
      sign = s[this.#i] === "-" ? -1 : 1;
      this.#i++;
    }
    if (this.#matchWord("Infinity")) return sign * Infinity;
    if (this.#matchWord("NaN")) return NaN;

    while (this.#i < s.length && /[0-9.eE+\-]/.test(s[this.#i])) this.#i++;
    const text = s.slice(start, this.#i);
    const n = Number(text);
    if (Number.isNaN(n)) throw new JSON5Error(`invalid number ${JSON.stringify(text)}`, start);
    return n;
  }

  #matchWord(word) {
    if (this.#src.startsWith(word, this.#i)) {
      this.#i += word.length;
      return true;
    }
    return false;
  }

  #readIdent() {
    const s = this.#src;
    const start = this.#i;
    while (this.#i < s.length && this.#isIdentPart(s[this.#i])) this.#i++;
    return s.slice(start, this.#i);
  }

  #isDigit(c) { return c >= "0" && c <= "9"; }
  #isIdentStart(c) { return /[A-Za-z_$]/.test(c); }
  #isIdentPart(c) { return /[A-Za-z0-9_$]/.test(c); }
}

/** Recursive-descent parser driving a one-token lookahead over the Lexer. */
class Parser {
  #tokens;
  #cur;

  constructor(src) {
    this.#tokens = new Lexer(src)[Symbol.iterator]();
    this.#advance();
  }

  static parse(src) {
    const p = new Parser(src);
    const value = p.#value();
    p.#expect(T.EOF);
    return value;
  }

  #advance() {
    this.#cur = this.#tokens.next().value;
  }

  #expect(kind, value) {
    const t = this.#cur;
    if (t.kind !== kind || (value !== undefined && t.value !== value)) {
      throw new JSON5Error(`expected ${value ?? kind}, got ${t.value ?? t.kind}`, t.index);
    }
    this.#advance();
    return t;
  }

  #value() {
    const t = this.#cur;
    switch (t.kind) {
      case T.PUNCT:
        if (t.value === "{") return this.#object();
        if (t.value === "[") return this.#array();
        throw new JSON5Error(`unexpected ${t.value}`, t.index);
      case T.STRING:
        this.#advance();
        return t.value;
      case T.NUMBER:
        this.#advance();
        return t.value;
      case T.IDENT:
        return this.#keyword();
      default:
        throw new JSON5Error(`unexpected ${t.kind}`, t.index);
    }
  }

  #keyword() {
    const t = this.#expect(T.IDENT);
    switch (t.value) {
      case "true": return true;
      case "false": return false;
      case "null": return null;
      case "Infinity": return Infinity;
      case "NaN": return NaN;
      default:
        throw new JSON5Error(`unexpected identifier ${JSON.stringify(t.value)}`, t.index);
    }
  }

  #object() {
    this.#expect(T.PUNCT, "{");
    const obj = {};
    while (!(this.#cur.kind === T.PUNCT && this.#cur.value === "}")) {
      // Key: string or unquoted identifier.
      let key;
      if (this.#cur.kind === T.STRING) key = this.#expect(T.STRING).value;
      else if (this.#cur.kind === T.IDENT) key = this.#expect(T.IDENT).value;
      else throw new JSON5Error("expected object key", this.#cur.index);

      this.#expect(T.PUNCT, ":");
      obj[key] = this.#value();

      if (this.#cur.kind === T.PUNCT && this.#cur.value === ",") this.#advance();
      else break;
    }
    this.#expect(T.PUNCT, "}");
    return obj;
  }

  #array() {
    this.#expect(T.PUNCT, "[");
    const arr = [];
    while (!(this.#cur.kind === T.PUNCT && this.#cur.value === "]")) {
      arr.push(this.#value());
      if (this.#cur.kind === T.PUNCT && this.#cur.value === ",") this.#advance();
      else break;
    }
    this.#expect(T.PUNCT, "]");
    return arr;
  }
}

/** Convenience wrapper. */
export function parseJSON5(src) {
  return Parser.parse(src);
}

// --- tests -------------------------------------------------------------------

test("standard JSON still parses", () => {
  assertEqual(parseJSON5('{"a":1,"b":[2,3],"c":null}'), { a: 1, b: [2, 3], c: null });
});

test("comments are skipped", () => {
  const src = `{
    // line comment
    a: 1, /* block
    comment */ b: 2,
  }`;
  assertEqual(parseJSON5(src), { a: 1, b: 2 });
});

test("unquoted keys and single quotes", () => {
  assertEqual(parseJSON5("{ name: 'ada', role: 'eng' }"), { name: "ada", role: "eng" });
});

test("trailing commas in arrays and objects", () => {
  assertEqual(parseJSON5("[1, 2, 3,]"), [1, 2, 3]);
  assertEqual(parseJSON5("{a:1,}"), { a: 1 });
});

test("hex, signed, and special numbers", () => {
  assertEqual(parseJSON5("0xFF"), 255);
  assertEqual(parseJSON5("[.5, 2., -0.25, +3]"), [0.5, 2, -0.25, 3]);
  assertEqual(parseJSON5("-Infinity"), -Infinity);
  assert(Number.isNaN(parseJSON5("NaN")));
});

test("string escapes and line continuation", () => {
  // Raw source: 'a\nb\x41B\<newline>c'  ->  a, newline, b, 'A', 'B', <continuation>, c
  assertEqual(parseJSON5(String.raw`'a\nb\x41B\
c'`), "a\nbABc");
});

test("nested structures round-trip via deepEqual", () => {
  const src = `{
    users: [
      { id: 1, tags: ['a', 'b'], meta: { active: true } },
      { id: 2, tags: [], meta: { active: false, score: 0xA } },
    ],
    total: 2,
  }`;
  const expected = {
    users: [
      { id: 1, tags: ["a", "b"], meta: { active: true } },
      { id: 2, tags: [], meta: { active: false, score: 10 } },
    ],
    total: 2,
  };
  assert(deepEqual(parseJSON5(src), expected));
});

test("malformed inputs throw JSON5Error", () => {
  assertThrows(() => parseJSON5("{a:1"), "expected");
  assertThrows(() => parseJSON5("[1 2]"), "expected");
  assertThrows(() => parseJSON5("'unterminated"), "unterminated");
  assertThrows(() => parseJSON5("/* open"), "unterminated");
  assertThrows(() => parseJSON5("@"), "unexpected");
});

await run("JSON5 parser");
