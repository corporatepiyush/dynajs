// timeout: 180
// Split from tests/test_data_w3_fixes.js: the structures sections.
import { TOML } from "dyna:config";
import { Graph } from "dyna:structures";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { check(a === b, m + " -- got " + JSON.stringify(a) + ", want " + JSON.stringify(b)); }
async function throws(fn, m) { n++; try { await fn(); fails++; print("FAIL: " + m + " (did not throw)"); } catch (e) { } }
function peq(text, expected, m) {
    n++;
    let got;
    try { got = TOML.parse(text).s; }
    catch (e) { fails++; print("FAIL: " + m + " (threw " + e + ")"); return; }
    check(got === expected, m + " -- got " + JSON.stringify(got) + ", want " + JSON.stringify(expected));
}
{
    const g = new Graph();
    for (let i = 0; i < 9; i++) g.addNode();
    for (const [a, b, w] of [[0,1,9],[1,2,6],[2,3,7],[1,4,5],[3,5,8],[3,6,7],[0,7,4],[5,8,5],[0,6,6]])
        g.addEdge(a, b, w);
    const hv = [0.5,0.75,0.5,0.25,0,0.75,1,1,0];
    const hd = [];
    for (let x = 0; x < 9; x++) hd.push(g.dijkstra(x, 8));
    const r = g.aStar(0, 8, (x) => hv[x] * hd[x]);
    eq(r.dist, g.dijkstra(0, 8),
       "OPT-055: aStar reopens closed nodes (admissible but inconsistent heuristic)");
}
if (fails === 0) print("test_structures_data_w3_fixes: all " + n + " checks passed");
else print("test_structures_data_w3_fixes: " + fails + " FAILED of " + n);
