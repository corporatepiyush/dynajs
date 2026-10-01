var o={a:1,b:2};
let s=0;
for(let i=0;i<1000000;i++){s+=o.a+o.b;}
console.log(s);
