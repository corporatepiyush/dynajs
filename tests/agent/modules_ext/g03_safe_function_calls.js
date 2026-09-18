// modules_ext g03 (dynajs-only): import each module and CALL one function with safe
// args; byte-record the outputs. Only deterministic calls: known-arg encode/decode,
// hex of fixed bytes, fixed timestamp, shapes (never values) for RNG/uuid/timezone
// dependent APIs, typeof only for classes/modules with side effects (net, file, log).
import * as bytes from "dyna:bytes";
import * as encoding from "dyna:encoding";
import * as hash from "dyna:hash";
import * as semver from "dyna:semver";
import * as sys from "dyna:sys";
import * as time from "dyna:time";
import * as uuid from "dyna:uuid";
import * as random from "dyna:random";
import * as mathx from "dyna:mathx";
import * as decimal from "dyna:decimal";
import * as url from "dyna:url";
import * as net from "dyna:net";
const out = (typeof print === 'function') ? print : ((...a) => console.log(...a));
function rec(tag, f) { try { const r = f(); out(tag + ': ' + (typeof r === 'object' && r !== null ? JSON.stringify(r) : String(r))); } catch (e) { out(tag + ': threw ' + ((e && e.name) || 'thrown')); } }
rec('bytes.bytesOf.len', () => bytes.bytesOf(new Uint8Array([104, 105])).length);
rec('bytes.compare', () => bytes.compare(new Uint8Array([97]), new Uint8Array([98])));
rec('encoding.b64', () => encoding.Base64Encode(new Uint8Array([1, 2, 3, 4])));
rec('encoding.b64.roundtrip', () => encoding.Base64Decode(encoding.Base64Encode(new Uint8Array([9, 8, 7]))).length);
rec('hash.blake3hex', () => String(hash.BLAKE3Hex('abc')).slice(0, 16) + '...');
rec('semver.parse', () => { const v = semver.parse('1.2.3-rc.1'); return v ? (v.major + '.' + v.minor + '.' + v.patch) : 'null'; });
rec('sys.cwd.type', () => typeof sys.cwd() === 'string');
rec('sys.pid.type', () => typeof sys.pid === 'function' ? (typeof sys.pid()) : 'nofn');
rec('time.rfc3339.epoch', () => { const s = time.formatRFC3339(new Date(0)); return String(s).slice(0, 4) + '-shape'; });
rec('time.now.type', () => typeof time.now === 'function' ? (typeof time.now()) : 'nofn');
rec('uuid.v4.shape', () => { const u = uuid.v4(); return typeof u + ':' + String(u).length + ':' + (String(u)[14] === '4'); });
rec('random.shape', () => { const f = random.random ? random.random() : random.float ? random.float() : NaN; return typeof f; });
rec('mathx.types', () => typeof mathx.Expression);
rec('mathx.lerp', () => typeof mathx.lerp === 'function' ? mathx.lerp(0, 10, 0.5) : 'nofn');
rec('decimal.types', () => typeof decimal.Decimal);
rec('url.types', () => typeof url.URL);
rec('net.types', () => typeof net.App + ',' + typeof net.HTTPClient);
out('DONE');
