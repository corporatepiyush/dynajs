function f(a){return a+1;}
let s=0;
for(let i=0;i<500000;i++){s+=f(i);}
console.log(s);
