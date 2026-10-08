// 43 · Delivery route planner — shortest paths, a goal-directed search, a delivery tour and a cabling plan on a street graph.
//
// WHAT IT SHOWS
//   - dyna:structures Graph: Dijkstra for one-to-all distances, A* with a straight-line heuristic for one-to-one
//   - turning coordinates into a weighted graph, and node names into the integer ids the graph uses
//   - a nearest-neighbour tour over a precomputed distance matrix (a practical TSP heuristic)
//   - a minimum spanning tree: the cheapest way to connect every site
//   - connected components to detect locations nothing can reach
//
// RUN      dynajs examples/apps/43-delivery-route-planner.js

import { Graph } from "dyna:structures";

// ---- the map: places with coordinates (km) and two-way roads -----------------
const places = {
    depot: [0, 0], north: [1, 5], market: [4, 4], station: [6, 1], harbour: [9, 3],
    school: [3, 8], hospital: [8, 7], mill: [11, 8], island: [15, 0],
};
const roads = [
    ["depot", "north"], ["depot", "station"], ["north", "market"], ["north", "school"], ["market", "station"],
    ["market", "hospital"], ["station", "harbour"], ["harbour", "hospital"], ["school", "hospital"], ["hospital", "mill"],
];

const names = Object.keys(places);
const id = Object.fromEntries(names.map((n, i) => [n, i]));
const straightLine = (a, b) => Math.hypot(places[a][0] - places[b][0], places[a][1] - places[b][1]);

const graph = new Graph({ directed: false, weighted: true });
for (const _ of names) graph.addNode();
// Roads wind: assume they are 20% longer than the straight line.
for (const [a, b] of roads) graph.addEdge(id[a], id[b], straightLine(a, b) * 1.2);

// ---- 1. reachability -------------------------------------------------------
const component = graph.connectedComponents();
const unreachable = names.filter((n) => component[id[n]] !== component[id.depot]);

// ---- 2. one-to-one with A* -------------------------------------------------
// The heuristic must never OVERestimate the remaining road distance. Straight
// line distance cannot: no road is shorter than a straight line.
function route(from, to) {
    const { dist, path } = graph.aStar(id[from], id[to], (node) => straightLine(names[node], to));
    return { km: dist, stops: path.map((n) => names[n]) };
}

// ---- 3. a delivery tour ----------------------------------------------------
// One Dijkstra per stop gives every pairwise road distance. Then always drive
// to the nearest undelivered stop; not optimal, but fast and usually close.
function tour(start, stops) {
    const all = [start, ...stops];
    const dist = Object.fromEntries(all.map((a) => [a, graph.dijkstra(id[a])]));
    const order = [start];
    const todo = new Set(stops);
    let total = 0;
    while (todo.size) {
        const here = order[order.length - 1];
        let next = null;
        for (const s of todo) if (next === null || dist[here][id[s]] < dist[here][id[next]]) next = s;
        total += dist[here][id[next]];
        order.push(next);
        todo.delete(next);
    }
    total += dist[order[order.length - 1]][id[start]];          // drive back
    return { order: [...order, start], km: total };
}

// ---- 4. cheapest network connecting everything reachable --------------------
const tree = graph.mst();

// ---- report ----------------------------------------------------------------
const trip = route("depot", "mill");
console.log(`depot -> mill: ${trip.km.toFixed(1)} km via ${trip.stops.join(" > ")}`);
const round = tour("depot", ["hospital", "school", "harbour", "market"]);
console.log(`delivery round: ${round.order.join(" > ")} (${round.km.toFixed(1)} km)`);
console.log(`fibre backbone: ${tree.edges.length} links, ${tree.weight.toFixed(1)} km`);
console.log("unreachable:", unreachable.join(", ") || "none");

// ---- self-test -------------------------------------------------------------
const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
check(unreachable.join() === "island", "the island has no road and is reported");
check(trip.stops[0] === "depot" && trip.stops[trip.stops.length - 1] === "mill", "the route starts and ends where asked");
check(trip.stops.every((s, i) => i === 0 || graph.hasEdge(id[trip.stops[i - 1]], id[s])), "every hop of the route is a real road");
// A* must agree with Dijkstra on the distance: the heuristic only changes how fast it is found.
check(Math.abs(trip.km - graph.dijkstra(id.depot, id.mill)) < 1e-9, "A* finds the same distance as Dijkstra");
check(trip.km >= straightLine("depot", "mill"), "no route beats the straight line");
check(new Set(round.order).size === 5 && round.order.length === 6, "the tour visits every stop once and returns");
const naive = ["depot", "hospital", "school", "harbour", "market", "depot"]
    .reduce((km, s, i, a) => i ? km + graph.dijkstra(id[a[i - 1]], id[s]) : 0, 0);
check(round.km <= naive, `nearest-neighbour beats visiting in list order (${round.km.toFixed(1)} vs ${naive.toFixed(1)} km)`);
check(tree.edges.length === names.length - 2, "the spanning tree links all eight connected places with seven edges");
check(tree.weight < roads.reduce((km, [a, b]) => km + straightLine(a, b) * 1.2, 0), "the backbone is shorter than the full road network");
console.log("self-test passed");
