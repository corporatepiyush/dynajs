// mini_vm.js — a tiny stack-based bytecode VM plus a compiler for a real
// arithmetic expression language (precedence, right-assoc power, unary minus,
// variables, and built-in function calls like sqrt/min/max/hypot).
//
// Pipeline: source -> tokens -> Pratt parser -> AST -> bytecode -> VM result.
//
// Engine features exercised: closures (the Pratt parser binds parse fns in a
// Map of denotations), a frozen opcode enum, a Float64Array as the operand
// stack with an explicit stack pointer (mechanical-sympathy: contiguous, no
// per-push allocation), a constant pool with de-duplication, and a
// disassembler. Self-verified against a table of expressions.

import { test, run, assert, assertEqual, assertClose, assertThrows } from "./harness.js";

// --- opcodes -----------------------------------------------------------------

const OP = Object.freeze({
  PUSH_CONST: 0, // arg = constant-pool index
  LOAD_VAR: 1,   // arg = variable-pool index
  CALL: 2,       // arg = (fnIndex << 4) | argc
  ADD: 3, SUB: 4, MUL: 5, DIV: 6, MOD: 7, POW: 8, NEG: 9,
});

const OP_NAME = Object.fromEntries(Object.entries(OP).map(([k, v]) => [v, k]));

const BUILTINS = [
  { name: "sqrt", argc: 1, fn: Math.sqrt },
  { name: "abs", argc: 1, fn: Math.abs },
  { name: "min", argc: 2, fn: Math.min },
  { name: "max", argc: 2, fn: Math.max },
  { name: "hypot", argc: 2, fn: Math.hypot },
  { name: "pow", argc: 2, fn: Math.pow },
];
const BUILTIN_INDEX = new Map(BUILTINS.map((b, i) => [b.name, i]));

// --- lexer -------------------------------------------------------------------

const TOKEN_RE =
  /\s*(?:(?<num>\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)|(?<id>[A-Za-z_]\w*)|(?<op>\*\*|[-+*/%^(),]))/y;

function* tokenize(src) {
  TOKEN_RE.lastIndex = 0;
  for (;;) {
    const start = TOKEN_RE.lastIndex;
    if (start >= src.length) break;
    const m = TOKEN_RE.exec(src); // sticky: only matches at lastIndex
    if (!m) {
      // The leading \s* consumed nothing usable. If only whitespace remains
      // it is a clean end-of-input; otherwise report the offending character.
      const rest = src.slice(start);
      if (/^\s*$/.test(rest)) break;
      throw new SyntaxError(`unexpected character ${JSON.stringify(rest.replace(/^\s*/, "")[0])}`);
    }
    const { num, id, op } = m.groups;
    if (num !== undefined) yield { type: "num", value: Number(num) };
    else if (id !== undefined) yield { type: "id", value: id };
    else if (op !== undefined) yield { type: op === "^" ? "**" : op, value: op };
  }
  yield { type: "eof", value: null };
}

// --- Pratt parser (precedence climbing) --------------------------------------

// Binary operator binding powers. Higher binds tighter. Power is right-assoc.
const BINDING = new Map([
  ["+", { lbp: 10, rbp: 11 }],
  ["-", { lbp: 10, rbp: 11 }],
  ["*", { lbp: 20, rbp: 21 }],
  ["/", { lbp: 20, rbp: 21 }],
  ["%", { lbp: 20, rbp: 21 }],
  ["**", { lbp: 30, rbp: 30 }], // right-assoc: rbp == lbp
]);

class Parser {
  #tokens;
  #pos = 0;

  constructor(src) {
    this.#tokens = [...tokenize(src)];
  }

  static parse(src) {
    const p = new Parser(src);
    const ast = p.#expr(0);
    p.#expect("eof");
    return ast;
  }

  #peek() { return this.#tokens[this.#pos]; }
  #next() { return this.#tokens[this.#pos++]; }
  #expect(type) {
    const t = this.#next();
    if (t.type !== type) throw new SyntaxError(`expected ${type}, got ${t.type}`);
    return t;
  }

  // The core precedence-climbing loop.
  #expr(minbp) {
    let left = this.#nud();
    for (;;) {
      const t = this.#peek();
      const bind = BINDING.get(t.type);
      if (!bind || bind.lbp < minbp) break;
      this.#next();
      const right = this.#expr(bind.rbp);
      left = { kind: "bin", op: t.type, left, right };
    }
    return left;
  }

  // Null denotation: literals, names, calls, unary minus, and parentheses.
  #nud() {
    const t = this.#next();
    switch (t.type) {
      case "num": return { kind: "num", value: t.value };
      case "id": {
        if (this.#peek().type === "(") return this.#call(t.value);
        return { kind: "var", name: t.value };
      }
      case "-": return { kind: "neg", operand: this.#expr(25) }; // tighter than * to bind unary
      case "+": return this.#expr(25);
      case "(": {
        const inner = this.#expr(0);
        this.#expect(")");
        return inner;
      }
      default:
        throw new SyntaxError(`unexpected ${t.type}`);
    }
  }

  #call(name) {
    this.#expect("(");
    const args = [];
    if (this.#peek().type !== ")") {
      args.push(this.#expr(0));
      while (this.#peek().type === ",") {
        this.#next();
        args.push(this.#expr(0));
      }
    }
    this.#expect(")");
    return { kind: "call", name, args };
  }
}

// --- compiler: AST -> bytecode ----------------------------------------------

class Compiler {
  #code = []; // array of { op, arg }
  #consts = [];
  #constIndex = new Map(); // dedupe identical constants
  #vars = [];
  #varIndex = new Map();

  static compile(ast) {
    const c = new Compiler();
    c.#emitNode(ast);
    return {
      code: c.#code,
      consts: Float64Array.from(c.#consts),
      vars: c.#vars,
    };
  }

  #constSlot(value) {
    if (this.#constIndex.has(value)) return this.#constIndex.get(value);
    const idx = this.#consts.push(value) - 1;
    this.#constIndex.set(value, idx);
    return idx;
  }

  #varSlot(name) {
    if (this.#varIndex.has(name)) return this.#varIndex.get(name);
    const idx = this.#vars.push(name) - 1;
    this.#varIndex.set(name, idx);
    return idx;
  }

  #emit(op, arg = 0) { this.#code.push({ op, arg }); }

  #emitNode(node) {
    switch (node.kind) {
      case "num":
        this.#emit(OP.PUSH_CONST, this.#constSlot(node.value));
        break;
      case "var":
        this.#emit(OP.LOAD_VAR, this.#varSlot(node.name));
        break;
      case "neg":
        this.#emitNode(node.operand);
        this.#emit(OP.NEG);
        break;
      case "bin":
        this.#emitNode(node.left);
        this.#emitNode(node.right);
        this.#emit(BIN_OP[node.op]);
        break;
      case "call": {
        const idx = BUILTIN_INDEX.get(node.name);
        if (idx === undefined) throw new ReferenceError(`unknown function ${node.name}`);
        const builtin = BUILTINS[idx];
        if (builtin.argc !== node.args.length) {
          throw new TypeError(`${node.name} expects ${builtin.argc} args, got ${node.args.length}`);
        }
        for (const a of node.args) this.#emitNode(a);
        this.#emit(OP.CALL, (idx << 4) | node.args.length);
        break;
      }
      default:
        throw new Error(`cannot compile node ${node.kind}`);
    }
  }
}

const BIN_OP = { "+": OP.ADD, "-": OP.SUB, "*": OP.MUL, "/": OP.DIV, "%": OP.MOD, "**": OP.POW };

// --- virtual machine ---------------------------------------------------------

class VM {
  #stack;
  #sp = 0;

  constructor(stackSize = 256) {
    // Contiguous typed-array operand stack; no per-push allocation.
    this.#stack = new Float64Array(stackSize);
  }

  #push(v) { this.#stack[this.#sp++] = v; }
  #pop() { return this.#stack[--this.#sp]; }

  run(program, env = {}) {
    this.#sp = 0;
    const { code, consts, vars } = program;
    // Resolve variable slots to a dense Float64Array once, before the hot loop.
    const varVals = new Float64Array(vars.length);
    for (let i = 0; i < vars.length; i++) {
      if (!Object.hasOwn(env, vars[i])) throw new ReferenceError(`undefined variable ${vars[i]}`);
      varVals[i] = env[vars[i]];
    }

    for (let ip = 0; ip < code.length; ip++) {
      const { op, arg } = code[ip];
      switch (op) {
        case OP.PUSH_CONST: this.#push(consts[arg]); break;
        case OP.LOAD_VAR: this.#push(varVals[arg]); break;
        case OP.NEG: this.#push(-this.#pop()); break;
        case OP.ADD: { const b = this.#pop(), a = this.#pop(); this.#push(a + b); break; }
        case OP.SUB: { const b = this.#pop(), a = this.#pop(); this.#push(a - b); break; }
        case OP.MUL: { const b = this.#pop(), a = this.#pop(); this.#push(a * b); break; }
        case OP.DIV: { const b = this.#pop(), a = this.#pop(); this.#push(a / b); break; }
        case OP.MOD: { const b = this.#pop(), a = this.#pop(); this.#push(a % b); break; }
        case OP.POW: { const b = this.#pop(), a = this.#pop(); this.#push(a ** b); break; }
        case OP.CALL: {
          const idx = arg >> 4, argc = arg & 0xf;
          const builtin = BUILTINS[idx];
          const args = new Array(argc);
          for (let k = argc - 1; k >= 0; k--) args[k] = this.#pop();
          this.#push(builtin.fn(...args));
          break;
        }
        default:
          throw new Error(`bad opcode ${op}`);
      }
    }
    if (this.#sp !== 1) throw new Error(`stack unbalanced: sp=${this.#sp}`);
    return this.#pop();
  }
}

/** Compile + run a single expression. */
export function evalExpr(src, env = {}) {
  const program = Compiler.compile(Parser.parse(src));
  return new VM().run(program, env);
}

/** Return a human-readable disassembly of the compiled program. */
export function disassemble(src) {
  const { code, consts, vars } = Compiler.compile(Parser.parse(src));
  return code
    .map(({ op, arg }, i) => {
      let detail = "";
      if (op === OP.PUSH_CONST) detail = `  ; ${consts[arg]}`;
      else if (op === OP.LOAD_VAR) detail = `  ; ${vars[arg]}`;
      else if (op === OP.CALL) detail = `  ; ${BUILTINS[arg >> 4].name}/${arg & 0xf}`;
      return `${String(i).padStart(3, "0")}  ${OP_NAME[op].padEnd(10)} ${String(arg).padStart(3)}${detail}`;
    })
    .join("\n");
}

// --- tests -------------------------------------------------------------------

test("operator precedence and associativity", () => {
  assertEqual(evalExpr("2 + 3 * 4"), 14);
  assertEqual(evalExpr("(2 + 3) * 4"), 20);
  assertEqual(evalExpr("2 ** 3 ** 2"), 512); // right-assoc: 2**(3**2)
  assertEqual(evalExpr("10 - 2 - 3"), 5); // left-assoc
  assertEqual(evalExpr("7 % 3 + 1"), 2);
});

test("unary minus binds correctly", () => {
  assertEqual(evalExpr("-2 ** 2"), -4); // -(2**2) per JS precedence emulation
  assertEqual(evalExpr("-(2) * -(3)"), 6);
  assertEqual(evalExpr("- -5"), 5);
});

test("variables via environment", () => {
  assertEqual(evalExpr("x * x + y", { x: 3, y: 1 }), 10);
  assertClose(evalExpr("a / b", { a: 1, b: 8 }), 0.125);
});

test("built-in function calls", () => {
  assertEqual(evalExpr("sqrt(16)"), 4);
  assertEqual(evalExpr("max(min(3, 9), 2)"), 3);
  assertClose(evalExpr("hypot(3, 4)"), 5);
  assertEqual(evalExpr("abs(-7) + pow(2, 10)"), 1031);
});

test("constant pool de-duplicates", () => {
  const { consts } = Compiler.compile(Parser.parse("2 * 2 + 2 * 2"));
  assertEqual(consts.length, 1); // only one distinct constant: 2
  assert(consts instanceof Float64Array);
});

test("disassembler output is stable", () => {
  const asm = disassemble("x + 1");
  assert(asm.includes("LOAD_VAR"));
  assert(asm.includes("PUSH_CONST"));
  assert(asm.includes("ADD"));
});

test("errors: unknown fn, arity, undefined var, junk", () => {
  assertThrows(() => evalExpr("frobnicate(1)"), "unknown function");
  assertThrows(() => evalExpr("min(1)"), "expects 2");
  assertThrows(() => evalExpr("x + 1"), "undefined variable");
  assertThrows(() => evalExpr("2 @ 3"), "unexpected character");
  assertThrows(() => evalExpr("2 +"), "unexpected");
});

test("stress: deep nesting stays balanced", () => {
  let expr = "1";
  for (let i = 0; i < 50; i++) expr = `(${expr} + 1)`;
  assertEqual(evalExpr(expr), 51);
});

await run("mini bytecode VM");
