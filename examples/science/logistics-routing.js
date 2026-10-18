// Logistics routing — a delivery-planning engine: a road network with speed classes, shortest-path travel-time
// matrices, a capacitated vehicle routing problem with time windows solved by Clarke–Wright savings and improved
// by 2-opt, relocate and swap local search, schedule simulation with ETAs, a lower bound, GeoJSON output and a
// dispatch API that re-plans when an order is added.
//
// WHAT IT IS
//   network      a city road graph: a grid of local streets (25 km/h), arterials (50 km/h) and a ring motorway
//                (90 km/h), some links closed; travel times from Dijkstra per depot/customer node, routes rebuilt
//                with A* for the turn-by-turn path
//   problem      80 deliveries with weights, service times and delivery windows (morning / afternoon / anytime),
//                a fleet of vans with capacity, a shift length and a cost per km and per hour
//   construction Clarke–Wright savings: start from one route per customer, merge route ends in order of savings
//                when the merged route stays feasible (capacity, windows, shift)
//   improvement  local search to a local optimum: 2-opt inside routes, relocate a stop to another route, swap
//                stops between routes — every move re-checked for feasibility
//   evaluation   distance, duration, cost (Decimal), waiting time, utilisation, on-time rate; a lower bound
//                (sum of each customer's cheapest in/out edge halves) to state the optimality gap honestly
//   output       GeoJSON FeatureCollection of routes and stops, a driver manifest, and POST /orders to insert a
//                late order with a cheapest-insertion re-plan
//   granular     relocate and swap only consider moving a stop next to one of its 10 nearest customers (a KDTree
//                knn over drop-off coordinates) — the "granular neighbourhood" of Toth & Vigo, which prunes most
//                pointless moves; the search itself prices routes in floating point and only the reported money
//                goes through Decimal
//
// MODULES  dyna:structures (Graph, KDTree) · dyna:decimal · dyna:http (App) · dyna:random · dyna:cli (Table)
//          · dyna:dataframe · dyna:json · dyna:time (formatUnix)
//
// RUN      dynajs examples/science/logistics-routing.js     plans tomorrow's deliveries and inserts a late order

import { Graph, KDTree } from "dyna:structures";
import { Decimal } from "dyna:decimal";
import { App } from "dyna:http";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";
import { DataFrame } from "dyna:dataframe";
import { Pointer as JSONPointer } from "dyna:json";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const rng = new Random(1959);

// =============================================================================
// Road network
// =============================================================================
const G = 24,
    CELL = 0.5; // 24x24 grid, 500 m blocks
const graph = new Graph({ directed: true, weighted: true }),
    coords = [];
for (let y = 0; y < G; y++)
    for (let x = 0; x < G; x++) {
        graph.addNode();
        coords.push([x * CELL, y * CELL]);
    }
const node = (x, y) => y * G + x;
const ringDist = (x, y) => Math.max(Math.abs(x - G / 2 + 0.5), Math.abs(y - G / 2 + 0.5));
let closed = 0;
const edges = [];
for (let y = 0; y < G; y++)
    for (let x = 0; x < G; x++)
        for (const [dx, dy] of [
            [1, 0],
            [0, 1],
        ]) {
            const nx = x + dx,
                ny = y + dy;
            if (nx >= G || ny >= G) continue;
            if (rng.nextFloat() < 0.04) {
                closed++;
                continue;
            } // road works
            const motorway = Math.round(ringDist(x, y)) === 10 && Math.round(ringDist(nx, ny)) === 10;
            const arterial = x % 6 === 0 || y % 6 === 0;
            const kmh = motorway ? 90 : arterial ? 50 : 25,
                minutes = (CELL / kmh) * 60;
            graph.addEdge(node(x, y), node(nx, ny), minutes);
            graph.addEdge(node(nx, ny), node(x, y), minutes);
            edges.push([node(x, y), node(nx, ny), kmh]);
        }

// =============================================================================
// Problem
// =============================================================================
const DEPOT = node(12, 12),
    SHIFT = [7 * 60, 17 * 60];
const VANS = Array.from({ length: 9 }, (_, i) => ({
    id: `VAN-${i + 1}`,
    capacity: 600,
    costKm: "0.42",
    costHour: "28.50",
}));
const WINDOWS = { morning: [8 * 60, 12 * 60], afternoon: [13 * 60, 17 * 60], anytime: [8 * 60, 17 * 60] };
const customers = Array.from({ length: 80 }, (_, i) => {
    const x = rng.nextBounded(G),
        y = rng.nextBounded(G),
        kind = ["morning", "afternoon", "anytime", "anytime"][i % 4];
    return {
        id: `C${String(i + 1).padStart(2, "0")}`,
        node: node(x, y),
        weight: 20 + rng.nextBounded(80),
        service: 4 + rng.nextBounded(8),
        window: WINDOWS[kind],
        kind,
    };
});
const sites = [DEPOT, ...customers.map((c) => c.node)];
const T = sites.map((s) => graph.dijkstra(s)).map((row) => sites.map((t) => row[t])); // minutes
const reachable = customers.filter((c, i) => Number.isFinite(T[0][i + 1]) && Number.isFinite(T[i + 1][0]));
const KM = sites.map((a) =>
    sites.map((b) => Math.abs(coords[a][0] - coords[b][0]) + Math.abs(coords[a][1] - coords[b][1])),
); // street km (Manhattan)

// =============================================================================
// Route evaluation (feasibility and cost)
// =============================================================================
const ix = new Map(customers.map((c, i) => [c.id, i + 1]));
function evaluate(route) {
    // route: array of customer indices (1-based into sites)
    let t = SHIFT[0],
        load = 0,
        prev = 0,
        km = 0,
        wait = 0,
        late = 0;
    const stops = [];
    for (const k of route) {
        const c = customers[k - 1];
        load += c.weight;
        t += T[prev][k];
        km += KM[prev][k];
        if (t < c.window[0]) {
            wait += c.window[0] - t;
            t = c.window[0];
        }
        if (t > c.window[1]) late++;
        stops.push({ id: c.id, eta: t, window: c.window });
        t += c.service;
        prev = k;
    }
    t += T[prev][0];
    km += KM[prev][0];
    const feasible = load <= 600 && late === 0 && t <= SHIFT[1];
    return { feasible, load, km, minutes: t - SHIFT[0], end: t, wait, stops };
}
const cost = (e) =>
    new Decimal(e.km.toFixed(3))
        .mul(VANS[0].costKm)
        .add(new Decimal((e.minutes / 60).toFixed(4)).mul(VANS[0].costHour));
const totalCost = (routes) => routes.reduce((s, r) => s.add(cost(evaluate(r))), Decimal.ZERO);

// =============================================================================
// Clarke–Wright savings
// =============================================================================
function savings(cust) {
    let routes = cust.map((k) => [k]);
    const list = [];
    for (const i of cust) for (const j of cust) if (i !== j) list.push([T[i][0] + T[0][j] - T[i][j], i, j]);
    list.sort((a, b) => b[0] - a[0]);
    const routeOf = new Map(routes.map((r) => [r[0], r]));
    for (const [, i, j] of list) {
        const ri = routeOf.get(i),
            rj = routeOf.get(j);
        if (!ri || !rj || ri === rj || ri.at(-1) !== i || rj[0] !== j) continue; // i must end its route, j must start one
        const merged = [...ri, ...rj];
        if (!evaluate(merged).feasible) continue;
        routes = routes.filter((r) => r !== ri && r !== rj);
        routes.push(merged);
        for (const k of merged) routeOf.set(k, merged);
    }
    return routes;
}

// =============================================================================
// Local search
// =============================================================================
const near = (() => {
    // 10 nearest customers of each customer (1-based)
    const tree = new KDTree(customers.map((c) => coords[c.node]));
    return customers.map(
        (c) =>
            new Set(
                [...tree.knn(coords[c.node], 11).indices].filter((i) => i !== customers.indexOf(c)).map((i) => i + 1),
            ),
    );
})();
const fcost = (r) => {
    if (!r.length) return 0;
    const e = evaluate(r);
    return e.km * Number(VANS[0].costKm) + (e.minutes / 60) * Number(VANS[0].costHour);
};
function improve(routes) {
    let improved = true,
        moves = { twoOpt: 0, relocate: 0, swap: 0 },
        rounds = 0,
        tried = 0;
    const c = fcost;
    while (improved && rounds++ < 50) {
        improved = false;
        for (let a = 0; a < routes.length; a++) {
            // 2-opt within a route
            const r = routes[a];
            for (let i = 0; i < r.length - 1; i++)
                for (let j = i + 1; j < r.length; j++) {
                    const cand = [...r.slice(0, i), ...r.slice(i, j + 1).reverse(), ...r.slice(j + 1)];
                    tried++;
                    if (evaluate(cand).feasible && c(cand) < c(routes[a]) - 1e-6) {
                        routes[a] = cand;
                        moves.twoOpt++;
                        improved = true;
                    }
                }
        }
        for (let a = 0; a < routes.length; a++)
            for (let b = 0; b < routes.length; b++) {
                // relocate a stop next to a neighbour
                if (a === b) continue;
                for (let i = 0; i < routes[a].length; i++)
                    for (let p = 0; p <= routes[b].length; p++) {
                        const k = routes[a][i],
                            prev = routes[b][p - 1],
                            next = routes[b][p];
                        if (!near[k - 1].has(prev) && !near[k - 1].has(next)) continue;
                        tried++;
                        const ra = routes[a].filter((_, x) => x !== i),
                            rb = [...routes[b].slice(0, p), k, ...routes[b].slice(p)];
                        if (
                            (ra.length === 0 || evaluate(ra).feasible) &&
                            evaluate(rb).feasible &&
                            c(ra) + c(rb) < c(routes[a]) + c(routes[b]) - 1e-6
                        ) {
                            routes[a] = ra;
                            routes[b] = rb;
                            moves.relocate++;
                            improved = true;
                            break;
                        }
                    }
            }
        routes = routes.filter((r) => r.length);
        for (let a = 0; a < routes.length; a++)
            for (let b = a + 1; b < routes.length; b++) {
                // swap two neighbouring stops
                for (let i = 0; i < routes[a].length; i++)
                    for (let j = 0; j < routes[b].length; j++) {
                        if (!near[routes[a][i] - 1].has(routes[b][j])) continue;
                        tried++;
                        const ra = [...routes[a]],
                            rb = [...routes[b]];
                        [ra[i], rb[j]] = [rb[j], ra[i]];
                        if (
                            evaluate(ra).feasible &&
                            evaluate(rb).feasible &&
                            c(ra) + c(rb) < c(routes[a]) + c(routes[b]) - 1e-6
                        ) {
                            routes[a] = ra;
                            routes[b] = rb;
                            moves.swap++;
                            improved = true;
                        }
                    }
            }
    }
    return { routes, moves, rounds, tried };
}

// =============================================================================
// Solve
// =============================================================================
const cust = reachable.map((c) => ix.get(c.id));
let t0 = performance.now();
const initial = savings(cust);
const initialCost = totalCost(initial);
const { routes, moves, tried } = improve(initial.map((r) => [...r]));
const solveMs = performance.now() - t0;
const finalCost = totalCost(routes);
const lowerBound = cust.reduce((s, k) => {
    const ins = Math.min(...[0, ...cust].filter((x) => x !== k).map((x) => T[x][k])),
        outs = Math.min(...[0, ...cust].filter((x) => x !== k).map((x) => T[k][x]));
    return s + (ins + outs) / 2 + customers[k - 1].service;
}, 0);
const evals = routes.map(evaluate),
    totalMinutes = evals.reduce((s, e) => s + e.minutes, 0);
const pathFor = (a, b) =>
    graph.aStar(
        a,
        b,
        (n) => ((Math.abs(coords[n][0] - coords[b][0]) + Math.abs(coords[n][1] - coords[b][1])) / 90) * 60,
    ).path;
const geojson = {
    type: "FeatureCollection",
    features: [
        ...routes.map((r, v) => ({
            type: "Feature",
            properties: { vehicle: VANS[v].id, stops: r.length },
            geometry: {
                type: "LineString",
                coordinates: [0, ...r, 0]
                    .slice(0, -1)
                    .flatMap((k, i, arr) =>
                        pathFor(sites[k], sites[i + 1 < arr.length ? arr[i + 1] : 0]).map((n) => coords[n]),
                    ),
            },
        })),
        ...customers.map((c) => ({
            type: "Feature",
            properties: { id: c.id, window: c.kind },
            geometry: { type: "Point", coordinates: coords[c.node] },
        })),
    ],
};

// =============================================================================
// Dispatch API: insert a late order by cheapest feasible insertion
// =============================================================================
const app = new App({ port: 0 });
app.post("/orders", (req) => {
    const o = JSON.parse(req.body),
        n = node(o.x, o.y);
    customers.push({
        id: o.id,
        node: n,
        weight: o.weight,
        service: o.service ?? 6,
        window: WINDOWS[o.window] || WINDOWS.anytime,
        kind: o.window,
    });
    const k = customers.length;
    const toNew = sites.map((s) => graph.dijkstra(s, n)),
        fromNew = graph.dijkstra(n);
    T.forEach((row, i) => row.push(toNew[i]));
    sites.push(n);
    T.push(sites.map((s) => fromNew[s]));
    KM.forEach((row, i) =>
        row.push(Math.abs(coords[sites[i]][0] - coords[n][0]) + Math.abs(coords[sites[i]][1] - coords[n][1])),
    );
    KM.push(sites.map((s) => Math.abs(coords[s][0] - coords[n][0]) + Math.abs(coords[s][1] - coords[n][1])));
    let best = null;
    routes.forEach((r, v) => {
        for (let p = 0; p <= r.length; p++) {
            const cand = [...r.slice(0, p), k, ...r.slice(p)];
            const e = evaluate(cand);
            if (!e.feasible) continue;
            const delta = Number(cost(e)) - Number(cost(evaluate(r)));
            if (!best || delta < best.delta) best = { v, p, delta, cand, eta: e.stops.find((s) => s.id === o.id).eta };
        }
    });
    if (!best) {
        if (routes.length < VANS.length && evaluate([k]).feasible) {
            routes.push([k]);
            return {
                vehicle: VANS[routes.length - 1].id,
                position: 0,
                extraCost: cost(evaluate([k])).toFixed(2),
                newVan: true,
            };
        }
        return {
            status: 409,
            body: JSON.stringify({ error: "no feasible insertion" }),
            contentType: "application/json",
        };
    }
    routes[best.v] = best.cand;
    return {
        vehicle: VANS[best.v].id,
        position: best.p,
        extraCost: best.delta.toFixed(2),
        eta: `${String(Math.floor(best.eta / 60)).padStart(2, "0")}:${String(Math.round(best.eta % 60)).padStart(2, "0")}`,
    };
});
app.start();
const late = await (
    await fetch(`http://127.0.0.1:${app.port}/orders`, {
        method: "POST",
        body: JSON.stringify({ id: "LATE-1", x: 15, y: 9, weight: 30, window: "afternoon" }),
    })
).json();
const impossible = await fetch(`http://127.0.0.1:${app.port}/orders`, {
    method: "POST",
    body: JSON.stringify({ id: "LATE-2", x: 3, y: 20, weight: 900, window: "morning" }),
});
await impossible.text();
app.close();

// =============================================================================
// Checks and report
// =============================================================================
const results = {};
const served = new Set(routes.flat().filter((k) => k <= 80));
results[
    `every reachable customer is served exactly once (${cust.length} of 80; ${80 - cust.length} cut off by closures)`
] = served.size === cust.length && routes.flat().filter((k) => k <= 80).length === cust.length;
results["every route respects capacity, time windows and the shift"] = evals.every(
    (e) => e.feasible && e.load <= 600 && e.end <= SHIFT[1],
);
results[`${routes.length} vans used of ${VANS.length}`] = routes.length <= VANS.length;
results[
    `local search improved cost from ${initialCost.toFixed(2)} to ${finalCost.toFixed(2)} (${moves.twoOpt} 2-opt, ${moves.relocate} relocate, ${moves.swap} swap)`
] = finalCost.cmp(initialCost) <= 0 && moves.twoOpt + moves.relocate + moves.swap > 0;
results["the neighbour lists come from the KDTree's 10 nearest drop-offs"] = near.every(
    (set, i) => set.size === 10 && !set.has(i + 1),
);
results[
    `driving+service ${totalMinutes.toFixed(0)} min is above the lower bound ${lowerBound.toFixed(0)} (gap ${((totalMinutes / lowerBound - 1) * 100).toFixed(0)}%)`
] = totalMinutes >= lowerBound;
results["GeoJSON routes follow the road graph"] =
    JSONPointer.get(geojson, "/features/0/geometry/type") === "LineString" &&
    geojson.features[0].geometry.coordinates.length > routes[0].length;
results[
    `the late order went to ${late.vehicle} at stop ${late.position} (+€${late.extraCost}, ETA ${late.eta}); an overweight order is refused`
] = late.vehicle && Number(late.extraCost) >= 0 && impossible.status === 409;

const df = new DataFrame({
    km: Float64Array.from(evals.map((e) => e.km)),
    minutes: Float64Array.from(evals.map((e) => e.minutes)),
    load: Float64Array.from(evals.map((e) => e.load)),
});
const hhmm = (m) => `${String(Math.floor(m / 60)).padStart(2, "0")}:${String(Math.round(m % 60)).padStart(2, "0")}`;
print("logistics-routing");
print(
    `  network    ${G * G} junctions, ${edges.length} road segments (${closed} closed); travel-time matrix ${sites.length - 2}x${sites.length - 2} by Dijkstra`,
);
print(
    `  solve      savings ${initial.length} routes, cost ${initialCost.toFixed(2)} -> local search ${routes.length} routes, cost €${finalCost.toFixed(2)} in ${solveMs.toFixed(0)} ms (${tried} granular moves tried)`,
);
print(
    table([
        ["van", "stops", "load kg", "km", "start-end", "wait min", "cost €"],
        ...routes.map((r, v) => {
            const e = evaluate(r);
            return [
                VANS[v].id,
                String(r.length),
                String(e.load),
                e.km.toFixed(1),
                `${hhmm(SHIFT[0])}-${hhmm(e.end)}`,
                e.wait.toFixed(0),
                cost(e).toFixed(2),
            ];
        }),
    ]),
);
print(
    `  fleet      mean ${df.MEAN("km").toFixed(1)} km and ${(df.MEAN("load") / 6).toFixed(0)}% load per van; manifest for ${VANS[0].id}: ${evaluate(
        routes[0],
    )
        .stops.slice(0, 4)
        .map((s) => `${s.id}@${hhmm(s.eta)}`)
        .join(", ")}…`,
);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
