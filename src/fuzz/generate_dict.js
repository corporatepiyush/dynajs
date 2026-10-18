function collectBuiltinNames(obj, visited = new Set(), result = new Set()) {
  if (visited.has(obj))
    return;

  visited.add(obj);
  const properties = Object.getOwnPropertyNames(obj);
  for (var i=0; i < properties.length; i++) {
    var property = properties[i];
    if (property != "collectBuiltinNames" && typeof property != "number")
      result.add(property);
    if (typeof obj[property] === 'object' && obj[property] !== null)
      collectBuiltinNames(obj[property], visited, result);
  }
  return result;
}

console.log(Array.from(collectBuiltinNames(this)).join('\n'));
