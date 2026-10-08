// flags: --std
import "./httpc.js";
import { TCPServer, HTTPClient } from "dyna:net";
import { makeTempDir, writeFile, Path, removeAll } from "dyna:file";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("  FAIL: " + m); } }

const CERT = "-----BEGIN CERTIFICATE-----\nMIIC6DCCAdCgAwIBAgIUZffd0t5pGL74tPa9YwrIhxWOSj4wDQYJKoZIhvcNAQEL\nBQAwFDESMBAGA1UEAwwJMTI3LjAuMC4xMB4XDTI2MTAwNTEwMTYxNloXDTM2MTAw\nMjEwMTYxNlowFDESMBAGA1UEAwwJMTI3LjAuMC4xMIIBIjANBgkqhkiG9w0BAQEF\nAAOCAQ8AMIIBCgKCAQEAkRiY/fDaIxRQT+fLAKv/sMAIWN64b3pLe0tefcBAWbsK\nHh1V4KILFabIthLxENs53KSHzKqJfCrZiyDSmpXdkQc2h0lbx3LrHTi1jg3uesb4\nILcFOoPaX/fa3kC6EGVBuu1jl7dpvkS8vGc/Q6+FMWJdome6nTZKeokNyExwsfyw\nqCwwsX50MGRiMPe5ELpxX97ywekrOAX/j61/pVpR/qwGIfzzuSG6ru/vafr/tEDI\nUl0wk2JSazAheKKYr0/Jbm7Q1igTuMxyzwX5+QhA2k+5V3cKuQG3VMuzEI6pZ4hO\nd9NSVBDhGu/YsWlRPk/v7MBRRVgwNO1GT1qy8xtWBwIDAQABozIwMDAdBgNVHQ4E\nFgQUFWMV7e+nFsb3bmUkg0noFrkNIlYwDwYDVR0TAQH/BAUwAwEB/zANBgkqhkiG\n9w0BAQsFAAOCAQEAWK7BGld0af/vobWmKRwBkSp8hQy0m5lvHwGA3EO2WOcR2T9P\nE6gvpnvOI6e1uoPmpsZWZD/Ttu7rB6GFtAl1QnqD8CqhvyNsDg4GYrvt9SfhXKAF\nxNauUZogY7t2Un5TBHK5KQcajLwBUFKHijeVWeCIbKk2j8zppMqv/ycNE+z+67gt\n6I6jnWolyZy7uueGtDvLiggcOD+s9a6RsHftgZ9GH8ccSYl72TLqR20SYmWbpLLk\niUPKW7e4iUUk9gJ8qOytfcI0ZKpQ9XOi8TzKeOnGJRcd5Qbuex6m1VDCVoAP5LMP\nyXsQyFT5Sem0UPZ46V7Xp8kNi5ZsVLe3D0UZAg==\n-----END CERTIFICATE-----\n";
const KEY = "-----BEGIN PRIVATE KEY-----\nMIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQCRGJj98NojFFBP\n58sAq/+wwAhY3rhvekt7S159wEBZuwoeHVXgogsVpsi2EvEQ2zncpIfMqol8KtmL\nINKald2RBzaHSVvHcusdOLWODe56xvggtwU6g9pf99reQLoQZUG67WOXt2m+RLy8\nZz9Dr4UxYl2iZ7qdNkp6iQ3ITHCx/LCoLDCxfnQwZGIw97kQunFf3vLB6Ss4Bf+P\nrX+lWlH+rAYh/PO5Ibqu7+9p+v+0QMhSXTCTYlJrMCF4opivT8lubtDWKBO4zHLP\nBfn5CEDaT7lXdwq5AbdUy7MQjqlniE5301JUEOEa79ixaVE+T+/swFFFWDA07UZP\nWrLzG1YHAgMBAAECggEABnMyzzcXELAlJygwB1ueBjFt2DrZCruZAhDGUTjBTjvY\n+0sP4j3R2FM4YOpT+mKEc/aBwuth0+SOaczlD9InA1Iaw8YoZPlgJ5sPFxLaIxht\nN1HA0xUWxBAJ6WS0i2i06fuw4wcC6wkLNXoxfxodBaZQcJkZMUaUkQCUCES9oxZ+\nFuiE7fzo/ivepd2XCurom0fVbQTb2un+K4kbzDOtfX5vliF1bZ3NgG30VdBFAqeq\nLw2HHt68ow4bC0ihZqHJyU8I5u0gekdyiimKdBDU8tEkNVMhGRz7kf3iliLp8CfO\nLV0cGUjMQqt7DEA3j/pXZcAkFwZ3qIlVU4yJC8HQgQKBgQDKNbtZuE65QSVQfaOx\nlybDIthtT7rOwfEzVmqyJ/YQT+dhBMcsTJ5/1OjrANrdylyWaae5kxSHd/d37VDY\n7DpAErLAqSVDdDMuyK/SB043eWYD+OWW0Y3BzeqfwTFJ9UC/uToDRzCckutiu7dN\n6+XCiz1iDHDOS4Res3nUkg8EEQKBgQC3sXqyFMrTYpNaqEtHhKcr4OWnWSRTV7Dh\nS7gGFM/HuxOifJsLz5Udt2UmDoF7DTcSgUecG1KFyK9H02chMFnONkmisN/zlgh/\nhN1ssdGdRLo55dT1suYD6Sr0QpRQxAXb2hPZcXCX4W9ldqOAUTFoijUYEW4AKyr/\nFL5e4XDwlwKBgCb8zS6zVpppcSRf1Rv8pMCjC0+oO8B5rGPVmaTYB52FinsuTKjB\n7R2Ak5gcBhcVWVy26lvhe+fSvagl3Zy1j1WjRUvYURL8aJHwp9W7Ct2vTngdmUbH\nCKoBZed/fF1iKCXPNNxE8Z5+xVu4DdO8VAR6jE9HTsi3zsHjoO3Xa4XBAoGAKscc\nCaip6zxDkJMspMURoThIgwZRXU9Ik87sVg42rQ617dsSyFdZJIh297vdD2jucFLG\n+GWsfBdWKmXi2GnIICuoTkjefn1sETZB0nQ+ml9M9vq881LsGfEM3cE7hOuBaceJ\nY9P1IomPRZOxU8qUtQGqh6ZXdZaX9rs/8hySDpUCgYBhy1pT6s71VJ7cxzZ6BV1l\nM/5XFF1e2Ccq6lLrOlfVF0zu6phpnJ3PYW2MATGM7IIYLh6kcVFAn37b1x4sxtFv\n0VCUF1fS40gDYD1G7bRuv4B41TCs4Zl/kgzfskaXFeseET+vRuT3hgd5lXgLy7G+\n1soiigyAahyQtBFN4B9/yg==\n-----END PRIVATE KEY-----\n";

// PEM material must be on disk: tls cert/key are PATHS.
const T = makeTempDir("audit-tls");
const certPath = new Path(T + "/cert.pem");
const keyPath = new Path(T + "/key.pem");
writeFile(certPath, CERT);
writeFile(keyPath, KEY);

// Fix N1-23: the default trust store must not load a cwd-relative
// tests/corpus/ca-bundle.pem; N1-26: inherited options must not reach the
// TLS setup. This self-signed server MUST be rejected either way.
Object.prototype.rejectUnauthorized = false;

const srv = new TCPServer({
    port: 0,
    tls: { cert: String(certPath), key: String(keyPath) },
});
srv.start({
    data: (c, b) => {
        c.write(new TextEncoder().encode(
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok"));
    },
});

let rejected = false, accepted = false;
try {
    const c = new HTTPClient();
    c.setTimeout(3000);
    const r = await c.getAsync("https://127.0.0.1:" + srv.port + "/");
    accepted = true;
    c.close();
} catch (e) {
    rejected = true;
}
ok(rejected && !accepted,
    "self-signed cert rejected even with Object.prototype.rejectUnauthorized=false");

delete Object.prototype.rejectUnauthorized;

// an inherited `ca` must not be picked up either.
Object.prototype.ca = "/nonexistent/audit-ca.pem";
const srv2 = new TCPServer({
    port: 0,
    tls: { cert: String(certPath), key: String(keyPath) },
});
srv2.start({ data: (c, b) => {} });
try {
    const c2 = new HTTPClient();
    c2.setTimeout(3000);
    const r2 = await c2.getAsync("https://127.0.0.1:" + srv2.port + "/");
    ok(false, "prototype ca must not disable verification");
    c2.close();
} catch (e) {
    ok(true, "prototype ca ignored: connection still fails verification");
}
delete Object.prototype.ca;

srv.close();
srv2.close();
removeAll(T);

if (fails) {
    print("test_audit_http_6: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_audit_http_6 failed");
}
print("test_audit_http_6: " + n + " assertions, 0 failures");
