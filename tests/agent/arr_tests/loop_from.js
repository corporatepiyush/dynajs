var src = new Array(1000); for (var i = 0; i < 1000; i++) src[i] = i;
var t0 = Date.now(); var sink = 0;
while (Date.now() - t0 < 8000) { for (var i = 0; i < 2000; i++) sink += Array.from(src).length; }
print(sink);
