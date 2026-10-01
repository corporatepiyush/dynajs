function __EV(name, fn) { test(name, fn); }
function __say(s) { (typeof print === "function" ? print : console.log)(s); }
function __res(id, log) {
  var o = {};
  o.id = id;
  o[Symbol.dispose] = function () { log.push("dispose:" + this.id + ":" + (this === o)); };
  return o;
}

__EV("script-top-level-rejected", function () {
  assert_throws(function () { (0, eval)("{ using x = null; } using y = null;"); }, "SyntaxError");
});

__EV("block-level-allowed", function () {
  var ok = (0, eval)('{ using x = null; } true;');
  assert(ok === true, "block-level using must parse");
});

__EV("await-using-outside-async-rejected", function () {
  assert_throws(function () { (0, eval)("function f(){ await using x = null; }"); }, "SyntaxError");
});

__EV("using-requires-initializer", function () {
  assert_throws(function () { (0, eval)("function f(){ using x; }"); }, "SyntaxError");
});

__EV("single-statement-context-rejected", function () {
  assert_throws(function () { (0, eval)("if (1) using x = null;"); }, "SyntaxError");
});

__EV("labelled-statement-rejected", function () {
  assert_throws(function () { (0, eval)("lbl: using x = null;"); }, "SyntaxError");
});

__EV("line-terminator-makes-it-expression", function () {
  var f = new Function("var x = 'unset'; try { using\nx = 1; } catch (e) {} return typeof x;");
  assert_eq(f(), "string");
});

__EV("using-as-identifier-still-works", function () {
  var using = 5;
  assert_eq(using + 1, 6);
  var o = { using: 7 };
  assert_eq(o.using, 7);
});

__EV("for-in-head-rejected", function () {
  assert_throws(function () { (0, eval)("for (using x in {}) {}"); }, "SyntaxError");
});

__EV("redeclaration-in-block-rejected", function () {
  assert_throws(function () { (0, eval)("{ using a = null; using a = null; }"); }, "SyntaxError");
});

__EV("var-conflicts-with-using-rejected", function () {
  assert_throws(function () { (0, eval)("function f(){ using a = null; var a; }"); }, "SyntaxError");
});

__EV("for-using-of-of-reads-using-as-target", function () {
  var err = __mustThrow(function () { (0, eval)("for (using of of [1]) {}"); });
  assert(err, "expected ReferenceError");
  assert_eq(err.constructor.name, "ReferenceError");
});

summary("using_grammar_matrix");
