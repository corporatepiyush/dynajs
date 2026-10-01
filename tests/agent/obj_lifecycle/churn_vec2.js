function Vec2(x, y) {
    this.x = x;
    this.y = y;
}
function Mat2(a, b, c, d) {
    this.a = a; this.b = b; this.c = c; this.d = d;
}
Vec2.prototype.add = function (o) {
    return new Vec2(this.x + o.x, this.y + o.y);
};
Vec2.prototype.sub = function (o) {
    return new Vec2(this.x - o.x, this.y - o.y);
};
Vec2.prototype.scale = function (s) {
    return new Vec2(this.x * s, this.y * s);
};
Vec2.prototype.dot = function (o) {
    return this.x * o.x + this.y * o.y;
};
Mat2.prototype.mulv = function (v) {
    return new Vec2(this.a * v.x + this.b * v.y,
                    this.c * v.x + this.d * v.y);
};
Mat2.prototype.mulm = function (m) {
    return new Mat2(this.a * m.a + this.b * m.c, this.a * m.b + this.b * m.d,
                    this.c * m.a + this.d * m.c, this.c * m.b + this.d * m.d);
};

var acc = 0;
var N = 400000;
var m1 = new Mat2(1, 2, 3, 4);
var m2 = new Mat2(5, 6, 7, 8);
var v0 = new Vec2(1, 0);

var t0 = Date.now();
for (var i = 0; i < N; i++) {
    var x = (i & 1023) + 1;
    var v = new Vec2(x, x + 1);
    var w = new Vec2(x * 2, x * 3);
    var r1 = v.add(w);
    var r2 = v.sub(w);
    var r3 = r1.add(r2);
    var r4 = r3.scale(0.5);
    var m = m1.mulm(m2);
    var mv = m.mulv(v);
    var mv2 = m.mulv(r1);
    var d = r4.dot(mv) + mv2.dot(r2) + m.a + m.d + mv.x + mv2.y;
    var lit = {x: r4.x, y: mv.y, m: m.a, k: d};
    var lit2 = {px: lit.x, py: lit.y, pd: lit.k};
    acc += lit2.pd;
}
var t1 = Date.now();
print("churn_vec2: acc=" + acc + " time=" + (t1 - t0) + "ms N=" + N);
