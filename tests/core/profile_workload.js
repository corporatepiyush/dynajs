var SECONDS = 20;
if (typeof scriptArgs !== "undefined" && scriptArgs[1]) SECONDS = +scriptArgs[1];

var seed = 12345;
function rnd(n) { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed % n; }

var words = [];
for (var i = 0; i < 512; i++) words.push("word" + i + "_" + (i * 7919 % 997));

function makeRecord(i) {
    return {
        id: i,
        uuid: "550e8400-e29b-41d4-a716-" + (446655440000 + i),
        name: words[rnd(words.length)] + " " + words[rnd(words.length)],
        path: "/api/v1/resource/" + i + "/sub/" + rnd(1000),
        email: "user" + i + "@example.com",
        active: (i & 1) === 0,
        score: i * 1.5 + rnd(100) / 7,
        tags: [words[rnd(512)], words[rnd(512)], words[rnd(512)]],
        created: new Date(1700000000000 + i * 60000)
    };
}

var emailRe = /^[^@]+@[^@]+\.[a-z]{2,}$/;
var pathRe = /\/api\/v(\d+)\/resource\/(\d+)/;

var end = Date.now() + SECONDS * 1000;
var rounds = 0, sink = 0;

while (Date.now() < end) {
    var recs = [];
    for (var i = 0; i < 2000; i++) recs.push(makeRecord(i + rounds * 2000));

    var json = JSON.stringify(recs);
    var back = JSON.parse(json);
    sink += back.length;

    var active = back.filter(function (r) { return r.active; });
    var names = active.map(function (r) { return r.name; });
    var total = active.reduce(function (a, r) { return a + r.score; }, 0);
    sink += total | 0;

    var ids = back.map(function (r) { return r.id; });
    for (var k = 0; k < 200; k++) {
        sink += ids.indexOf(rnd(2000));
        if (ids.includes(rnd(2000))) sink++;
    }

    sink += names.join(", ").length;
    var lines = [];
    for (var i = 0; i < 500; i++) {
        var r = back[i];
        lines.push(String(r.id).padStart(8, "0") + " | " +
                   r.name.padEnd(40, " ") + " | " + r.email);
    }
    sink += lines.join("\n").length;

    for (var i = 0; i < 500; i++) {
        if (emailRe.test(back[i].email)) sink++;
        var m = pathRe.exec(back[i].path);
        if (m) sink += m[2].length;
    }

    for (var i = 0; i < 500; i++) {
        var d = new Date(back[i].created);
        sink += d.toISOString().length + d.toUTCString().length;
    }

    var sorted = active.slice().sort(function (a, b) { return a.score - b.score; });
    sink += sorted.length;

    var m2 = new Map(), s2 = new Set();
    for (var i = 0; i < 2000; i++) { m2.set(back[i].uuid, back[i]); s2.add(back[i].name); }
    sink += m2.size + s2.size;

    rounds++;
}

print("rounds=" + rounds + " sink=" + sink);
