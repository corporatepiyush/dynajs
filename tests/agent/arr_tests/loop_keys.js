var arr = new Array(1000); for (var i = 0; i < 1000; i++) arr[i] = i;
var t0 = Date.now(); var sink = 0;
while (Date.now() - t0 < 8000) {
  for (var j = 0; j < 500; j++) { var it = arr.keys(); var o; while (!(o = it.next()).done) sink += o.value; }
}
print(sink);
