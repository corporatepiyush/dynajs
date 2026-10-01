__EXP = {};
__EXP[2] = ["iso-throw RangeError"];
__EXP[3] = ["SUMMARY fails=0 hash=6a4482b0"];
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var feed = "", fails = 0;
function chk(cond, label) { if (!cond) { fails++; __L(0, "FAIL " + label); } }
function isn(x) { return typeof x === "number" && x !== x; }
var d = new Date(NaN);
chk(isn(d.getTime()), "nan-time");
chk(isn(d.getFullYear()) && isn(d.getMonth()) && isn(d.getDate()) && isn(d.getHours()) &&
    isn(d.getMilliseconds()) && isn(d.getDay()) && isn(d.getTimezoneOffset()), "nan-fields");
chk(isn(Date.parse(d.toString())) || d.toString() === "Invalid Date", "nan-tostring");
feed += d.toString() + "|";
try { feed += d.toISOString(); } catch (e) { feed += "throw:" + e.constructor.name; }
feed += "\n";
d.setMonth(5); chk(isn(d.getTime()), "setmonth-invalid");
d.setTime(8640000000000000 + 1); chk(isn(d.getTime()), "settime-clip");
d.setTime(NaN);
var r = d.setFullYear(2024, 4, 15);
chk(typeof r === "number" && !isn(r), "setfullyear-returns-finite");
chk(d.getFullYear() === 2024 && d.getMonth() === 4 && d.getDate() === 15, "setfullyear-resurrect");
chk(d.getTime() === r, "setfullyear-return-eq");
feed += d.toISOString() + "\n";
var e = new Date(Date.UTC(2020, 0, 1));
e.getFullYear(); e.getMonth();
e.setTime(NaN);
chk(isn(e.getFullYear()) && isn(e.getMonth()), "cache-invalidated-by-nan");
feed += "e:" + e.getFullYear() + "\n";
e.setTime(0);
chk(e.getTime() === 0 && e.getFullYear() === new Date(0).getFullYear(), "nan-to-zero");
feed += "e2:" + e.toISOString() + "\n";
var arr = [new Date(NaN), new Date(0), new Date(NaN)];
feed += "JSON:" + JSON.stringify(arr) + "\n";
chk(JSON.stringify(arr) === "[null,\"1970-01-01T00:00:00.000Z\",null]", "json-invalid-null");
try { new Date(NaN).toISOString(); __L(1, "FAIL iso-no-throw"); }
catch (e) { __L(2, "iso-throw " + e.constructor.name); }
var c = new Date(8.64e15);
chk(c.getTime() === 8.64e15, "clip-max-ok");
c.setTime(8.64e15 + 1); chk(isn(c.getTime()), "clip-max-plus1");
chk(isn(c.getFullYear()), "clip-max-fields-nan");
__L(3, "SUMMARY fails=" + fails + " hash=" + fnv(feed));

summary("builtins_ext");
