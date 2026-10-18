// Split from tests/test_cov_file_html_xml_yaml_json.js: the file sections.
import { Path, File, FileReader, FileWriter, FileLock, Watcher, Glob, readFile, writeFile, stat, lstat, exists, readDir, makeDir, removeAll, rename, copyFile, move, sniffType, symlink, readLink, realPath, glob, tempDir, makeTempDir, makeTempFile } from "dyna:file";

let n=0,fails=0;
function assert(c,m){n++; if(!c){fails++; print("FAIL: "+m)}}
function eq(a,b,m){assert(a===b,m)}
function throws(fn,re,m){n++; try{fn(); fails++; print("FAIL: "+m)}catch(e){if(re && !re.test(String(e.message))) {fails++; print("FAIL: "+m+" wrong msg "+e.message)}}}
function j(v){ return JSON.stringify(v); }
function hex(u){ return Array.from(u,b=>b.toString(16).padStart(2,"0")).join(""); }
function bytes(h){ return new Uint8Array(h.match(/../g) ? h.match(/../g).map(x=>parseInt(x,16)) : []); }
function logStep(s){ try{ let fs=require("fs"); }catch(e){} }
let ROOT = makeTempDir("cov-file-");
try {

{
  let p1 = new Path("a");
  eq(String(p1),"a","Path 1 seg");
  eq(String(p1.dirname),".","dirname of single");
  eq(p1.basename,"a","basename");
  eq(p1.extname,"","ext empty");
  assert(!p1.isAbsolute,"isAbsolute false");

  let p2 = new Path("/a","b","c");
  eq(String(p2),"/a/b/c","many segs join");

  let p3 = new Path("a", new Path("b/c"));
  eq(String(p3),"a/b/c","Path+string mix");

  throws(()=>new Path(),/needs at least one/,"empty Path throws");
  throws(()=>new Path("a\u0000b"),/NUL/,"NUL segment throws");
  throws(()=>{ new Path("a", 42) },/segment/,"non-string segment throws");
  eq(String(new Path("a/../b")),"b","a/../b normalizes");
  eq(String(new Path("/a/../b")),"/b","/a/../b");
  eq(String(new Path("a/./b")),"a/b","dot normalizes");
  eq(String(new Path("a//b")),"a/b","double slash");
  eq(String(new Path("")),".","empty string => .");
  eq(String(new Path(".")),".","dot => .");
  eq(String(new Path("..")),"..","bare ..");
  eq(String(new Path("/..")),"/","/.. => /");
  eq(String(new Path("a/b/../../c")),"c","complex ..");
  eq(String(new Path("/a/b").relativeTo(new Path("/a/c"))),"../c","relative ../c");
  eq(String(new Path("/a/b").relativeTo(new Path("/a/b"))),".","relative self => .");
  eq(String(new Path("a/b").relativeTo(new Path("a/c"))),"../c","relative relative");
  assert(new Path("a//b").equals(new Path("a/b")),"equals normalized");
  assert(!new Path("a/b").equals(new Path("a/c")),"not equals");
  assert(!new Path("a/b").equals("a/b"),"non-Path not equals");
  assert(Path.isPath(new Path("x")),"isPath true");
  assert(!Path.isPath("x"),"isPath false string");
  assert(!Path.isPath(null),"isPath null");
  assert(Path.cwd().isAbsolute,"cwd absolute");
  assert(Path.temp().isAbsolute,"temp absolute");
  assert(Path.home().isAbsolute,"home absolute");
  eq(Path.sep,"/","sep");
  eq(Path.delimiter,":","delimiter");
  eq(String(new Path("/a").join("b","c")),"/a/b/c","join");
  eq(String(new Path("/a/b").resolve("../c")),"/a/c","resolve");
  eq(String(new Path("/a/b").resolve("/x")),"/x","resolve absolute rebases");
  eq(new Path("/a/b.txt").basenameWithout(".txt"),"b","basenameWithout");
  eq(new Path("/a/b.txt").basenameWithout(".js"),"b.txt","basenameWithout miss");
  eq(String(new Path(new Path("x/y"))),"x/y","Path from Path shares");
  eq(j({p:new Path("/a/b")}),'{"p":"/a/b"}',"toJSON");
  eq(String(new Path("/a//b")),"/a/b","toString normalized");
  let longSeg = "a".repeat(255);
  let lp = new Path("/tmp", longSeg);
  assert(String(lp).length>255,"long seg 255");
  let long2 = "b".repeat(200);
  let lp2 = new Path("/tmp", longSeg, long2);
  assert(String(lp2).length>400,"long multi");
}

{
  let p = ROOT.join("file_empty.txt");
  let f = new File(p);
  eq(f.writeText(""),0,"write empty 0");
  eq(f.readText(),"","read empty");
  eq(f.stat().size,0,"stat size 0");
  assert(f.exists(),"exists empty");
  eq(f.writeText("X"),1,"1 byte");
  eq(f.readText(),"X","read 1 byte");
  let big = "y".repeat(256*1024);
  eq(f.writeText(big),256*1024,"256KB write");
  eq(f.readText().length,256*1024,"256KB read");
  let nul = "a\u0000b\u0000c";
  f.writeText(nul);
  let back = f.readText();
  eq(back.length,5,"NUL len 5");
  assert(back.charCodeAt(1)===0,"NUL preserved 1");
  assert(back.charCodeAt(3)===0,"NUL preserved 2");
  let payload = new Uint8Array([0,1,2,255,0,3]);
  let pb = ROOT.join("bin.dat");
  let fb = new File(pb);
  fb.writeBytes(payload);
  let got = fb.readBytes();
  eq(got.length,6,"binary len");
  eq(Array.from(got).join(","),"0,1,2,255,0,3","binary content");
  eq(got.constructor.name,"Uint8Array","Uint8Array");
  let all = new Uint8Array(256);
  for(let i=0;i<256;i++) all[i]=i;
  fb.writeBytes(all);
  let backAll = fb.readBytes();
  eq(backAll.length,256,"all 256");
  let same=true; for(let i=0;i<256;i++) if(backAll[i]!==i) same=false;
  assert(same,"all bytes same");
  let pa = ROOT.join("append.txt");
  let fa = new File(pa);
  fa.writeText("hello");
  fa.writeText(" world",{append:true});
  eq(fa.readText(),"hello world","append");
  eq(fa.append("!"),1,"append method");
  eq(fa.readText(),"hello world!","append method content");
  fa.writeText("overwrite");
  eq(fa.readText(),"overwrite","overwrite truncates");
  let src = ROOT.join("src.txt");
  let dst = ROOT.join("dst.txt");
  new File(src).writeText("payload");
  let cf = new File(src).copyTo(dst);
  eq(cf.readText(),"payload","copyTo content");
  eq(new File(src).readText(),"payload","src survives");
  cf.writeText("changed");
  eq(new File(src).readText(),"payload","copy is not link");
  throws(()=>new File(src).copyTo(dst),/exists|EEXIST/,"copyTo refuses exists");
  copyFile(src,dst,{overwrite:true});
  eq(readFile(dst),"payload","copyFile overwrite ok");
  let mvSrc = ROOT.join("mv_src.txt");
  let mvDst = ROOT.join("mv_dst.txt");
  new File(mvSrc).writeText("mv");
  let mvHandle = new File(mvSrc);
  let sameH = mvHandle.moveTo(mvDst);
  assert(sameH===mvHandle,"moveTo returns this");
  assert(sameH.path.equals(mvDst),"moveTo retargets");
  eq(sameH.readText(),"mv","moved content");
  assert(!exists(mvSrc),"old gone");
  assert(exists(mvDst),"new exists");
  let st = new File(mvDst).stat();
  assert(st.isFile && !st.isDir,"stat isFile");
  assert(st.size===2,"stat size");
  let linkTarget = ROOT.join("link_target.txt");
  new File(linkTarget).writeText("target");
  let linkPath = ROOT.join("link.txt");
  try{ symlink(String(linkTarget), linkPath);
    let ls = lstat(linkPath);
    let ss = stat(linkPath);
    assert(ls.isSymlink,"lstat symlink");
    assert(!ss.isSymlink && ss.isFile,"stat follows");
    eq(readLink(linkPath), String(linkTarget), "readLink verbatim");
    assert(String(realPath(linkPath))===String(realPath(linkTarget)),"realPath resolves");
    assert(exists(linkPath),"exists follows lstat true for dangling? but exists true for valid link");
    let dang = ROOT.join("dangling.txt");
    symlink("/nonexistent/target", dang);
    assert(exists(dang),"dangling exists true (lstat)");
    assert(lstat(dang).isSymlink,"dangling isSymlink");
    throws(()=>stat(dang),/ENOENT|stat/,"stat dangling throws");
  }catch(e){ if(!/symlink/.test(e.message)) throw e; }
  let rm = ROOT.join("gone.txt");
  new File(rm).writeText("x");
  assert(new File(rm).exists(),"exists before remove");
  new File(rm).remove();
  assert(!new File(rm).exists(),"removed");
  new File(rm).writeText("again");
  eq(new File(rm).readText(),"again","remove is path not descriptor");
  try{ new File(mvDst).chmod(0o644); eq(new File(mvDst).stat().mode & 0o777, 0o644,"chmod"); }catch(e){}
  eq(String(new File(mvDst).realPath()), String(realPath(mvDst)),"realPath method equals free");
}

{
  let p = ROOT.join("reader.txt");
  writeFile(p,"line1\nline2\nline3");
  let r = new FileReader(p);
  eq(r.read(5),"line1","read 5");
  eq(r.readLine(),"","readLine after partial? remainder of line1 is empty + newline");
  r.close();
  let r2 = new FileReader(p,{bufferSize:64});
  eq(r2.readLine(),"line1","readLine first");
  eq(r2.readLine(),"line2","second");
  eq(r2.readLine(),"line3","third");
  eq(r2.readLine(),null,"null at EOF");
  eq(r2.readLine(),null,"null stays");
  r2.close();
  assert(r2.closed,"closed flag");
  r2.close();
  assert(r2.closed,"double close ok");
  let r3 = new FileReader(p);
  let first2 = r3.read(2);
  eq(first2,"li","read 2");
  let rest = r3.readAll();
  eq(first2+rest,"line1\nline2\nline3","read+readAll reconstructs");
  r3.close();
  let emptyP = ROOT.join("reader_empty.txt");
  writeFile(emptyP,"");
  let re = new FileReader(emptyP);
  eq(re.read(),"","read empty");
  eq(re.readLine(),null,"empty readLine null");
  eq(re.readAll(),"","empty readAll");
  re.close();
  let oneP = ROOT.join("reader_one.txt");
  writeFile(oneP,"a");
  let rOne = new FileReader(oneP);
  eq(rOne.read(1),"a","1 byte read");
  eq(rOne.read(1),"","beyond EOF empty");
  rOne.close();
  let twoP = ROOT.join("reader_two.txt");
  writeFile(twoP,"ab");
  let rTwo = new FileReader(twoP);
  eq(rTwo.read(1),"a","2-bytes first");
  eq(rTwo.read(1),"b","second");
  eq(rTwo.read(), "","after");
  rTwo.close();
}

{
  let p = ROOT.join("writer.txt");
  let w = new FileWriter(p,{bufferSize:4096, preallocate: 1024*1024});
  eq(w.write("hello "),6,"writer string");
  eq(w.write(new Uint8Array([119,111,114,108,100])),5,"writer Uint8Array");
  let ab = new Uint8Array([33]).buffer;
  eq(w.write(ab),1,"writer ArrayBuffer");
  let u8 = new Uint8Array([9,8,7,6]);
  eq(w.write(u8.subarray(1,3)),2,"writer subarray");
  w.flush();
  w.sync();
  w.close();
  assert(w.closed,"writer closed");
  w.close();
  eq(new File(p).stat().size,14,"writer total bytes 14");
  let wb = new File(p).readBytes();
  eq(wb.length,14,"writer bytes length");
  eq(wb[0],104,"writer first byte h");
  let wp = ROOT.join("writer_append.txt");
  writeFile(wp,"A");
  let wa = new FileWriter(wp,{append:true});
  wa.write("B"); wa.close();
  eq(readFile(wp),"AB","writer append");
}

{
  let gRoot = ROOT.join("globtest");
  makeDir(gRoot);
  makeDir(gRoot.join("sub"));
  writeFile(gRoot.join("one.txt"),"1");
  writeFile(gRoot.join("two.txt"),"2");
  writeFile(gRoot.join("top.js"),"3");
  writeFile(gRoot.join("sub/deep.js"),"4");
  writeFile(gRoot.join("sub/nested.txt"),"5");
  let gl = new Glob("*.txt");
  eq(gl.pattern,"*.txt","glob pattern");
  assert(gl.hasWildcard,"hasWildcard");
  assert(!new Glob("exact.txt").hasWildcard,"literal no wildcard");
  assert(gl.matches(new Path("one.txt")),"lexical match");
  assert(!gl.matches(new Path("one.js")),"lexical reject");
  assert(gl.matches(new Path("does-not-exist-anywhere.txt")),"lexical no FS");
  let viaClass = gl.expand(gRoot).map(String).sort().join(",");
  let viaFree = glob("*.txt",{cwd:gRoot}).map(String).sort().join(",");
  eq(viaClass,viaFree,"expand equals glob");
  eq(viaClass,"one.txt,two.txt","expand finds");
  let filtered = gl.filter([new Path("a.txt"),new Path("b.js"),new Path("c.txt")]);
  eq(filtered.map(String).join(","),"a.txt,c.txt","filter lexical");
  assert(filtered.every(x=>Path.isPath(x)),"filter returns Paths");
  eq(gl.filter([]).length,0,"empty filter");
  eq(new Glob("**/*.js").expand(gRoot).map(String).sort().join(","),"sub/deep.js,top.js","** spans zero or more");
  eq(new Glob("*.js").expand(gRoot).map(String).join(","),"top.js","* no sep");
  let q = new Glob("?.txt");
  writeFile(gRoot.join("x.txt"),"x");
  eq(q.matches(new Path("a.txt")),true,"? matches single");
  assert(!q.matches(new Path("ab.txt")),"? not double");
  let qExpand = new Glob("?.txt").expand(gRoot).map(String);
  assert(qExpand.includes("x.txt"),"? expand finds x.txt");
  let gg = glob("**/*.txt",{cwd:gRoot});
  assert(gg.length>=3,"** glob finds at least 3 txt");
  throws(()=>new Glob(),/requires a string/,"Glob no arg");
  throws(()=>new Glob(42),/string/,"Glob non-string");
  throws(()=>gl.matches("one.txt"),/Path/,"matches needs Path");
  throws(()=>gl.filter("nope"),/array/,"filter needs array");
  let deep = ROOT.join("a/b/c");
  makeDir(deep,{recursive:true});
  assert(exists(deep),"makeDir recursive");
  throws(()=>makeDir(ROOT.join("nope/sub")),/No such file|ENOENT/,"makeDir non-recursive fails");
  removeAll(ROOT.join("a"));
  assert(!exists(ROOT.join("a")),"removeAll recursive");
  removeAll(ROOT.join("missing_nope"));
  assert(true,"removeAll missing no-op");
  let pngPath = gRoot.join("img.png");
  writeFile(pngPath, new Uint8Array([0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A]));
  eq(sniffType(pngPath),"image/png","sniff file");
  eq(sniffType(new Uint8Array([0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A])),"image/png","sniff bytes");
  eq(sniffType(new Uint8Array([0xFF,0xD8,0xFF])),"image/jpeg","sniff jpeg");
  let rw = ROOT.join("rw.txt");
  eq(writeFile(rw,"hi"),2,"writeFile sync");
  eq(readFile(rw),"hi","readFile sync");
  eq(writeFile(rw," there",{append:true}),6,"append sync");
  eq(readFile(rw),"hi there","append content");
  let entries = readDir(gRoot);
  assert(entries.length>=5,"readDir length");
  assert(entries.some(e=>e.name==="one.txt"),"readDir contains");
  eq(exists(rw), new File(rw).exists(),"exists free vs method");
  let renFrom = ROOT.join("ren_from.txt");
  let renTo = ROOT.join("ren_to.txt");
  writeFile(renFrom,"ren");
  rename(renFrom, renTo);
  assert(!exists(renFrom) && exists(renTo),"rename free");
  let movFrom = ROOT.join("mov_from.txt");
  let movTo = ROOT.join("mov_to.txt");
  writeFile(movFrom,"mov");
  move(movFrom, movTo);
  assert(!exists(movFrom) && exists(movTo),"move free");
  let td = tempDir();
  assert(Path.isPath(td),"tempDir Path");
  assert(td.isAbsolute,"tempDir absolute");
  let mtf = makeTempFile("cov-");
  assert(exists(mtf),"makeTempFile exists");
  assert(String(realPath(mtf))===String(mtf) || String(realPath(mtf)).length>0,"realpath tempFile");
  let large = ROOT.join("large.bin");
  let big = new Uint8Array(1024*1024);
  for(let i=0;i<big.length;i++) big[i]=i & 0xFF;
  writeFile(large, big);
  eq(stat(large).size,1024*1024,"large 1MB size");
  eq(new File(large).readBytes().length,1024*1024,"large readBytes");
  {
    const norm = String(new Path("/tmp","../etc/passwd"));
    assert(!norm.includes(".."), "traversal normalized: no .. left in " + norm);
    assert(norm.endsWith("etc/passwd"), "traversal normalized: tail kept in " + norm);
  }
  let t1 = makeTempDir("race-");
  let t2 = makeTempDir("race-");
  assert(!t1.equals(t2),"temp dirs unique");
  removeAll(t1); removeAll(t2);
}

{
  let lockPath = ROOT.join("lockfile");
  writeFile(lockPath,"x");
  let lock = new FileLock(lockPath);
  let val = lock.withLock(()=>42);
  eq(val,42,"withLock returns");
  assert(lock.closed,"withLock consumes");
  lock.close();
  assert(lock.closed,"double close lock");
  let lock2 = new FileLock(lockPath,{retry:0});
  let v2 = lock2.withLock(()=>99);
  eq(v2,99,"second lock after close");
  let lock3 = new FileLock(lockPath);
  throws(()=>lock3.withLock(null),/function/,"withLock needs fn");
  throws(()=>lock3.withLock("no"),/function/,"withLock not fn");
  lock3.close();
  let wdir = ROOT.join("watchdir");
  makeDir(wdir);
  let wat = new Watcher(wdir);
  wat.start(()=>{});
  let stats = wat.stats();
  assert(typeof stats.entries==="number","watcher entries");
  assert(typeof stats.directories==="number","directories");
  assert(typeof stats.events==="number","events");
  assert(typeof stats.truncated==="boolean","truncated bool");
  assert(typeof stats.debounceMs==="number","debounce");
  assert(!wat.closed,"not closed after start");
  wat.close();
  assert(wat.closed,"closed after");
  wat.close();
  assert(wat.closed,"double close watcher");
  let wat2 = new Watcher(wdir,{debounceMs:10});
  wat2.start(()=>{});
  eq(wat2.stats().debounceMs,10,"debounceMs opt");
  wat2.close();
}

} finally {
}
{
  let wdir2 = ROOT.join("watch_trunc");
  makeDir(wdir2);
  let wat = new Watcher(wdir2,{recursive:true});
  wat.start(()=>{});
  assert(typeof wat.stats().truncated==="boolean","truncated type");
  wat.close();
}
removeAll(ROOT);
assert(!exists(ROOT),"cleanup ROOT removed");
if(fails){
  print("test_file_cov_file_html_xml_yaml_json: "+fails+" FAILED of "+n+" assertions");
  throw new Error("test_cov failed");
}
print("test_file_cov_file_html_xml_yaml_json: "+n+" assertions, 0 failures");
