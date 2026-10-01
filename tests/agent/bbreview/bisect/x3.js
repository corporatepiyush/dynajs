const o1 = { v: 1 };
function w3(n, obj) {
  with (obj) {
    try {
      if (n === 0) throw new Error("w3-boom");
      return w3(n - 1, obj);
    } catch (e) {
      return "w3-caught@" + n;
    }
  }
}
console.log("w3s", w3(5, o1));
console.log("w3d-start");
console.log("w3d", w3(100000, o1));
