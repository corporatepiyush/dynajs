/* Event-emitter kernel: construct handler objects/closures contexts and
 * emit events through a small emitter; measures object churn under
 * polymorphic call pressure. */
function Emitter() {
    this.handlers = Object.create(null);
    this.count = 0;
}
Emitter.prototype.on = function (name, fn) {
    var l = this.handlers[name];
    if (l === undefined)
        l = this.handlers[name] = [];
    l.push(fn);
    return this;
};
Emitter.prototype.emit = function (name, a, b) {
    var l = this.handlers[name];
    if (l !== undefined) {
        for (var i = 0; i < l.length; i++)
            l[i](a, b);
    }
    this.count++;
    return this;
};

var em = new Emitter();
em.on("tick", function (a, b) { acc += a.x + b.y; });
em.on("tick", function (a, b) { acc += a.y - b.x; });
em.on("packet", function (a, b) { acc += a.id * 2; });
em.on("packet", function (a, b) {
    var rec = {id: a.id, t: b, px: a.x, py: a.y};
    acc += rec.px + rec.py + rec.t;
});
em.on("flush", function (a, b) { acc += 1; });

var acc = 0;
var N = 2000000;
var t0 = Date.now();
for (var i = 0; i < N; i++) {
    var v = {x: i & 255, y: (i >> 3) & 255, id: i & 1023};
    em.emit("tick", v, {x: i & 7, y: i & 15});
    if ((i & 15) === 0)
        em.emit("packet", v, i & 31);
    if ((i & 255) === 0)
        em.emit("flush", v, 0);
}
var t1 = Date.now();
print("ev_emit: acc=" + acc + " time=" + (t1 - t0) + "ms N=" + N);
