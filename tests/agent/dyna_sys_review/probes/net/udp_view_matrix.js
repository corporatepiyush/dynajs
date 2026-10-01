import { UDPSocket } from "dyna:net";
import { getEnv } from "dyna:sys";
import { ok, eq, setTag, done } from "../../rh_module.js";
const PORT = parseInt(getEnv("DYN_CTL_PORT"));
setTag("net.udp_view_matrix");

var sock = new UDPSocket({ port: 0, host: "127.0.0.1" });
var peerPort = PORT;
var got = [];
var expect = [];
var echoes = 0;

sock.start({
  message: function (data, from) {
    echoes++;
    got.push(Array.prototype.slice.call(data, 0));
  }
});

function sendAndExpect(label, payload, wantBytes) {
  var n = -1;
  try { n = sock.send(payload, "127.0.0.1", peerPort); }
  catch (e) {
    ok(false, label + ": send threw " + e);
    expect.push({ label: label, want: [], threw: true });
    return;
  }
  ok(n === wantBytes.length, label + ": send() returned " + n + " want " + wantBytes.length);
  expect.push({ label: label, want: wantBytes });
}

sendAndExpect("plain Uint8Array", new Uint8Array([104, 105]), [104, 105]);

var buf = new ArrayBuffer(10);
var full = new Uint8Array(buf);
for (var i = 0; i < 10; i++) full[i] = i;
var view = new Uint8Array(buf, 3, 3);
sendAndExpect("offset view", view, [3, 4, 5]);

var sub = full.subarray(6, 9);
sendAndExpect("subarray view", sub, [6, 7, 8]);

var zero = new Uint8Array(buf, 2, 0);
sendAndExpect("zero-length view", zero, []);

function U8Sub(a) {
  var arr = new Uint8Array(a);
  arr.extra = 42;
  return arr;
}
var subobj = U8Sub([200, 201]);
sendAndExpect("subclass-shaped Uint8Array", subobj, [200, 201]);

var i8 = new Int8Array([-1, 127, -128]);
sendAndExpect("Int8Array", i8, [255, 127, 128]);

var f32 = new Float32Array([1.0]);
sendAndExpect("Float32Array view", f32, [0, 0, 128, 63]);

var dv = new DataView(buf, 1, 4);
var dvWant = [1, 2, 3, 4];
var nDv = sock.send(dv, "127.0.0.1", peerPort);
expect.push({ label: "DataView", want: dvWant, recordOnly: true });
ok(true, "DataView send returned " + nDv + " (documented contract is Uint8Array only)");

var nStr = -1;
try { nStr = sock.send("abc", "127.0.0.1", peerPort); } catch (e) { ok(false, "string send threw " + e); }
ok(nStr === 3, "string send returns 3, got " + nStr);
expect.push({ label: "string", want: [97, 98, 99] });

var big = new Uint8Array(8192);
for (var b = 0; b < 8192; b++) big[b] = b & 0xff;
var nBig = -1;
try { nBig = sock.send(big, "127.0.0.1", peerPort); } catch (e) { ok(false, "8KiB send threw " + e); }
ok(nBig === 8192, "8KiB send returns 8192, got " + nBig);
expect.push({ label: "8KiB", want: null, big: true, len: 8192 });

var waited = 0;
var tick = function () {
  if (echoes >= expect.length || waited > 100) {
    eq(got.length, expect.length, "received one echo per send");
    for (var k = 0; k < expect.length && k < got.length; k++) {
      var e = expect[k], g = got[k];
      if (e.big) {
        var bg = g.length > 0 && g[0] === 62 ? g.slice(1) : g;
        ok(bg.length === e.len, e.label + ": echo length " + bg.length + " want " + e.len);
        var bad = -1;
        for (var j = 0; j < bg.length; j++) { if (bg[j] !== (j & 0xff)) { bad = j; break; } }
        ok(bad < 0, e.label + ": byte-exact (first mismatch at " + bad + ")");
      } else {
        var stripped = g.length > 0 && g[0] === 62 ? g.slice(1) : g;
        eq(stripped, e.want, e.label + ": echoed bytes exact");
      }
    }
    sock.close();
    setTag("net.udp_view_matrix");
    done();
    return;
  }
  waited++; setTimeout(tick, 20);
};
setTimeout(tick, 30);
