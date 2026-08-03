/* Differential oracle for the get_date_string() / JSON \uXXXX rewrites.
   Dumps a large corpus of formatted output; the SHA of this dump must be
   identical before and after the change. Run:
     ./dynajs tests/oracle_date_json.js | shasum */
var out = [];
var push = function (s) { out.push(s); if (out.length > 4096) { print(out.join("\n")); out = []; } };

/* dense sweep near the epoch and across the full representable range */
var steps = [1, 7, 61, 3607, 86399, 86400 * 37, 86400 * 397];
for (var si = 0; si < steps.length; si++) {
    var st = steps[si] * 1000;
    for (var k = -600; k <= 600; k++) {
        var t = k * st;
        var d = new Date(t);
        push(d.toISOString());
        push(d.toUTCString());
        push(d.toString());
        push(d.toLocaleString());
        push(d.toLocaleDateString());
        push(d.toLocaleTimeString());
        push(d.toDateString());
        push(d.toTimeString());
        push(JSON.stringify(d));
    }
}

/* extremes and the extended-year snprintf fallback */
var edge = [0, -1, 1, 8640000000000000, -8640000000000000,
            8639999999999999, -8639999999999999, 253402300799999, -62135596800000];
for (var i = 0; i < edge.length; i++) {
    var d = new Date(edge[i]);
    push(d.toISOString());
    push(d.toUTCString());
    push(d.toString());
    push(d.toLocaleString());
}
for (var y = -300000; y <= 300000; y += 4409) {
    var d = new Date(0);
    d.setUTCFullYear(y, 5, 15);
    if (isNaN(d.getTime())) continue;
    push(d.toISOString());
    push(d.toUTCString());
    push(d.toLocaleDateString());
}

/* JSON escape corpus: every BMP code point, in blocks */
for (var base = 0; base < 0x10000; base += 64) {
    var s = "";
    for (var c = base; c < base + 64; c++) s += String.fromCharCode(c);
    push(JSON.stringify(s));
}
/* single code points, so each escape is emitted in isolation */
for (var c = 0; c < 0x10000; c += 7) {
    push(JSON.stringify(String.fromCharCode(c)));
}
/* keys as well as values */
for (var c = 0; c < 0x2000; c += 11) {
    var o = {};
    o[String.fromCharCode(c) + "k"] = String.fromCharCode(c);
    push(JSON.stringify(o));
}
print(out.join("\n"));
