/* JSON-records kernel: parse a batch of JSON records into objects, touch
 * fields, re-stringify. Measures object lifecycle under JSON parse/build. */
var src = [];
for (var i = 0; i < 200; i++) {
    src.push('{"id":' + i + ',"name":"rec' + (i & 63) +
             '","vals":[' + (i % 7) + ',' + (i % 11) + ',' + (i % 13) +
             '],"meta":{"k":' + (i & 31) + ',"v":"x' + (i & 3) + '"}}');
}

var acc = 0;
var ROUNDS = 3000;
var t0 = Date.now();
for (var r = 0; r < ROUNDS; r++) {
    for (var i = 0; i < src.length; i++) {
        var o = JSON.parse(src[i]);
        var m = {id: o.id, sum: o.vals[0] + o.vals[1] + o.vals[2],
                 kv: o.meta.k + o.meta.v.length};
        acc += m.sum + m.kv;
        if ((i & 31) === 0)
            JSON.stringify(m);
    }
}
var t1 = Date.now();
print("json_records: acc=" + acc + " time=" + (t1 - t0) + "ms R=" + ROUNDS);
