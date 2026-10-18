// flags: --std
// Split from tests/test_cov_net_http_url_validate.js: the schema sections.
import "../lib/httpc.js";
import { Schema } from "dyna:schema";
import * as std from "std";

let n=0,fails=0;
function assert(c,m){ n++; if(!c){ fails++; print("FAIL: "+m); } }
function eq(a,b,m){ assert(a===b, m+" (got "+JSON.stringify(a)+", want "+JSON.stringify(b)+")"); }
function throws(fn,re,m){ n++; try{ fn(); fails++; print("FAIL: "+m+" did not throw"); }catch(e){ if(re && !re.test(String(e.message))){ fails++; print("FAIL: "+m+" wrong msg: "+e.message); } } }
function throwsMsg(fn,substr,m){ n++; try{ fn(); fails++; print("FAIL: "+m+" did not throw"); }catch(e){ if(String(e).indexOf(substr)<0){ fails++; print("FAIL: "+m+" want containing "+substr+" got "+String(e).slice(0,120)); } } }
function log(s){ try{ const f=std.open(`${std.getenv("TMPDIR") || "/tmp"}/dj_cov_net.${Date.now() % 10000000}.log`,"a"); f.puts(new Date().toISOString().slice(11,19)+" "+s+"\n"); f.close(); }catch(e){} }
log("net addresses start");
log("validate done");
log("schema start");
{
  function valid(s,i){ return Schema.validate(s,i).valid; }
  function errs(s,i){ return Schema.validate(s,i).errors; }
  assert(valid({},{}),"empty schema accepts anything");
  assert(valid({},42),"empty schema number");
  assert(valid(true,42),"true schema");
  assert(!valid(false,42),"false schema");
  assert(!valid({type:"string"},5),"type string vs number");
  assert(valid({type:"string"},""),"type string empty");
  assert(valid({type:"number"},3.5),"type number");
  assert(!valid({type:"number"},NaN),"NaN not number");
  assert(!valid({type:"number"},Infinity),"Infinity not number");
  assert(valid({type:"integer"},3),"integer");
  assert(!valid({type:"integer"},3.5),"3.5 not integer");
  assert(valid({type:["string","number"]},"x"),"union string");
  assert(valid({type:["string","number"]},5),"union number");
  assert(!valid({type:["string","number"]},true),"union reject bool");
  assert(valid({type:"object"},{}),"object");
  assert(!valid({type:"object"},[]),"array not object");
  assert(valid({const:5},5),"const 5");
  assert(!valid({const:5},6),"const mismatch");
  assert(valid({enum:[1,2,3]},2),"enum member");
  assert(!valid({enum:[1,2,3]},4),"enum miss");
  assert(valid({enum:[{a:1},"x"]},{a:1}),"enum deep");
  assert(!valid({enum:[{a:1}]},{a:2}),"enum deep miss");
  assert(valid({minimum:5},5),"minimum inclusive");
  assert(!valid({minimum:5},4.9),"minimum violation");
  assert(valid({maximum:5},5),"maximum inclusive");
  assert(!valid({maximum:5},5.1),"maximum violation");
  assert(valid({exclusiveMinimum:5},5.01),"exclMin");
  assert(!valid({exclusiveMinimum:5},5),"exclMin edge");
  assert(valid({multipleOf:2},10),"multipleOf");
  assert(!valid({multipleOf:2},7),"multipleOf fail");
  assert(valid({minLength:2},"ab"),"minLength");
  assert(!valid({minLength:2},"a"),"minLength fail");
  assert(valid({maxLength:2},"ab"),"maxLength");
  assert(!valid({maxLength:2},"abc"),"maxLength fail");
  assert(!valid({minLength:2},"😀"),"surrogate minLength 2 fails for single emoji (counts code points)");
  assert(valid({minLength:1},"😀"),"emoji minLength 1 passes");
  assert(valid({minLength:1, maxLength:4},"café"),"café 4 chars");
  assert(!valid({maxLength:3},"café"),"café exceeds 3");
  assert(valid({minLength:1},"a\uD800"),"lone surrogate counts as 1?");
  assert(valid({pattern:"^a*$"},"aaa"),"pattern ^a*$");
  assert(!valid({pattern:"^a*$"},"ba"),"pattern fail");
  assert(valid({pattern:"a.c"},"a\u00E9c"),"dot matches code point");
  assert(valid({pattern:"a.c"},"a\u{1D306}c"),"dot matches surrogate pair");
  throws(()=>Schema.compile({pattern:"["}),null,"invalid pattern throws at compile");
  assert(valid({minItems:2},[1,2]),"minItems pass");
  assert(!valid({minItems:2},[1]),"minItems fail");
  assert(valid({maxItems:2},[1,2]),"maxItems pass");
  assert(!valid({maxItems:2},[1,2,3]),"maxItems fail");
  assert(valid({uniqueItems:true},[1,2,3]),"uniqueItems distinct");
  assert(!valid({uniqueItems:true},[1,2,1]),"uniqueItems duplicate");
  assert(valid({uniqueItems:true},[{a:1},{a:2}]),"uniqueItems distinct objects");
  assert(!valid({uniqueItems:true},[{a:1},{a:1}]),"uniqueItems duplicate objects");
  assert(valid({minProperties:2},{a:1,b:2}),"minProperties pass");
  assert(!valid({minProperties:2},{a:1}),"minProperties fail");
  assert(valid({maxProperties:2},{a:1,b:2}),"maxProperties pass");
  assert(!valid({maxProperties:2},{a:1,b:2,c:3}),"maxProperties fail");
  assert(valid({dependentRequired:{a:["b","c"]}},{a:1,b:2,c:3}),"dependentRequired satisfied");
  assert(!valid({dependentRequired:{a:["b","c"]}},{a:1,b:2}),"dependentRequired missing");
  let schReq={type:"object", properties:{a:{type:"string"}}, required:["a"]};
  assert(valid(schReq,{a:"hi"}),"required pass");
  assert(!valid(schReq,{}),"required fail");
  let schAdd={type:"object", properties:{a:{type:"string"}}, additionalProperties:false};
  assert(valid(schAdd,{a:"hi"}),"additionalProperties pass");
  assert(!valid(schAdd,{a:"hi",b:1}),"additionalProperties false fail");
  let schAddTrue={type:"object", properties:{a:{type:"string"}}, additionalProperties:{type:"number"}};
  assert(valid(schAddTrue,{a:"hi",b:1}),"additionalProperties schema pass");
  assert(!valid(schAddTrue,{a:"hi",b:"str"}),"additionalProperties schema fail");
  assert(valid({allOf:[{type:"integer"},{minimum:0}]},5),"allOf pass");
  assert(!valid({allOf:[{type:"integer"},{minimum:0}]},-1),"allOf fail");
  assert(valid({anyOf:[{type:"string"},{type:"number"}]},"abc"),"anyOf string");
  assert(valid({anyOf:[{type:"string"},{type:"number"}]},123),"anyOf number");
  assert(!valid({anyOf:[{type:"string"},{type:"number"}]},true),"anyOf fail");
  assert(valid({oneOf:[{type:"integer"},{minimum:5}]},3),"oneOf first only");
  assert(!valid({oneOf:[{type:"integer"},{minimum:5}]},6),"oneOf both fails");
  assert(!valid({oneOf:[{type:"integer"},{type:"boolean"}]},"abc"),"oneOf neither");
  assert(valid({not:{type:"string"}},123),"not pass");
  assert(!valid({not:{type:"string"}},"abc"),"not fail");
  let cond={if:{properties:{kind:{const:"num"}}, required:["kind"]}, then:{properties:{val:{type:"number"}}, required:["val"]}, else:{properties:{val:{type:"string"}}, required:["val"]}};
  assert(valid(cond,{kind:"num",val:42}),"if then pass");
  assert(!valid(cond,{kind:"num",val:"x"}),"if then fail");
  assert(valid(cond,{kind:"str",val:"x"}),"else pass");
  assert(!valid(cond,{kind:"str",val:42}),"else fail");
  let treeSchema={ $defs:{node:{type:"object", properties:{val:{type:"integer"}, left:{$ref:"#/$defs/node"}, right:{$ref:"#/$defs/node"}}, required:["val"]}}, $ref:"#/$defs/node"};
  let compiledTree=Schema.compile(treeSchema);
  assert(compiledTree.validate({val:1, left:{val:2}, right:{val:3, left:{val:4}}}).valid,"recursive tree valid");
  assert(!compiledTree.validate({val:1, left:{val:"bad"}}).valid,"recursive tree invalid");
  let errRes=Schema.validate({type:"object", properties:{users:{type:"array", items:{type:"object", properties:{age:{type:"integer", minimum:0}}, required:["age"]}}}},{users:[{age:25},{age:-5}]});
  assert(!errRes.valid,"nested fails");
  assert(errRes.errors.length>0,"errors generated");
  assert(errRes.errors[0].path==="/users/1/age","RFC6901 path "+errRes.errors[0].path);
  assert(errRes.errors[0].keyword==="minimum","keyword minimum");
  let cacheSch={type:"string", minLength:2};
  let c1=Schema.compile(cacheSch);
  let c2=Schema.compile(cacheSch);
  assert(c1.validate("ab").valid,"cache compile ab");
  assert(!c1.validate("a").valid,"cache compile a fail");
  assert(Schema.validate(c1,"ab").valid,"validate with compiled");
  assert(!Schema.validate(c1,"a").valid,"validate compiled fail");
  assert(valid('{"type":"number"}',5),"JSON string schema");
  throws(()=>Schema.compile('[1,2]'),null,"array not schema throws");
  throws(()=>Schema.compile("not json"),null,"unparsable string throws");
  assert(valid({minLength:0},"",),"minLength 0 empty");
  assert(!valid({minLength:1},"",),"minLength 1 empty fail");
}
let dnsChecked=false;
let fetchDone=false;
let finished=false;
function finish(){
  if(finished) return;
  if(!dnsChecked || !fetchDone){
    setTimeout(finish,50);
    return;
  }
  finished=true;
  if(fails===0) print("test_schema_cov_net_http_url_validate: all "+n+" checks passed");
  else print("test_schema_cov_net_http_url_validate: "+fails+" FAILED of "+n+" assertions");
  log("done n="+n+" fails="+fails);
  if(fails) throw new Error("test_schema_cov_net_http_url_validate failed");
}

dnsChecked = true;
fetchDone = true;
finish();
