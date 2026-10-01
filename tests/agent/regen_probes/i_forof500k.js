var arr=[1,2,3,4,5];
let s=0;
for(let i=0;i<100000;i++){for(var v of arr){s+=v;}}
console.log(s);
