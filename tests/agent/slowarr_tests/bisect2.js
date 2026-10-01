const op = scriptArgs[1], shape = scriptArgs[2];
const SYM = Symbol("s");
function mk(n) {
    let a;
    if (n === "getter") {
        a = []; for (let i = 0; i < 8; i++) a.push(i * 10);
        Object.defineProperty(a, 3, { get() { return 999; }, configurable: true });
        return a;
    }
    if (n === "withSym") { a = []; for (let i = 0; i < 8; i++) a.push(i*10); a[3] = SYM; a[4] = BigInt(90); return a; }
    if (n === "frozen") { a = []; for (let i = 0; i < 8; i++) a.push(i*10); delete a[3]; return Object.freeze(a); }
    if (n === "sealed") { a = []; for (let i = 0; i < 8; i++) a.push(i*10); delete a[3]; return Object.seal(a); }
    if (n === "sub") { let s = new Sub(); for (let i = 0; i < 5; i++) s.push(i*10); delete s[3]; return s; }
    if (n === "allHoles") return new Array(5);
    if (n === "mixedW") {
        a = []; for (let i = 0; i < 8; i++) a.push(i*10); delete a[3]; a[3] = 30;
        Object.defineProperty(a, 1, { writable: false });
        return a;
    }
    return "bad";
}
class Sub extends Array {}
for (let i = 0; i < 500; i++) {
    let a = mk(shape);
    if (op === "shift") a.shift();
    else if (op === "unshift2") a.unshift("u1", "u2");
    else if (op === "reverse") a.reverse();
    else if (op === "slice") a.slice();
    else if (op === "concat") a.concat([1]);
    else if (op === "iof") a.indexOf(SYM);
    else if (op === "inc") a.includes(30);
    else if (op === "liof") a.lastIndexOf(30);
}
print("OK " + op + " " + shape);
