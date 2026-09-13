// PGO training: module-heavy program. Imports a fan of 24 generated modules
// (generated inline by gen_train_modules.js at tree-prep time into mod_*.js)
// and drives every exported function so the module loader, import machinery,
// cross-module calls and module-level closures all land in the profile.
import { fib, ackermann as ack1 } from "./mod_a.js";
import { qsort, lcg } from "./mod_b.js";
import { rot13, piglatin } from "./mod_c.js";
import { Matrix as M1, matmul } from "./mod_d.js";
import { LRUCache } from "./mod_e.js";
import { parseCsv } from "./mod_f.js";
import { tokenizer } from "./mod_g.js";
import { bignumMul } from "./mod_h.js";
import { dateFmt } from "./mod_i.js";
import { regexpDrive } from "./mod_j.js";
import { protoChain } from "./mod_k.js";
import { generators } from "./mod_l.js";
import { promises } from "./mod_m.js";
import { maps } from "./mod_n.js";
import { symbols } from "./mod_o.js";
import { getters } from "./mod_p.js";
import { classes } from "./mod_q.js";
import { typed } from "./mod_r.js";
import { jsonDrive } from "./mod_s.js";
import { errorPaths } from "./mod_t.js";
import { stringBuild } from "./mod_u.js";
import { sortDrive } from "./mod_v.js";
import { tailRec } from "./mod_w.js";
import { iterators } from "./mod_x.js";

let sink = 0;
const t0 = Date.now();

sink += fib(22);
sink += ack1(2, 3);
const arr = lcg(20000, 12345);
sink += qsort(arr.slice(0, 5000)).length;
sink += rot13("The quick brown fox jumps over the lazy dog, x2!").length;
sink += piglatin("pGO training modules exercise import machinery").length;
sink += matmul(new M1(24), new M1(24)).trace();
const cache = new LRUCache(512);
for (let i = 0; i < 20000; i++) cache.put(i % 800, i);
for (let i = 0; i < 20000; i++) sink += cache.get(i % 800) | 0;
sink += parseCsv("a,b,c\n1,2,3\n4,5,6\n".repeat(400)).rows.length;
const toks = tokenizer("for (let i = 0; i < 100; i++) { sum += arr[i]; }".repeat(300));
sink += toks.count;
sink += bignumMul("98765432109876543210", "12345678901234567890");
sink += dateFmt(new Date(Date.UTC(2026, 8, 11)), 5000);
sink += regexpDrive("abababXabab abc abcabc 12345 abcdef", 3000);
sink += protoChain(4000);
sink += generators(3000);
sink += promises();
sink += maps(8000);
sink += symbols(2000);
sink += getters(6000);
sink += classes(4000);
sink += typed(20000);
sink += jsonDrive('{"k":[1,2,3],"s":"str","o":{"n":null}}', 4000);
sink += errorPaths(3000);
sink += stringBuild(4000);
sink += sortDrive(12000);
sink += tailRec(600);
sink += iterators(8000);

const t1 = Date.now();
console.log("train_modules ok: sink=" + sink + " ms=" + (t1 - t0));
