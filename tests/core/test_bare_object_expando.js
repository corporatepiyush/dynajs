// covers: c:src/object/shapes_objects_gc.inc.c
// Native objects created by JS_NewObjectProtoClassBare (web stream objects, their readers/writers/controllers, and promises the
// runtime creates internally such as reader.closed) start with a zero-capacity property array and a hashed empty shape of their
// own (find_hashed_shape_proto keeps it apart from the 2-slot shape that ordinary objects with the same prototype use). Expandos
// must behave exactly as on any object: growth from zero capacity, shape transitions shared with ordinary objects of the same
// prototype, define/delete/freeze, enumeration order, and many properties (dictionary-style resize).
import { suite } from "../lib/kit.js";

const t = suite("bare_object_expando");

const makers = {
    TransformStream: () => new TransformStream(),
    ReadableStream: () => new ReadableStream(),
    WritableStream: () => new WritableStream(),
    "reader.closed promise": () => new ReadableStream().getReader().closed,
    "writer.ready promise": () => new WritableStream().getWriter().ready,
    "pipeTo promise": () => { const p = new ReadableStream().pipeTo(new WritableStream()); p.catch(() => {}); return p; },
    WritableStreamDefaultWriter: () => new WritableStream().getWriter(),
};

for (const [name, mk] of Object.entries(makers)) {
    t.test(`${name}: expandos grow from zero capacity`, (a) => {
        const o = mk();
        a.eq(Object.keys(o).length, 0);
        for (let i = 0; i < 40; i++) o["k" + i] = i;
        let sum = 0;
        for (let i = 0; i < 40; i++) sum += o["k" + i];
        a.eq(sum, 780);
        a.eq(Object.keys(o).join(","), Array.from({ length: 40 }, (_, i) => "k" + i).join(","));
    });
    t.test(`${name}: define, delete, freeze`, (a) => {
        const o = mk();
        Object.defineProperty(o, "g", { get() { return 7; }, configurable: true, enumerable: false });
        o.x = 1;
        a.eq(o.g, 7);
        a.eq(Object.keys(o).join(","), "x");
        a.ok(delete o.g);
        a.eq(o.g, undefined);
        o[Symbol.for("s")] = 2;
        a.eq(o[Symbol.for("s")], 2);
        Object.freeze(o);
        a.ok(Object.isFrozen(o));
        a.throws(() => { "use strict"; o.y = 1; }, TypeError);
        a.eq(o.x, 1);
    });
}

t.test("bare and ordinary objects with one prototype share transitions without mixing capacities", (a) => {
    const plain = Object.create(TransformStream.prototype);
    const bare = new TransformStream();
    plain.a = 1; plain.b = 2; plain.c = 3;
    bare.a = 10; bare.b = 20; bare.c = 30; bare.d = 40;
    const bare2 = new TransformStream();
    bare2.b = 5; bare2.a = 6;
    a.eq([plain.a, plain.b, plain.c, plain.d].join(","), "1,2,3,");
    a.eq([bare.a, bare.b, bare.c, bare.d].join(","), "10,20,30,40");
    a.eq(Object.keys(bare2).join(","), "b,a");
    a.ok(bare.readable instanceof ReadableStream, "the bare object still works as a stream");
    a.eq(Object.keys(new TransformStream()).length, 0);
});

t.test("internal promises with expandos still settle and chain", async (a) => {
    const rs = new ReadableStream({ start(c) { c.close(); } });
    const p = rs.getReader().closed;
    p.tag = "t";
    p.more = [1, 2, 3];
    const v = await p.then(() => p.tag + p.more.length);
    a.eq(v, "t3");
});

await t.run();
