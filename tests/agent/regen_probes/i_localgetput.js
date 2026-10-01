let s=0, x=0;
for(let i=0;i<2000000;i++){x=i&1023;s+=x;}
console.log(s);
