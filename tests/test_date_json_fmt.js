function eq(a, b, m) {
    if (a !== b) {
        print("FAIL " + m + ": got " + JSON.stringify(a) + " want " + JSON.stringify(b));
        throw new Error(m);
    }
}

/* ---- Date string formats: output must be byte-identical to the snprintf
   version these replaced (get_date_string in src/builtins/date.inc.c). ---- */
var d = new Date(Date.UTC(2018, 0, 2, 23, 2, 56, 927));
eq(d.toISOString(), "2018-01-02T23:02:56.927Z", "iso");
eq(d.toUTCString(), "Tue, 02 Jan 2018 23:02:56 GMT", "utc");
eq(d.toJSON(), "2018-01-02T23:02:56.927Z", "json");

/* zero / boundary field values exercise every digit emitter */
eq(new Date(Date.UTC(2000, 0, 1, 0, 0, 0, 0)).toISOString(),
   "2000-01-01T00:00:00.000Z", "zeros");
eq(new Date(Date.UTC(1999, 11, 31, 23, 59, 59, 999)).toISOString(),
   "1999-12-31T23:59:59.999Z", "nines");
eq(new Date(Date.UTC(2018, 0, 2, 1, 2, 3, 4)).toISOString(),
   "2018-01-02T01:02:03.004Z", "ms_pad_1");
eq(new Date(Date.UTC(2018, 0, 2, 1, 2, 3, 45)).toISOString(),
   "2018-01-02T01:02:03.045Z", "ms_pad_2");
eq(new Date(Date.UTC(2018, 0, 2, 1, 2, 3, 456)).toISOString(),
   "2018-01-02T01:02:03.456Z", "ms_pad_3");

/* every month/day name index */
for (var mo = 0; mo < 12; mo++) {
    var dm = new Date(Date.UTC(2018, mo, 15, 0, 0, 0));
    eq(dm.toISOString().slice(5, 7), ("0" + (mo + 1)).slice(-2), "isoMonth" + mo);
    eq(dm.toUTCString().length, 29, "utcLen" + mo);
}
for (var dy = 1; dy <= 7; dy++) {
    var dd = new Date(Date.UTC(2018, 0, dy));
    eq(dd.toUTCString().slice(0, 3), "SunMonTueWedThuFriSat".substr(dd.getUTCDay() * 3, 3),
       "dayName" + dy);
}

/* extended / negative years take the snprintf fallback -- must still match */
eq(new Date(Date.UTC(275760, 8, 13)).toISOString(),
   "+275760-09-13T00:00:00.000Z", "yearMax");
eq(new Date(-8640000000000000).toISOString(),
   "-271821-04-20T00:00:00.000Z", "yearMin");
var y0 = new Date(Date.UTC(2000, 0, 1));
y0.setUTCFullYear(0);
eq(y0.toISOString().slice(0, 4), "0000", "year0");
var yneg = new Date(Date.UTC(2000, 0, 1));
yneg.setUTCFullYear(-45);
eq(yneg.toISOString().slice(0, 7), "-000045", "yearNeg");
eq(yneg.toUTCString().indexOf("-0045") >= 0, true, "utcYearNeg: " + yneg.toUTCString());

/* toString / toLocaleString shapes (timezone-dependent, so match structure) */
var ts = d.toString();
eq(/^[A-Z][a-z]{2} [A-Z][a-z]{2} \d{2} \d{4} \d{2}:\d{2}:\d{2} GMT[+-]\d{4}$/.test(ts),
   true, "toString: " + ts);
var tl = d.toLocaleString();
eq(/^\d{2}\/\d{2}\/\d{4}, \d{2}:\d{2}:\d{2} [AP]M$/.test(tl), true, "toLocaleString: " + tl);
eq(/^\d{2}\/\d{2}\/\d{4}$/.test(d.toLocaleDateString()), true, "toLocaleDateString");
eq(/^\d{2}:\d{2}:\d{2} [AP]M$/.test(d.toLocaleTimeString()), true, "toLocaleTimeString");
eq(new Date(NaN).toString(), "Invalid Date", "invalid");

/* 12-hour wrap for toLocaleString */
eq(new Date(2018, 0, 2, 0, 0, 0).toLocaleTimeString(), "12:00:00 AM", "12am");
eq(new Date(2018, 0, 2, 12, 0, 0).toLocaleTimeString(), "12:00:00 PM", "12pm");
eq(new Date(2018, 0, 2, 13, 5, 9).toLocaleTimeString(), "01:05:09 PM", "1pm");
eq(new Date(2018, 0, 2, 23, 59, 59).toLocaleTimeString(), "11:59:59 PM", "11pm");

/* ---- JSON \uXXXX escape path (was snprintf) ---- */
var NUL = String.fromCharCode(0);
eq(JSON.stringify(NUL), '"\\u0000"', "esc0");
eq(JSON.stringify(String.fromCharCode(0x1f)), '"\\u001f"', "esc1f");
eq(JSON.stringify(String.fromCharCode(0x0b)), '"\\u000b"', "esc0b");
eq(JSON.stringify("a" + String.fromCharCode(1) + "b" + String.fromCharCode(2) + "c"),
   '"a\\u0001b\\u0002c"', "escMixed");
eq(JSON.stringify(String.fromCharCode(0xd800)), '"\\ud800"', "escLoneHi");
eq(JSON.stringify(String.fromCharCode(0xdfff)), '"\\udfff"', "escLoneLo");
eq(JSON.stringify("\t\r\n\b\f\"\\"), '"\\t\\r\\n\\b\\f\\"\\\\"', "escShort");
eq(JSON.stringify("\u{1f600}"), '"\u{1f600}"', "astralPair");

/* every control code point round-trips and produces the exact lowercase hex */
var all = "";
for (var i = 0; i < 32; i++) all += String.fromCharCode(i);
eq(JSON.parse(JSON.stringify(all)), all, "escRoundTrip");
var shortEsc = { 8: "\\b", 9: "\\t", 10: "\\n", 12: "\\f", 13: "\\r" };
for (var i = 0; i < 32; i++) {
    var want = i in shortEsc ? '"' + shortEsc[i] + '"'
             : '"\\u00' + (i < 16 ? "0" : "1") + "0123456789abcdef"[i & 15] + '"';
    eq(JSON.stringify(String.fromCharCode(i)), want, "escHex" + i);
}
for (var i = 0xd800; i <= 0xdfff; i += 37) {
    var want = '"\\u' + i.toString(16) + '"';
    eq(JSON.stringify(String.fromCharCode(i)), want, "escSurr" + i);
}

/* ---- JSON bulk-scan boundaries (json_clean_run8 / json_parse_string).
   The scan advances 8 bytes at a time, so the interesting cases are strings
   whose special byte sits at every offset around a block boundary. ---- */
var specials = ['"', "\\", String.fromCharCode(1), String.fromCharCode(0x1f), "é", "☃"];
for (var si = 0; si < specials.length; si++) {
    for (var off = 0; off <= 20; off++) {
        var s = "";
        for (var k = 0; k < off; k++) s += "a";
        s += specials[si];
        for (var k = 0; k < 20 - off; k++) s += "b";
        eq(JSON.parse(JSON.stringify(s)), s, "scanRT_" + si + "_" + off);
        eq(JSON.parse(JSON.stringify({ x: s })).x, s, "scanRTObj_" + si + "_" + off);
        eq(JSON.parse(JSON.stringify([s]))[0], s, "scanRTArr_" + si + "_" + off);
    }
}
/* clean runs at every length across several block boundaries */
for (var len = 0; len <= 40; len++) {
    var s = "";
    for (var k = 0; k < len; k++) s += "abcdefgh"[k % 8];
    eq(JSON.parse(JSON.stringify(s)), s, "cleanRun" + len);
    eq(JSON.parse('"' + s + '"'), s, "cleanParse" + len);
}
/* multi-byte UTF-8 must still go through the per-character path */
eq(JSON.parse('"é☃\u{1f600}"'), "é☃\u{1f600}", "utf8Parse");
eq(JSON.parse('"abcdefgh☃abcdefgh"'), "abcdefgh☃abcdefgh", "utf8Straddle");
/* unterminated / bad escapes must still throw, not silently truncate */
function throws(src, m) {
    var t = false;
    try { JSON.parse(src); } catch (e) { t = true; }
    if (!t) { print("FAIL " + m + ": expected throw for " + src); throw new Error(m); }
}
throws('"abcdefgh', "unterminated8");
throws('"abcdefghi', "unterminated9");
throws('"abcdefghijklmnop', "unterminated16");
throws('"abc\\', "danglingEscape");
throws('"abc\\uZZZZ"', "badUnicodeEscape");
throws('"abc' + String.fromCharCode(10) + '"', "rawControl");

print("date/json fmt: ok");
