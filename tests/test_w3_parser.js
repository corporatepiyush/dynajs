// timeout: 180
// Wave-3 engine regression suite: parser findings SEC-084 (duplicate-parameter
// and private-field quadratic checks), SEC-158 (OOM-path label/cpool index
// guards), COMPAT-115 (strict octal/non-octal diagnostics), COMPAT-116
// (embedded NUL), OPT-010 (error line/col through the shared cache).
// Timing rows use generous budgets so they discriminate quadratic
// (multi-second) from linear (sub-second) work without flaking on slow hosts.

// TIME_SCALE multiplies every wall-clock budget in this file (argv[1]). The
// sanitizer leg runs this suite under -fsanitize=address,undefined, where a
// duration threshold measures the SANITIZER rather than the parser; scaling
// keeps the QUADRATIC-vs-LINEAR discrimination these rows exist for without
// asserting a release-build number against an instrumented build. Default 1 =
// exactly the historical thresholds. The engine-side fix (work-based bounds
// instead of wall clock) is B1-15 / TEST-03 and stays with the engine lane.
const TIME_SCALE = Math.max(1, parseInt(scriptArgs[1] || "1", 10) || 1);

let n = 0, failed = 0;
function ok(c, m) { n++; if (!c) { failed++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function catches(fn) { try { fn(); return null; } catch (e) { return e; } }

// COMPAT-115: strict-mode diagnostics distinguish 08/09 from legacy octal
let e07 = catches(function () { eval('"use strict"; 07'); });
ok(e07 instanceof SyntaxError, "COMPAT-115 strict 07 is a SyntaxError");
ok(e07 !== null && /octal literals are not allowed/.test(e07.message), "COMPAT-115 strict 07 names octal literals");
let e08 = catches(function () { eval('"use strict"; 08'); });
ok(e08 instanceof SyntaxError, "COMPAT-115 strict 08 is a SyntaxError");
ok(e08 !== null && /leading zeros/.test(e08.message), "COMPAT-115 strict 08 names decimals with leading zeros");
let e09 = catches(function () { eval('"use strict"; 09'); });
ok(e09 instanceof SyntaxError && /leading zeros/.test(e09.message), "COMPAT-115 strict 09 names decimals with leading zeros");
eq(eval("07"), 7, "COMPAT-115 sloppy 07 accepted");
eq(eval("08"), 8, "COMPAT-115 sloppy 08 accepted");
eq(eval("09"), 9, "COMPAT-115 sloppy 09 accepted");
eq(eval("018"), 18, "COMPAT-115 sloppy 018 accepted");

// COMPAT-116: raw NUL outside strings/comments is refused, inside literals is data
let NUL = String.fromCharCode(0);
ok(catches(function () { eval("var a=1;" + NUL + "var b=2;"); }) instanceof SyntaxError, "COMPAT-116 embedded raw NUL is a SyntaxError");
ok(catches(function () { eval(NUL + "var a=1;"); }) instanceof SyntaxError, "COMPAT-116 leading raw NUL is a SyntaxError");
eq(eval('"a' + NUL + 'b"').length, 3, "COMPAT-116 raw NUL inside a string literal is data");
eq(eval('`a' + NUL + 'b`').length, 3, "COMPAT-116 raw NUL inside a template literal is data");
eq(eval("/a" + NUL + "b/").source.length, 3, "COMPAT-116 raw NUL inside a regexp literal is data");
eq(eval("1 // x" + NUL + "y\n+ 2"), 3, "COMPAT-116 raw NUL inside a line comment is data");
ok(catches(function () { JSON.parse('"a' + NUL + 'b"'); }) instanceof SyntaxError, "COMPAT-116 raw NUL in JSON text is refused");

// SEC-084: duplicate detection behavior is unchanged
ok(catches(function () { eval('function f(a,a){"use strict";}'); }) instanceof SyntaxError, "SEC-084 duplicate simple args");
ok(catches(function () { eval("function f(a,a=1){}"); }) instanceof SyntaxError, "SEC-084 duplicate default args");
ok(catches(function () { eval("function f({a},{a}){}"); }) instanceof SyntaxError, "SEC-084 duplicate destructured args");
ok(catches(function () { eval("function f(a,{b:a}){}"); }) instanceof SyntaxError, "SEC-084 arg vs destructured binding");
ok(catches(function () { eval("function f([a],[a=1]){}"); }) instanceof SyntaxError, "SEC-084 duplicate nested destructuring");
eq(eval("(function f(a,b=1,c){return a+b+c})(1,2,3)"), 6, "SEC-084 unique params with defaults compile");
eq(eval("(function f(a,b,c){return a+b+c})(1,2,3)"), 6, "SEC-084 plain params compile");
eq(eval("(function ({a,b},[c]){return a+b+c})({a:1,b:2},[3])"), 6, "SEC-084 destructured params compile");
eq(eval("(function(){ class C { #a = 1; m() { return this.#a; } } return new C().m(); })()"), 1, "SEC-084 private field declaration/use");

// SEC-084 timing: 60000 default-parameter names (has_parameter_expressions
// path, per-parameter duplicate scan) must stay sub-second
(function () {
    let a = [];
    for (let i = 0; i < 60000; i++) a.push("a" + i + "=" + i);
    let t = Date.now();
    new Function("return function(" + a.join(",") + "){}");
    let dt = Date.now() - t;
    ok(dt < TIME_SCALE * 2000, "SEC-084 60000 default parameters compile in < " + (TIME_SCALE * 2000) + "ms (got " + dt + "ms)");
})();

// SEC-084 timing: 30000 private fields in one class must stay sub-second
// TIME_SCALE scales both timing rows so a SANITIZER leg (several times slower,
// and measuring the sanitizer rather than the engine) does not assert a
// release-build duration against itself. The engine-side fix -- work-based
// bounds instead of wall clock -- is B1-15 / TEST-03 and stays with the
// engine lane.
(function () {
    let a = [];
    for (let i = 0; i < 30000; i++) a.push("#f" + i + ";");
    let t = Date.now();
    new Function("class C { " + a.join("") + " }");
    let dt = Date.now() - t;
    ok(dt < TIME_SCALE * 700, "SEC-084 30000 private fields compile in < " + (TIME_SCALE * 700) + "ms (got " + dt + "ms)");
})();

// SEC-158: the guarded label/cpool paths still compile and run every shape
eq(eval("(function(){ switch (1) { case 0: return 'a'; case 1: return 'b'; default: return 'c'; } })()"), "b", "SEC-158 switch fallthrough/default");
eq(eval("(function(){ let s=0; let i=0; do { s+=i; i++; } while (i<4); return s; })()"), 6, "SEC-158 do-while");
eq(eval("(function(){ let s=0; for (let i=0;i<4;i++) s+=i; return s; })()"), 6, "SEC-158 for");
eq(eval("(function(){ let s=0; for (const x of [1,2,3]) s+=x; return s; })()"), 6, "SEC-158 for-of");
eq(eval("(function(){ let a; ({a=5}={}); return a; })()"), 5, "SEC-158 destructuring default");
eq(eval("(function(){ class C { #a; static { this.b = 1; } m() { return this?.x; } } return typeof new C().m(); })()"), "undefined", "SEC-158 class fields/static block/optional chain");
eq(eval("(function(){ try { null.x; } catch (e) { return e instanceof TypeError; } })()"), true, "SEC-158 try/catch");
eq(eval("(async function(){ let s=0; for await (const x of [Promise.resolve(1),2]) s+=x; return s; })() instanceof Promise"), true, "SEC-158 for-await-of compiles");

// OPT-010: error positions flow through the shared line/col cache
(function () {
    let filler = "";
    for (let i = 0; i < 5000; i++) filler += "var x" + i + "=1;\n";
    let e = catches(function () { new Function(filler + "function f(){}\n" + "var = 2;"); });
    ok(e instanceof SyntaxError, "OPT-010 late error is a SyntaxError");
    let m = e === null ? null : /<input>:(\d+):(\d+)/.exec(String(e.stack));
    ok(m !== null && Number(m[1]) === 5004 && Number(m[2]) === 5, "OPT-010 line:col after a compiled function (got " + (m ? m[0] : "no stack position") + ")");
    let e2 = catches(function () { new Function(filler + "var = 2;"); });
    let m2 = e2 === null ? null : /<input>:(\d+):(\d+)/.exec(String(e2.stack));
    ok(m2 !== null && Number(m2[1]) === 5003 && Number(m2[2]) === 5, "OPT-010 line:col without a nested function (got " + (m2 ? m2[0] : "no stack position") + ")");
})();

setTimeout(function () {
    if (n !== 0) print("w3_parser: " + n + " checks, " + failed + " failures");
    if (failed !== 0) throw new Error("w3_parser: " + failed + " failures");
    print("w3_parser: OK (" + n + " checks)");
}, 20);
