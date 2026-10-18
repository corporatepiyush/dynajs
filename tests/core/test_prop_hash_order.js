"use strict";

function h_str(h, s) {
    for (var i = 0; i < s.length; i++) h = (h * 31 + s.charCodeAt(i)) | 0;
    return h;
}

function digest(o) {
    var h = 17;
    for (var k in o) {
        h = h_str(h, k);
        h = h_str(h, ":" + o[k] + ";");
    }
    h = (h * 31 + Object.keys(o).length) | 0;
    return h;
}

var cases = [];

function add(name, build) {
    var o = build();
    cases.push(name + "=" + digest(o));
}

add("string_seq", function () {
    var o = {};
    for (var i = 0; i < 300; i++) o["k" + i] = i * 3;
    return o;
});

add("int_dense", function () {
    var o = {};
    for (var i = 299; i >= 0; i--) o[i] = i * 3;
    return o;
});

add("int_stride", function () {
    var o = {};
    for (var i = 0; i < 300; i++) o[i * 65536] = i * 3;
    return o;
});

add("mixed", function () {
    var o = {};
    for (var i = 0; i < 100; i++) { o["s" + i] = i; o[i * 4096] = -i; }
    return o;
});

add("deleted", function () {
    var o = {};
    for (var i = 0; i < 400; i++) o["d" + i] = i;
    for (var i = 0; i < 400; i += 3) delete o["d" + i];
    for (var i = 0; i < 200; i++) o["e" + i] = i * 2;
    return o;
});

add("grow", function () {
    var o = {};
    for (var i = 0; i < 5000; i++) o["g" + i] = i;
    for (var i = 0; i < 5000; i += 7) delete o["g" + i];
    return o;
});

add("descriptors", function () {
    var o = {};
    for (var i = 0; i < 50; i++) {
        Object.defineProperty(o, "p" + i, {
            value: i, enumerable: (i % 2) === 0, writable: true,
            configurable: true
        });
    }
    for (var i = 0; i < 20; i++) {
        Object.defineProperty(o, "a" + i, {
            get: function () { return 7; }, enumerable: true, configurable: true
        });
    }
    return o;
});

add("json", function () {
    var parts = [];
    for (var i = 0; i < 400; i++) parts.push('"' + (i * 65536) + '":' + i);
    return JSON.parse("{" + parts.join(",") + "}");
});

function readback() {
    var bad = 0, i;
    var o = {};
    for (i = 0; i < 2000; i++) o[i * 65536] = i;
    for (i = 0; i < 2000; i++) if (o[i * 65536] !== i) bad++;
    for (i = 0; i < 2000; i += 2) delete o[i * 65536];
    for (i = 0; i < 2000; i++) {
        var want = (i % 2) ? i : undefined;
        if (o[i * 65536] !== want) bad++;
    }
    return bad;
}

for (var i = 0; i < cases.length; i++) print(cases[i]);
print("readback_failures=" + readback());
print("own_keys_order=" +
      Object.getOwnPropertyNames({ b: 1, 2: 1, a: 1, 0: 1, 10: 1 }).join(","));
