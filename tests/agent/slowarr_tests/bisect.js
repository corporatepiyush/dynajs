function mk(name) {
    let a;
    if (name === "dense") { a = []; for (let i = 0; i < 8; i++) a.push(i * 10); }
    else if (name === "denseSlow") { a = []; for (let i = 0; i < 8; i++) a.push(i*10); delete a[3]; a[3] = 30; }
    else if (name === "holeyMid") { a = []; for (let i = 0; i < 8; i++) a.push(i*10); delete a[3]; }
    else if (name === "verySparse") { a = []; a[1000000] = 7; }
    else if (name === "holeyStart") { a = []; for (let i = 0; i < 8; i++) a.push(i*10); delete a[0]; }
    else if (name === "holeyEnd") { a = []; for (let i = 0; i < 8; i++) a.push(i*10); delete a[7]; }
    else if (name === "allHoles") { a = new Array(5); }
    else if (name === "withUndef") { a = []; for (let i = 0; i < 8; i++) a.push(i*10); a[3] = undefined; }
    return a;
}
const op = scriptArgs[1], shape = scriptArgs[2];
for (let i = 0; i < 2000; i++) {
    let a = mk(shape);
    if (op === "shift") a.shift();
    else if (op === "unshift") a.unshift("u");
    else if (op === "unshift2") a.unshift("u1", "u2");
    else if (op === "pop") a.pop();
    else if (op === "reverse") a.reverse();
    else if (op === "slice") a.slice();
    else if (op === "concat") a.concat([1]);
    else if (op === "inc") a.includes(30);
    else if (op === "iof") a.indexOf(30);
}
print("OK " + op + " " + shape);
