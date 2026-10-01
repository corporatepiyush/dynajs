__EXP = {};
__EXP[0] = ["push-sparse => [null,null,\"two\",null,null] len=5 own0=true"];
__EXP[1] = ["len-lie => [1,2] reads-length=true"];
__EXP[2] = ["call-sparse => 4:x,,,y"];
__EXP[3] = ["lit => [null,\"mid\",null] 3"];
__EXP[4] = ["1e5 => len=100000 mid=mid head=undefined tail=undefined own=true"];
__EXP[5] = ["many => len=150 [null,0,null,null,1,null] last=undefined"];
const out = typeof console !== "undefined" ? console.log : print;

{
  const a = [];
  a.length = 5;
  a[2] = "two";
  const dst = [];
  dst.push(...a);
  __L(0, "push-sparse => " + JSON.stringify(dst) + " len=" + dst.length +
      " own0=" + Object.prototype.hasOwnProperty.call(dst, 0));
}
{
  const b = {
    0: 1, 1: 2,
    get length() { return 1e9; },
    [Symbol.iterator]() {
      const self = this;
      let i = 0;
      return { next: () => (i < 2 ? { value: self[i++], done: false } : { done: true }) };
    },
  };
  const dst2 = [];
  dst2.push(...b);
  __L(1, "len-lie => " + JSON.stringify(dst2) + " reads-length=" + (b.length === 1e9));
}
{
  const c = [];
  c.length = 4;
  c[0] = "x";
  c[3] = "y";
  const f = (...args) => args.length + ":" + args.join(",");
  __L(2, "call-sparse => " + f(...c));
}
{
  const d = [];
  d.length = 3;
  d[1] = "mid";
  __L(3, "lit => " + JSON.stringify([...d]) + " " + [...d].length);
}
{
  const e = [];
  e.length = 100000;
  e[50000] = "mid";
  const r = [...e];
  __L(4, "1e5 => len=" + r.length + " mid=" + r[50000] + " head=" + r[0] +
      " tail=" + r[99999] + " own=" + Object.prototype.hasOwnProperty.call(r, 49999));
}
{
  const dst3 = [];
  for (let i = 0; i < 50; i++) {
    const s = [];
    s.length = 3;
    s[1] = i;
    dst3.push(...s);
  }
  __L(5, "many => len=" + dst3.length + " " + JSON.stringify(dst3.slice(0, 6)) + " last=" + dst3[dst3.length - 1]);
}

summary("arrays_ext");
