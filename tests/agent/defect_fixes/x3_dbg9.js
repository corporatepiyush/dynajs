async function t() {
  let returnCount = 0;
  const syncIterator = { [Symbol.iterator]() { return {
      next() { return { value: Promise.reject("reject"), done: false }; },
      return() { returnCount += 1; } }; } };
  try { for await (let _ of syncIterator); } catch (e) { console.log("caught", e, "rc=", returnCount); }
  console.log("t-end");
}
t().then(()=>console.log("settled"), (e)=>console.log("rejected", e));
setTimeout(() => { console.log("TICK"); process.exit(0); }, 200);
