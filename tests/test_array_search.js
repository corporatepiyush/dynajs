function eq(a, b, m) {
    if (a !== b) {
        print("FAIL " + m + ": got " + String(a) + " want " + String(b));
        throw new Error(m);
    }
}

var A = [1, 2, 3, 4, 5];
eq(A.indexOf(3), 2, "idx int");
eq(A.indexOf(9), -1, "idx miss");
eq(A.indexOf(3, 3), -1, "idx fromIndex past");
eq(A.indexOf(3, -3), 2, "idx negative fromIndex");
eq(A.lastIndexOf(3), 2, "last int");
eq(A.lastIndexOf(9), -1, "last miss");
eq(A.lastIndexOf(3, 1), -1, "last fromIndex");
eq(A.lastIndexOf(3, -1), 2, "last negative fromIndex");
eq(A.includes(3), true, "inc int");
eq(A.includes(9), false, "inc miss");

eq([1, 2.0, 3].indexOf(2), 1, "int needle float elem");
eq([1, 2, 3].indexOf(2.0), 1, "float needle int elem");
eq([1.5, 2.5].indexOf(2.5), 1, "float needle float elem");
eq([1, 2.0, 3].lastIndexOf(2), 1, "last int needle float elem");
eq([1, 2.0, 3].includes(2), true, "inc int needle float elem");
eq([1e300, 2].indexOf(1e300), 0, "big float");

eq([1, 2, 3].indexOf("2"), -1, "no string coercion");
eq([1, 2, 3].includes("2"), false, "inc no string coercion");
eq(["1", "2"].indexOf(2), -1, "no number coercion");
eq([true, false].indexOf(1), -1, "no bool coercion");

eq([NaN].indexOf(NaN), -1, "NaN strict");
eq([NaN].lastIndexOf(NaN), -1, "NaN strict last");
eq([NaN].includes(NaN), true, "NaN SameValueZero");
eq([1, NaN, 3].includes(NaN), true, "NaN SameValueZero mid");
eq([0].indexOf(-0), 0, "+0 finds -0");
eq([-0].indexOf(0), 0, "-0 finds +0");
eq([-0].includes(0), true, "inc -0 finds +0");
eq([0].lastIndexOf(-0), 0, "last +0 finds -0");

var o = {};
eq([o, {}].indexOf(o), 0, "object identity");
eq([{}].indexOf({}), -1, "object non-identity");
eq([null, undefined].indexOf(undefined), 1, "undefined");
eq([null, undefined].indexOf(null), 0, "null");
eq([true, false].indexOf(false), 1, "bool");
eq(["a", "b"].indexOf("b"), 1, "string");
eq(["a", "b"].indexOf("b".toUpperCase().toLowerCase()), 1, "string by value");
eq([1n, 2n].indexOf(2n), 1, "bigint");
eq([1n].indexOf(1), -1, "bigint vs int");
var s = Symbol("x");
eq([s, Symbol("x")].indexOf(s), 0, "symbol identity");

eq([2147483647, -2147483648].indexOf(-2147483648), 1, "int32 min");
eq([2147483647, -2147483648].indexOf(2147483647), 0, "int32 max");
eq([2147483648].indexOf(2147483648), 0, "beyond int32 (float)");

var M = [1, "1", 1.0, true, null, undefined, {}, 1n, 2];
eq(M.indexOf(2), 8, "mixed find int at end");
eq(M.indexOf("1"), 1, "mixed find string");
eq(M.lastIndexOf(1), 2, "mixed last int matches float");
eq(M.includes(2), true, "mixed includes");

var sparse = [1, , 3];
eq(sparse.indexOf(3), 2, "sparse");
eq(sparse.lastIndexOf(3), 2, "sparse last");
eq(sparse.indexOf(undefined), -1, "sparse hole is not undefined");
var getter = [1, 2, 3];
Object.defineProperty(getter, 1, { get: function () { return 42; }, configurable: true });
eq(getter.indexOf(42), 1, "getter array");
eq(getter.lastIndexOf(42), 1, "getter array last");
var big = [1, 2, 3];
big[100] = 9;
eq(big.indexOf(9), 100, "sparse tail");
eq(big.lastIndexOf(9), 100, "sparse tail last");

eq(Array.prototype.indexOf.call({ 0: "a", 1: "b", length: 2 }, "b"), 1, "arraylike");
eq(Array.prototype.lastIndexOf.call({ 0: "a", 1: "b", length: 2 }, "a"), 0, "arraylike last");
eq(Array.prototype.includes.call({ 0: 5, length: 1 }, 5), true, "arraylike includes");
var al = [1, 2, 3];
Object.defineProperty(al, "length", { value: 5, writable: true });
eq(al.lastIndexOf(3), 2, "length beyond dense");
eq(al.indexOf(3), 2, "length beyond dense idx");

eq([].indexOf(1), -1, "empty");
eq([].lastIndexOf(1), -1, "empty last");
eq([].includes(1), false, "empty includes");
eq([7].indexOf(7), 0, "single");
eq([7].lastIndexOf(7), 0, "single last");

var mut = [1, 2, 3, 4];
var idx = { valueOf: function () { mut.length = 2; return 0; } };
eq(mut.indexOf(3, idx), -1, "mutating fromIndex");

print("array search: ok");
