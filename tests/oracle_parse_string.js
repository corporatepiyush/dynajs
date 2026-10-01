function fnv(s) {
    let h = 2166136261 >>> 0;
    for (let i = 0; i < s.length; i++) {
        h ^= s.charCodeAt(i) & 0xff;
        h = Math.imul(h, 16777619) >>> 0;
        h ^= (s.charCodeAt(i) >>> 8);
        h = Math.imul(h, 16777619) >>> 0;
    }
    return h >>> 0;
}
let acc = 0, cases = 0, errs = 0;
function feed(src) {
    cases++;
    let out;
    try { out = "V" + String(eval(src)); }
    catch (e) { errs++; out = "E" + e.name + ":" + e.message; }
    acc = (Math.imul(acc, 31) + fnv(out)) >>> 0;
}

const BS = String.fromCharCode(92);
const Q = '"';
const TICK = String.fromCharCode(96);

const ALPHA = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJ0123456789 _-";
for (let len = 0; len <= 40; len++) {
    const body = ALPHA.slice(0, len);
    feed(JSON.stringify(body));
    feed("'" + body + "'");
    feed(TICK + body + TICK);
}

const BREAKERS = [BS + "n", BS + "t", BS + BS, BS + "u00e9", BS + "x41",
                  BS + "0", "$", BS + "r"];
for (let pos = 0; pos <= 24; pos++) {
    for (const br of BREAKERS) {
        const body = "a".repeat(pos) + br + "b".repeat(24 - pos);
        feed(Q + body + Q);
        feed(TICK + body + TICK);
    }
}

for (let pos = 0; pos <= 20; pos++) {
    feed(TICK + "x".repeat(pos) + "${1+1}" + "y".repeat(20 - pos) + TICK);
    feed(TICK + "x".repeat(pos) + "$" + "y".repeat(20 - pos) + TICK);
    feed(TICK + "x".repeat(pos) + "$$" + "y".repeat(20 - pos) + TICK);
}

const WIDE = ["é", "€", "😀", "", "߿",
              "ࠀ", "￿"];
for (let pos = 0; pos <= 18; pos++)
    for (const w of WIDE)
        feed(JSON.stringify("q".repeat(pos) + w + "r".repeat(18 - pos)));

for (const cc of [1, 8, 9, 10, 11, 12, 13, 27, 31]) {
    const ch = String.fromCharCode(cc);
    feed(Q + "a" + ch + "b" + Q);
    feed(TICK + "a" + ch + "b" + TICK);
}

for (const bad of [Q + "unterminated", "'unterminated", TICK + "unterminated",
                   Q + "esc at end" + BS, Q + BS + "u00" + Q,
                   Q + BS + "u{110000}" + Q, Q + BS + "u{}" + Q,
                   TICK + "${", TICK + "${}" + TICK, Q + BS, TICK + "a" + BS,
                   Q + "x".repeat(40), TICK + "x".repeat(40) + "${",
                   Q + BS + "u{-1}" + Q, Q + BS + "uZZZZ" + Q])
    feed(bad);

for (const n of [64, 100, 255, 256, 257, 1000, 4096]) {
    feed(JSON.stringify("z".repeat(n)));
    feed(JSON.stringify("z".repeat(n) + "é"));
    feed(JSON.stringify("é" + "z".repeat(n)));
    feed(TICK + "z".repeat(n) + "${0}" + TICK);
}

for (let i = 0; i < 200; i++)
    feed("(function(){ const a = " + Q + "left " + i + Q + ", b = 'mid " + i +
         "', c = " + TICK + "t " + i + " ${a}" + TICK +
         "; return a.length+b.length+c.length; })()");

print("oracle_parse_string: " + cases + " cases, " + errs + " rejected, hash " +
      acc.toString(16));
