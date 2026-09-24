// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = [["[LMT] e=-5350363200000 Y=1800 M=5 D=15 h=17 m=53 s=0 off=-353", "[LMT] e=-5350363200000 Y=1800 M=5 D=15 h=17 m=53 s=28 off=-353"], ["[LMT] e=-3852617400000 Y=1847 M=11 D=1 h=18 m=23 s=0 off=-353", "[LMT] e=-3852617400000 Y=1847 M=11 D=1 h=18 m=23 s=28 off=-353"], ["[LMT] e=-3187209600000 Y=1869 M=0 D=1 h=5 m=53 s=0 off=-353", "[LMT] e=-3187209600000 Y=1869 M=0 D=1 h=5 m=53 s=20 off=-353"], ["[LMT] e=-2717668800000 Y=1883 M=10 D=18 h=17 m=21 s=0 off=-321", "[LMT] e=-2717668800000 Y=1883 M=10 D=18 h=17 m=21 s=10 off=-321"], ["[LMT] e=-2208988801000 Y=1900 M=0 D=1 h=5 m=20 s=59 off=-321", "[LMT] e=-2208988801000 Y=1900 M=0 D=1 h=5 m=21 s=9 off=-321"], ["[LMT] e=-2208988800000 Y=1900 M=0 D=1 h=5 m=21 s=0 off=-321", "[LMT] e=-2208988800000 Y=1900 M=0 D=1 h=5 m=21 s=10 off=-321"], ["[LMT] e=-2208988770000 Y=1900 M=0 D=1 h=5 m=21 s=30 off=-321", "[LMT] e=-2208988770000 Y=1900 M=0 D=1 h=5 m=21 s=40 off=-321"]];
__EXP[1] = [["[LMT] walk 1899-12-31T23:00:00.000Z h=4 m=21 s=0 off=-321", "[LMT] walk 1899-12-31T23:00:00.000Z h=4 m=21 s=10 off=-321"], ["[LMT] walk 1899-12-31T23:10:00.000Z h=4 m=31 s=0 off=-321", "[LMT] walk 1899-12-31T23:10:00.000Z h=4 m=31 s=10 off=-321"], ["[LMT] walk 1899-12-31T23:20:00.000Z h=4 m=41 s=0 off=-321", "[LMT] walk 1899-12-31T23:20:00.000Z h=4 m=41 s=10 off=-321"], ["[LMT] walk 1899-12-31T23:30:00.000Z h=4 m=51 s=0 off=-321", "[LMT] walk 1899-12-31T23:30:00.000Z h=4 m=51 s=10 off=-321"], ["[LMT] walk 1899-12-31T23:40:00.000Z h=5 m=1 s=0 off=-321", "[LMT] walk 1899-12-31T23:40:00.000Z h=5 m=1 s=10 off=-321"], ["[LMT] walk 1899-12-31T23:50:00.000Z h=5 m=11 s=0 off=-321", "[LMT] walk 1899-12-31T23:50:00.000Z h=5 m=11 s=10 off=-321"], ["[LMT] walk 1900-01-01T00:00:00.000Z h=5 m=21 s=0 off=-321", "[LMT] walk 1900-01-01T00:00:00.000Z h=5 m=21 s=10 off=-321"], ["[LMT] walk 1900-01-01T00:10:00.000Z h=5 m=31 s=0 off=-321", "[LMT] walk 1900-01-01T00:10:00.000Z h=5 m=31 s=10 off=-321"], ["[LMT] walk 1900-01-01T00:20:00.000Z h=5 m=41 s=0 off=-321", "[LMT] walk 1900-01-01T00:20:00.000Z h=5 m=41 s=10 off=-321"], ["[LMT] walk 1900-01-01T00:30:00.000Z h=5 m=51 s=0 off=-321", "[LMT] walk 1900-01-01T00:30:00.000Z h=5 m=51 s=10 off=-321"], ["[LMT] walk 1900-01-01T00:40:00.000Z h=6 m=1 s=0 off=-321", "[LMT] walk 1900-01-01T00:40:00.000Z h=6 m=1 s=10 off=-321"], ["[LMT] walk 1900-01-01T00:50:00.000Z h=6 m=11 s=0 off=-321", "[LMT] walk 1900-01-01T00:50:00.000Z h=6 m=11 s=10 off=-321"], ["[LMT] walk 1900-01-01T01:00:00.000Z h=6 m=21 s=0 off=-321", "[LMT] walk 1900-01-01T01:00:00.000Z h=6 m=21 s=10 off=-321"]];
__EXP[2] = [["[LMT] rt t=-2717706060000 s=0 off=-321", "[LMT] rt t=-2717706070000 s=0 off=-321"]];
__EXP[3] = [["[LMT] hash=c79428c6", "[LMT] hash=754779"]];
__REQ = {"dynajs": {"0": 7, "1": 13, "2": 1, "3": 1}, "node": {"0": 7, "1": 13, "2": 1, "3": 1}};
// test_date_pre1900_lmt.js — pre-1900 local times: the engine drops the
// sub-minute LMT offset (tm_gmtoff/60 truncation, 10-28s observed).
// DOCUMENTED PRE-EXISTING DIVERGENCE vs node (V8 keeps full seconds) —
// rows tagged [LMT]; hard requirement: tree must be byte-identical to the
// pre-change BASELINE here (no Date change crossed this line).
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var feed = "";
var epochs = [
  Date.UTC(1800, 5, 15, 12, 0, 0),
  Date.UTC(1847, 11, 1, 12, 30, 0),
  Date.UTC(1869, 0, 1, 0, 0, 0),
  Date.UTC(1883, 10, 18, 12, 0, 0),
  Date.UTC(1899, 11, 31, 23, 59, 59),
  Date.UTC(1900, 0, 1, 0, 0, 0),
  Date.UTC(1900, 0, 1, 0, 0, 30)
];
for (var i = 0; i < epochs.length; i++) {
  var d = new Date(epochs[i]);
  var line = "[LMT] e=" + epochs[i] +
    " Y=" + d.getFullYear() + " M=" + d.getMonth() + " D=" + d.getDate() +
    " h=" + d.getHours() + " m=" + d.getMinutes() + " s=" + d.getSeconds() +
    " off=" + d.getTimezoneOffset();
  feed += line + "\n";
  __L(0, line);
}
// hourly walk across the 1900 boundary
for (var m = -6; m <= 6; m++) {
  var d = new Date(Date.UTC(1900, 0, 1, 0, m * 10, 0));
  var line = "[LMT] walk " + d.toISOString() + " h=" + d.getHours() + " m=" + d.getMinutes() + " s=" + d.getSeconds() + " off=" + d.getTimezoneOffset();
  feed += line + "\n";
  __L(1, line);
}
// round-trip: new Date(y,m,d,h,mi,s) from pre-1900 local fields
var d0 = new Date(1883, 10, 18, 7, 0, 0);
feed += "[LMT] rt t=" + d0.getTime() + " s=" + d0.getSeconds() + " off=" + d0.getTimezoneOffset() + "\n";
__L(2, "[LMT] rt t=" + d0.getTime() + " s=" + d0.getSeconds() + " off=" + d0.getTimezoneOffset());
__L(3, "[LMT] hash=" + fnv(feed));

summary("builtins_ext");
