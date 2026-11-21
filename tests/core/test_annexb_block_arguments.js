// covers: integration c:src/parser/parser.inc.c -- Annex B block function named `arguments` overrides the arguments binding when evaluated
// ECMA-262 FunctionDeclarationInstantiation step 32: "arguments" only skips the entry var; evaluation still binds it (hoisted var reuses arguments_var_idx).
// annexB/block-decl-func-skip-arguments.js tests superseded "Append arguments to parameterNames" wording; it is a known test262 error.
import { suite } from "../lib/kit.js";

const t = suite("annexb_block_arguments");
const fn = (src, ...args) => new Function(src)(...args);
const fnp = (params, src, ...args) => new Function(params, src)(...args);
const BODY = `const a = typeof arguments; { function arguments() {} } return a + "," + typeof arguments;`;

t.test("the block function overrides arguments after the block", (a) => {
    a.eq(fn(BODY), "object,function", "no parameters");
    a.eq(fnp("x", BODY, 1), "object,function", "simple parameter");
    a.eq(fnp("...x", BODY, 1), "object,function", "rest parameter");
    a.eq(fnp("x = 1", BODY), "object,function", "default parameter");
    a.eq((0, eval)(`(function test(arg) { eval(arg); { function arguments() { return 1; } } return typeof arguments; })("42")`), "function", "regress-602621 shape");
});

t.test("the arguments object is intact before the block runs", (a) => {
    a.eq(fn(`const v = arguments[0]; { function arguments() {} } return v;`, 7), 7, "first argument");
    a.eq(fn(`const n = arguments.length; { function arguments() {} } return n;`, 1, 2, 3), 3, "length");
    a.eq(fn(`const s = Object.prototype.toString.call(arguments); { function arguments() {} } return s;`), "[object Arguments]", "class");
    a.eq(fn(`if (false) { function arguments() {} } return typeof arguments;`), "object", "declaration never evaluated");
    a.eq(fn(`{ const b = typeof arguments; function arguments() {} return b; }`), "function", "inside the block it is the function");
});

t.test("mapped parameters stay independent of the hoisted binding", (a) => {
    a.eq(fnp("x", `{ function arguments() {} } x = 9; return typeof arguments + "," + x;`, 1), "function,9", "parameter write");
    a.eq(fnp("arguments", `{ function arguments() {} } return typeof arguments;`, 5), "number", "parameter named arguments is not overridden");
    a.eq(fn(`var arguments; { function arguments() {} } return typeof arguments;`), "function", "explicit var arguments");
    a.eq(fnp("a = () => arguments", `const b = typeof a(); { function arguments() {} } return b + "," + typeof arguments + "," + typeof a();`), "object,function,object", "parameter scope closure keeps the object");
});

t.test("strict code rejects a function named arguments", (a) => {
    let name = "none";
    try { fn(`"use strict"; { function arguments() {} }`); } catch (e) { name = e.name; }
    a.eq(name, "SyntaxError", "strict body");
});

await t.run();
