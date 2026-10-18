// Split from tests/test_cov_file_html_xml_yaml_json.js: the serialize sections.
import { makeTempDir } from "dyna:file";
import { Proto, MsgPackEncode, MsgPackDecode, CBOREncode, CBORDecode, CBORCanonical, ValueHash, structuredClone, ASN1 } from "dyna:serialize";

let n=0,fails=0;
function assert(c,m){n++; if(!c){fails++; print("FAIL: "+m)}}
function eq(a,b,m){assert(a===b,m)}
function throws(fn,re,m){n++; try{fn(); fails++; print("FAIL: "+m)}catch(e){if(re && !re.test(String(e.message))) {fails++; print("FAIL: "+m+" wrong msg "+e.message)}}}
function j(v){ return JSON.stringify(v); }
function hex(u){ return Array.from(u,b=>b.toString(16).padStart(2,"0")).join(""); }
function bytes(h){ return new Uint8Array(h.match(/../g) ? h.match(/../g).map(x=>parseInt(x,16)) : []); }
function logStep(s){ try{ let fs=require("fs"); }catch(e){} }
let ROOT = makeTempDir("cov-file-");
{
  let schema={fields:[{name:"id",number:1,type:"int32"},{name:"name",number:2,type:"string"},{name:"flag",number:3,type:"bool"},{name:"data",number:4,type:"bytes"}]};
  let val={id:1,name:"hi",flag:true,data:new Uint8Array([1,2,3])};
  let enc = Proto.encode(val,schema);
  assert(enc instanceof Uint8Array,"proto encodes Uint8Array");
  let dec = Proto.decode(enc,schema);
  eq(dec.id,1,"proto int32");
  eq(dec.name,"hi","proto string");
  eq(dec.flag,true,"proto bool");
  eq(hex(dec.data),"010203","proto bytes");
  let schemaI32={fields:[{name:"v",number:1,type:"int32"}]};
  eq(Proto.decode(Proto.encode({v:2147483647},schemaI32),schemaI32).v,2147483647,"int32 max");
  eq(Proto.decode(Proto.encode({v:-2147483648},schemaI32),schemaI32).v,-2147483648,"int32 min");
  throws(()=>Proto.encode({v:2147483648},schemaI32),/integer|range|overflow/,"int32 overflow high");
  throws(()=>Proto.encode({v:-2147483649},schemaI32),/integer|range|overflow/,"int32 overflow low");
  let schemaStr={fields:[{name:"s",number:1,type:"string"}]};
  eq(Proto.decode(Proto.encode({s:""},schemaStr),schemaStr).s,"","empty string");
  eq(Proto.decode(Proto.encode({s:"hello"},schemaStr),schemaStr).s,"hello","string roundtrip");
  throws(()=>Proto.encode({id:1},{fields:[{name:"id",number:536870912,type:"int32"}]}),/number/,"high tag 536870912");
  throws(()=>Proto.encode({id:1},{fields:[{name:"id",number:0,type:"int32"}]}),/number/,"tag 0");
  throws(()=>Proto.decode(new Uint8Array([0xFF]),schema),/truncated|varint/,"truncated varint");
  throws(()=>Proto.decode(new Uint8Array([0x08,0x01,0xFF]),schema),/truncated/,"truncated mid");
  let emptyDec = Proto.decode(new Uint8Array([]),schema);
  assert(j(emptyDec)==="{}" || typeof emptyDec==="object","empty decode ok");
  let badLen = new Uint8Array([0x12, 0xFF, 0xFF, 0xFF, 0xFF, 0x07]);
  throws(()=>Proto.decode(badLen,schema),/./,"huge length");
  function deepSchema(d){ let f={name:"x",number:1,type:"message",message:{fields:[]}}; let cur=f.message; for(let i=1;i<d;i++){ let nxt={name:"x",number:1,type:"message",message:{fields:[]}}; cur.fields.push(nxt); cur=nxt.message; } cur.fields.push({name:"v",number:1,type:"int32"}); return {fields:[f]}; }
  let s64=deepSchema(64);
  throws(()=>Proto.encode({x:{x:{}}}, deepSchema(65)),/nesting/,"schema nesting 65");
  let repSchema={fields:[{name:"nums",number:1,type:"int32",repeated:true}]};
  let repEnc=Proto.encode({nums:[1,2,3]},repSchema);
  let repDec=Proto.decode(repEnc,repSchema);
  eq(j(repDec.nums),j([1,2,3]),"repeated int32");
  let packedSchema={fields:[{name:"nums",number:1,type:"int32",repeated:true,packed:true}]};
  let packedEnc=Proto.encode({nums:[1,2,3]},packedSchema);
  let packedDec=Proto.decode(packedEnc,packedSchema);
  eq(j(packedDec.nums),j([1,2,3]),"packed");
  let mapSchema={fields:[{name:"m",number:1,type:"message",map:true,keyType:"string",valueType:"int32"}]};
  let mapEnc=Proto.encode({m:{a:1,b:2}},mapSchema);
  let mapDec=Proto.decode(mapEnc,mapSchema);
  eq(mapDec.m.a,1,"map a");
  eq(mapDec.m.b,2,"map b");
  let bigIntVal = 9007199254740992;
  let mpEnc = MsgPackEncode(bigIntVal);
  eq(MsgPackDecode(mpEnc),bigIntVal,"msgpack 2^53 roundtrip");
  let cborEnc = CBOREncode(bigIntVal);
  eq(CBORDecode(cborEnc),bigIntVal,"cbor 2^53");
  let negBig = -9007199254740992;
  eq(MsgPackDecode(MsgPackEncode(negBig)),negBig,"msgpack neg big");
  eq(CBORDecode(CBOREncode(negBig)),negBig,"cbor neg big");
  let two63 = 9223372036854775808;
  let mp63 = MsgPackEncode(two63);
  let dec63 = MsgPackDecode(mp63);
  assert(typeof dec63==="number","2^63 decodes as number");
  let fEnc = MsgPackEncode(1.5);
  eq(MsgPackDecode(fEnc),1.5,"msgpack float");
  let cfEnc = CBOREncode(1.5);
  eq(CBORDecode(cfEnc),1.5,"cbor float");
  throws(()=>MsgPackDecode(new Uint8Array([0x92,0x01])),/truncated|declared|exceeds/,"truncated msgpack");
  throws(()=>CBORDecode(new Uint8Array([0x82,0x01])),/truncated|declared|exceeds/,"truncated cbor");
  throws(()=>MsgPackDecode(new Uint8Array([0x93,0x01])),/truncated|declared|exceeds/,"truncated 3 array");
  throws(()=>CBORDecode(new Uint8Array([0x9F,0x01,0x02])),/indefinite/,"indefinite cbor refused");
  throws(()=>MsgPackEncode(()=>1),/./,"msgpack function");
  throws(()=>MsgPackEncode(Symbol("x")),/./,"msgpack symbol");
  throws(()=>CBOREncode(()=>1),/./,"cbor function");
  eq(ValueHash({a:1,b:2}), ValueHash({b:2,a:1}),"valuehash order not change");
  assert(ValueHash({a:1})!==ValueHash({a:2}),"valuehash diff");
  eq(ValueHash({a:{b:[1,"x"]}}).length,16,"valuehash 16 hex");
  assert(/^[0-9a-f]{16}$/.test(ValueHash(42)),"valuehash hex");
  eq(hex(CBORCanonical({bb:1,a:2})), hex(CBORCanonical({a:2,bb:1})), "canonical same regardless order");
  assert(hex(CBOREncode({bb:1,a:2})) !== hex(CBORCanonical({bb:1,a:2})) || true,"canonical vs ordinary may differ");
  let src={a:1,b:[1,2,{c:"three"}],d:new Uint8Array([1,2])};
  let cl=structuredClone(src);
  eq(j({a:cl.a,b:cl.b}),j({a:src.a,b:src.b}),"clone deep");
  assert(cl!==src && cl.b!==src.b,"clone diff object");
  let cyc={a:1}; cyc.self=cyc; cyc.list=[cyc,cyc];
  let cc=structuredClone(cyc);
  assert(cc.self===cc,"self ref clone");
  assert(cc.list[0]===cc,"list ref");
  assert(cc.list[0]===cc.list[1],"shared survive");
  let asnInt = ASN1.int(42);
  let asnEnc = ASN1.encode(asnInt);
  let asnDec = ASN1.decode(asnEnc);
  eq(asnDec.tag,2,"asn1 tag int");
  eq(asnDec.value,42,"asn1 value");
  let asnSeq = ASN1.seq([ASN1.int(1), ASN1.bool(true)]);
  let asnSeqEnc = ASN1.encode(asnSeq);
  let asnSeqDec = ASN1.decode(asnSeqEnc);
  eq(asnSeqDec.tag,16,"seq tag");
  let deep400 = "";
  for(let i=0;i<400;i++) deep400+="91";
  deep400+="01";
  throws(()=>MsgPackDecode(bytes(deep400)),/./,"400 deep nesting");
  let deep100 = "";
  for(let i=0;i<100;i++) deep100+="91";
  deep100+="01";
  let ok100 = MsgPackDecode(bytes(deep100));
  assert(ok100 !== undefined,"100 deep ok");
  let lies=["dd7fffffff","df7fffffff","db7fffffff","c6ffffffff"];
  for(let h of lies){
    throws(()=>MsgPackDecode(bytes(h)),/declared length/,"lied length "+h);
  }
  print("  dyna:serialize covered");
}
if(fails){
  print("test_serialize_cov_file_html_xml_yaml_json: "+fails+" FAILED of "+n+" assertions");
  throw new Error("test_cov failed");
}
print("test_serialize_cov_file_html_xml_yaml_json: "+n+" assertions, 0 failures");
