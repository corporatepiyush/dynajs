import { readFile, writeFile } from "dyna:file";
import { Path } from "dyna:file";
import { args } from "dyna:sys";
import * as c from "dyna:crypto";

const dir = args()[2] || ".";

function hex(u8) {
    let s = "";
    for (const b of u8) s += b.toString(16).padStart(2, "0");
    return s;
}
const P = (x) => new Path(x);
const out = {};
for (const alg of ["Ed", "X"]) {
    const pre = alg === "Ed" ? "Ed25519" : "X25519";
    const privPem = readFile(P(dir + "/" + (alg === "Ed" ? "ed_priv.pem" : "x_priv.pem")));
    const pubPem = readFile(P(dir + "/" + (alg === "Ed" ? "ed_pub.pem" : "x_pub.pem")));
    const fromPriv = c[pre + "PemToRaw"](privPem);
    const fromPub = c[pre + "PemToRaw"](pubPem);
    out[alg + "_privRaw"] = hex(fromPriv.privateKey);
    out[alg + "_pubRaw"] = hex(fromPriv.publicKey);
    out[alg + "_pubOnlyHasNoPriv"] = fromPub.privateKey === undefined;
    out[alg + "_pubOnlyRaw"] = hex(fromPub.publicKey);
    const back = c[pre + "PemFromRaw"]({ privateKey: fromPriv.privateKey, publicKey: fromPriv.publicKey });
    writeFile(P(dir + "/" + alg + "_rt_priv.pem"), back.privateKey);
    writeFile(P(dir + "/" + alg + "_rt_pub.pem"), back.publicKey);
}
print(JSON.stringify(out));
