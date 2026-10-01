import { fff1 } from "./fff1.so";
var mod = { f: fff1 };
function callImm(m) {
    return m.f(2);
}
console.log(callImm(mod));
