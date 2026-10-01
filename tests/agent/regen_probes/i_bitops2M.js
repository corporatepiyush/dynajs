let s=0;
for(let i=0;i<2000000;i++){s+=(i<<2)+(i>>>1)+(i&255)+(i|16)+(i^85);}
console.log(s);
