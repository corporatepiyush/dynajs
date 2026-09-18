// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["[WALL] 2024-2-10 1:40 t=1710015000000 h=1 m=40 off=-330 utc=2024-03-09T20:10:00.000Z", "[WALL] 2024-2-10 1:50 t=1710015600000 h=1 m=50 off=-330 utc=2024-03-09T20:20:00.000Z", "[WALL] 2024-2-10 1:60 t=1710016200000 h=2 m=0 off=-330 utc=2024-03-09T20:30:00.000Z", "[WALL] 2024-2-10 1:70 t=1710016800000 h=2 m=10 off=-330 utc=2024-03-09T20:40:00.000Z", "[WALL] 2024-2-10 1:80 t=1710017400000 h=2 m=20 off=-330 utc=2024-03-09T20:50:00.000Z", "[WALL] 2024-2-10 1:90 t=1710018000000 h=2 m=30 off=-330 utc=2024-03-09T21:00:00.000Z", "[WALL] 2024-2-10 1:100 t=1710018600000 h=2 m=40 off=-330 utc=2024-03-09T21:10:00.000Z", "[WALL] 2024-2-10 1:110 t=1710019200000 h=2 m=50 off=-330 utc=2024-03-09T21:20:00.000Z", "[WALL] 2024-2-10 1:120 t=1710019800000 h=3 m=0 off=-330 utc=2024-03-09T21:30:00.000Z", "[WALL] 2024-2-10 1:130 t=1710020400000 h=3 m=10 off=-330 utc=2024-03-09T21:40:00.000Z", "[WALL] 2024-2-10 3:0 t=1710019800000 h=3 m=0 off=-330 utc=2024-03-09T21:30:00.000Z", "[WALL] 2024-2-10 3:10 t=1710020400000 h=3 m=10 off=-330 utc=2024-03-09T21:40:00.000Z", "[WALL] 2024-2-10 3:20 t=1710021000000 h=3 m=20 off=-330 utc=2024-03-09T21:50:00.000Z", "[WALL] 2024-10-3 0:40 t=1730574600000 h=0 m=40 off=-330 utc=2024-11-02T19:10:00.000Z", "[WALL] 2024-10-3 0:50 t=1730575200000 h=0 m=50 off=-330 utc=2024-11-02T19:20:00.000Z", "[WALL] 2024-10-3 0:60 t=1730575800000 h=1 m=0 off=-330 utc=2024-11-02T19:30:00.000Z", "[WALL] 2024-10-3 0:70 t=1730576400000 h=1 m=10 off=-330 utc=2024-11-02T19:40:00.000Z", "[WALL] 2024-10-3 0:80 t=1730577000000 h=1 m=20 off=-330 utc=2024-11-02T19:50:00.000Z", "[WALL] 2024-10-3 0:90 t=1730577600000 h=1 m=30 off=-330 utc=2024-11-02T20:00:00.000Z", "[WALL] 2024-10-3 0:100 t=1730578200000 h=1 m=40 off=-330 utc=2024-11-02T20:10:00.000Z", "[WALL] 2024-10-3 0:110 t=1730578800000 h=1 m=50 off=-330 utc=2024-11-02T20:20:00.000Z", "[WALL] 2024-10-3 0:120 t=1730579400000 h=2 m=0 off=-330 utc=2024-11-02T20:30:00.000Z", "[WALL] 2024-10-3 0:130 t=1730580000000 h=2 m=10 off=-330 utc=2024-11-02T20:40:00.000Z", "[WALL] 2024-10-3 0:140 t=1730580600000 h=2 m=20 off=-330 utc=2024-11-02T20:50:00.000Z", "[WALL] 2024-10-3 2:0 t=1730579400000 h=2 m=0 off=-330 utc=2024-11-02T20:30:00.000Z", "[WALL] 2024-10-3 2:10 t=1730580000000 h=2 m=10 off=-330 utc=2024-11-02T20:40:00.000Z", "[WALL] 2024-10-3 2:20 t=1730580600000 h=2 m=20 off=-330 utc=2024-11-02T20:50:00.000Z"];
__EXP[1] = ["[WALL] cross-out t=1678395600000 off=-330"];
__EXP[2] = ["[WALL] cross-in t=1710018000000 off=-330"];
__EXP[3] = ["[WALL] setdate-gap t=1710018000000 off=-330"];
__EXP[4] = ["[WALL] hash=473a4bc6"];
// test_date_gap_fold_walltime.js — local wall-time construction inside the DST
// gap (2024-03-10 02:00-03:00 nonexistent) and fold (2024-11-03 01:00-02:00
// ambiguous). Documented impl-defined row class vs V8's heuristic (audit §22):
// rows tagged [WALL] (node diff tolerated); tree-vs-baseline must be exact.
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var feed = "";
function probe(y, mo, da, h, mi) {
  var d = new Date(y, mo, da, h, mi, 0, 0);
  var line = "[WALL] " + y + "-" + mo + "-" + da + " " + h + ":" + mi +
    " t=" + d.getTime() + " h=" + d.getHours() + " m=" + d.getMinutes() +
    " off=" + d.getTimezoneOffset() + " utc=" + d.toISOString();
  feed += line + "\n";
  __L(0, line);
}
// spring gap 2024-03-10: 01:40 .. 03:20 every 10min
for (var m = 0; m < 10; m++) probe(2024, 2, 10, 1, 40 + m * 10);
for (var m = 0; m < 3; m++)  probe(2024, 2, 10, 3, m * 10);
// fall fold 2024-11-03: 00:40 .. 02:20 every 10min
for (var m = 0; m < 11; m++) probe(2024, 10, 3, 0, 40 + m * 10);
for (var m = 0; m < 3; m++)  probe(2024, 10, 3, 2, m * 10);
// setFullYear crossing into/out of gap
var d1 = new Date(2024, 2, 10, 2, 30);
d1.setFullYear(2023); // 2023-03-10 02:30 is a VALID local time
__L(1, "[WALL] cross-out t=" + d1.getTime() + " off=" + d1.getTimezoneOffset());
feed += "[WALL] cross-out t=" + d1.getTime() + "\n";
var d2 = new Date(2023, 2, 10, 2, 30);
d2.setFullYear(2024); // lands in the gap
__L(2, "[WALL] cross-in t=" + d2.getTime() + " off=" + d2.getTimezoneOffset());
feed += "[WALL] cross-in t=" + d2.getTime() + "\n";
// setHours into the gap from a valid day
var d3 = new Date(2024, 2, 11, 2, 30);
d3.setDate(10);
__L(3, "[WALL] setdate-gap t=" + d3.getTime() + " off=" + d3.getTimezoneOffset());
feed += "[WALL] setdate-gap t=" + d3.getTime() + "\n";
__L(4, "[WALL] hash=" + fnv(feed));

summary("builtins_ext");
