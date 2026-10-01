// flags: --std
let n = 0, err = 0;
function t(u, base) {
    n++;
    try {
        const o = base !== undefined ? new URL(u, base) : new URL(u);
        return "" + o;
    } catch (e) {
        err++;
        return "ERR";
    }
}

t("file:");
t("file:?q");
t("file:#f");
t("file:/");
t("file://");
t("file:///a");
t("file:../x", "file:///dir/");
t("file:", "file:///dir/");
t("file:/", "file:///dir/");
t("file:/", "file://h/");
t("file:/", "http://h/");
t("file://", "file://h/");
t("//h2:81/p", "file:///dir/");
t("//u:p@h2:81/p?q#f", "file:///dir/");
t("http://u:p@h:8080/", "http://base:9/");
t("?q", "http://u:p@h:8080/b/c");
t("#f", "http://u:p@h:8080/b/c");
t("//n:h/", "http://u:p@h:8080/b/c");
t("file:c|/x");
t("file:c|/x", "file://server/share/");
t("file:./x", "file://server/share/y");
t("file://host", "file:other");
t("file:/x", "file:");
t("file:/x", "file:?q");
t("file:/x", "file:#f");
t("file:", "file:");
t("file:", "file:?q");
t("//", "file:///a");
t("//@:", "http://a/");
t("//@:", "file:///a");

t("file:///c|/x");
t("file:///c:/x", "file:///d|/y");
t("file:/x", "file:///c:/y");

const pre = ["", "file:", "file:/", "file://", "file:///", "FILE:", "File://",
             "//", "http:", "http://", "https://h", "http://u:p@h:1",
             "http://h:65536/", "http://h:0/", "//[::1]:80/", "c|/x",
             "file:c|/x", "file://h", "//h:", "//h:99999999999999999999",
             "http://h:99999999999999999999/", "  http://h/ ", "\thttp://h/",
             "http://[::1/", "http://h/%zz", "http://h/\u0000x"];
const suf = ["", ":", ":80", ":65535", ":65536", ":", "::", ":x", "@",
             "@h", "u@", "u:p@", "u:p:extra@", "/p", "/p//q", "?q", "#f",
             "/p?q#f", "/", "//", "///", "%", "%2", "\u0000", " ", "h"];
const bases = [undefined, "http://b/", "file:///d/", "file:", "file://s/share/",
               "http://u:p@b:81/x/y", "//b/", "http://b:0/", ""];
for (const b of bases)
    for (const p of pre)
        for (const s of suf)
            t(p + s, b);

let seed = 42;
const rnd = () => (seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x80000000;
const alpha = "file:/?#@[]:;%.\u0000 abcp|\\";
for (let i = 0; i < 20000; i++) {
    const len = Math.floor(rnd() * 12);
    let s2 = "";
    for (let j = 0; j < len; j++) s2 += alpha[Math.floor(rnd() * alpha.length)];
    t(s2, rnd() < 0.5 ? undefined : "file:///d/");
}
print("probe C: " + n + " parses, " + err + " refusals (refusals are fine)");
if (n < 20000) throw new Error("url parse fuzz: corpus did not run (n=" + n + ")");
