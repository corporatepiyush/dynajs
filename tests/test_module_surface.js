/* test_module_surface.js -- dynajs.d.ts is a CLAIM about the binary;
 * this file is the check. Every non-gated name declared in the .d.ts must
 * exist at runtime (namespace export, class, prototype member or namespace
 * member), and every runtime namespace export must be declared. The table
 * below is GENERATED from the .d.ts (tools extract: brace-matched module
 * blocks); regenerate it whenever the .d.ts changes:
 *
 *   python3 tools/extract_dts.py dynajs.d.ts
 *
 * dyna:uring is Linux-only and its absence here is asserted separately.
 * dyna:crypto names behind CONFIG_TLS are checked only when present.
 *
 * Each module is probed in a CHILD PROCESS (one static import per file):
 * a missing builtin module aborts at parse time, so an absent module must
 * not take the whole sweep down with it.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_module_surface.js
 */

import * as std from "std";
import * as os from "os";
import { Exec, cwd } from "dyna:sys";
let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("  FAIL: " + m); } }

const SURF = {"dyna:bytes":{"functions":["bytesOf","compare","equal","indexOf","lastIndexOf","contains","count","concat","copy","fill","toUtf8","fromUtf8","isValidUtf8","isValidUtf16","countUtf8","countUtf16","latin1ToUtf8","utf8ToLatin1","utf8ToUtf16","utf16ToUtf8","decode","encode","encodingExists","encodings","readUint8","readInt8","readUint16LE","readUint16BE","readInt16LE","readInt16BE","readUint32LE","readUint32BE","readInt32LE","readInt32BE","readBigUint64LE","readBigUint64BE","readBigInt64LE","readBigInt64BE","readFloatLE","readFloatBE","readDoubleLE","readDoubleBE","writeUint8","writeInt8","writeUint16LE","writeUint16BE","writeInt16LE","writeInt16BE","writeUint32LE","writeUint32BE","writeInt32LE","writeInt32BE","writeBigUint64LE","writeBigUint64BE","writeBigInt64LE","writeBigInt64BE","writeFloatLE","writeFloatBE","writeDoubleLE","writeDoubleBE"],"consts":[],"classes":{"Bytes":["alloc","isBytes","concat","length","isAscii","isValidUtf8","array","slice","compare","equals","indexOf","lastIndexOf","includes","count","indexOfAny","fill","toUtf8","toString","readUint8","readInt8","readUint16LE","readUint16BE","readInt16LE","readInt16BE","readUint32LE","readUint32BE","readInt32LE","readInt32BE","readBigUint64LE","readBigUint64BE","readBigInt64LE","readBigInt64BE","readFloatLE","readFloatBE","readDoubleLE","readDoubleBE","writeUint8","writeInt8","writeUint16LE","writeUint16BE","writeInt16LE","writeInt16BE","writeUint32LE","writeUint32BE","writeInt32LE","writeInt32BE","writeBigUint64LE","writeBigUint64BE","writeBigInt64LE","writeBigInt64BE","writeFloatLE","writeFloatBE","writeDoubleLE","writeDoubleBE"],"Text":["isWide","value","isValidUtf8","isValidUtf16","countUtf8","countUtf16","toUtf8","latin1ToUtf8","utf8ToLatin1","utf8ToUtf16","utf16ToUtf8","toBytes","toJSON","toString"]},"enums":[],"namespaces":{}},"dyna:cli":{"functions":["StyleText","Styles","IsTTY","Columns","ColorDepth"],"consts":[],"classes":{"Command":["name","describe","option","argument","command","allowUnknown","parse","help"]},"enums":[],"namespaces":{}},"dyna:compress":{"functions":["zstd","unzstd","brotli","unbrotli","snappy","unsnappy","lz4Compress","lz4Decompress","lz4Frame","lz4Unframe","gzip","gunzip","TarPack","TarList","TarExtract","ZipPack","ZipList","ZipRead"],"consts":[],"classes":{"Compressor":["close","dispose","closed","[Symbol.dispose]","compress","decompress","algo","dictId"],"Dictionary":["close","dispose","closed","[Symbol.dispose]","compress","decompress","id","size"]},"enums":[],"namespaces":{}},"dyna:config":{"functions":[],"consts":[],"classes":{},"enums":[],"namespaces":{"TOML":["parse","stringify"],"INI":["parse"],"Env":["parse"],"FrontMatter":["split"]}},"dyna:crypto":{"functions":["HMAC","HMACHex","HKDF","PBKDF2","RandomBytes","TimingSafeEqual","HOTPGenerate","TOTPGenerate","JWTSign","JWTVerify"],"consts":[],"classes":{"Hmac":["close","dispose","closed","[Symbol.dispose]","sign","signHex","update","digest","digestHex","verify","algorithm","digestSize"]},"enums":[],"namespaces":{"Bcrypt":["hash","verify"],"Argon2id":["hash","verify","hashAsync","verifyAsync","asyncStats"]},"tls_functions":["Ed25519Generate","Ed25519Sign","Ed25519Verify","X25519Generate","X25519Derive","Scrypt"],"tls_classes":["AESGCM","ChaCha20Poly1305"],"tls_namespaces":["RSA","X509","ECDSA","ECDH"]},"dyna:csv":{"functions":[],"consts":[],"classes":{"CSVFile":["close","dispose","closed","[Symbol.dispose]","create","read","addRow","updateCell","removeRow","addColumn","removeColumn","renameColumn","readColumnValuesRange","readRowRange","selectColumnRange"]},"enums":[],"namespaces":{}},"dyna:dataframe":{"functions":[],"consts":["DataFrame"],"classes":{},"enums":[],"namespaces":{}},"dyna:decimal":{"functions":[],"consts":[],"classes":{"Decimal":["add","sub","mul","div","mod","pow","abs","neg","cmp","equals","round","toFixed","toString","toJSON","toNumber","isZero","sign","digits"],"Money":["add","sub","cmp","equals","mul","allocate","toString","toJSON","amount","currency","format","toDecimal"]},"enums":[],"namespaces":{}},"dyna:encoding":{"functions":["HexEncode","HexDecode","Base64Encode","Base64Decode","Base64URLEncode","Base64URLDecode","Base32Encode","Base32Decode","Base32HexEncode","Base32HexDecode","Base85Encode","Base85Decode","Base58Encode","Base58Decode","Base58CheckEncode","Base58CheckDecode","BaseXEncode","BaseXDecode","PutUvarint","PutVarint","Uvarint","Varint","DetectEncoding","detectEncoding","JSON5Parse","JSON5Stringify","StableStringify","QREncode","QRToString"],"consts":[],"classes":{"JSONPath":["all","first","paths"]},"enums":[],"namespaces":{}},"dyna:file":{"functions":["readFile","writeFile","readFileAsync","writeFileAsync","copyFileAsync","asyncStats","stat","lstat","exists","readDir","makeDir","remove","removeAll","rename","copyFile","move","sniffType","symlink","readLink","realPath","chmod","glob","tempDir","makeTempDir","makeTempFile","dataDir","configDir","cacheDir","dataDirSite","configDirSite","cacheDirSite"],"consts":[],"classes":{"Path":["cwd","home","temp","isPath","sep","delimiter","dirname","basename","extname","isAbsolute","join","resolve","relativeTo","equals","basenameWithout","toString","toJSON"],"File":["path","readText","readBytes","writeText","writeBytes","append","stat","lstat","exists","remove","realPath","chmod","moveTo","copyTo","reader","writer","toString","toJSON"],"FileReader":["close","dispose","closed","[Symbol.dispose]","read","readLine","readAll"],"FileWriter":["close","dispose","closed","[Symbol.dispose]","write","flush","sync","syncAsync"],"FileLock":["close","dispose","closed","[Symbol.dispose]","withLock"],"Watcher":["close","dispose","closed","[Symbol.dispose]","start","stats"],"Glob":["matches","expand","filter","pattern","hasWildcard"]},"enums":[],"namespaces":{}},"dyna:hash":{"functions":["MD5","MD5Hex","SHA1","SHA1Hex","SHA224","SHA224Hex","SHA256","SHA256Hex","SHA384","SHA384Hex","SHA512","SHA512Hex","CRC32","CRC32C","SHA3_224","SHA3_224Hex","SHA3_256","SHA3_256Hex","SHA3_384","SHA3_384Hex","SHA3_512","SHA3_512Hex","Keccak256","Keccak256Hex","SHAKE128","SHAKE128Hex","SHAKE256","SHAKE256Hex","BLAKE3","BLAKE3Hex","BLAKE2b","BLAKE2bHex","BLAKE2s","BLAKE2sHex","Murmur3_128","Murmur3_128Hex","XXHash32","XXHash64"],"consts":[],"classes":{"Hasher":["update","digest","digestHex","reset","algorithm","digestSize"]},"enums":[],"namespaces":{}},"dyna:html":{"functions":["HTMLParse","HTMLStringify","HTMLText","MarkdownToHTML"],"consts":[],"classes":{"Selector":["all","first","matches"],"Sanitizer":["clean"],"Template":["render"]},"enums":[],"namespaces":{}},"dyna:http":{"functions":["ContentTypeParse","ContentTypeFormat","Negotiate","NegotiateToken","RangeParse","CookieParse","CookieSerialize","ETagMatch","MultipartParse","MultipartFormat","fetch"],"consts":["Request","Response","Headers","AbortController","AbortSignal","FormData"],"classes":{"HTTPClient":["close","dispose","closed","[Symbol.dispose]","get","post","request","getAsync","postAsync","requestAsync","setTimeout","disconnect"],"HTTPServer":["close","dispose","closed","[Symbol.dispose]","start","stop","port"],"HTTPServerAsync":["close","dispose","closed","[Symbol.dispose]","start","stop","port"],"App":["close","dispose","closed","[Symbol.dispose]","rpc","static","proxy","upload","ws","sse","start","port"],"WsClient":["close","dispose","closed","[Symbol.dispose]","send"]},"enums":[],"namespaces":{}},"dyna:json":{"functions":[],"consts":["Patch"],"classes":{},"enums":[],"namespaces":{"Pointer":["get","has","set","remove","escape","unescape"]}},"dyna:log":{"functions":["Debug"],"consts":[],"classes":{"Logger":["trace","debug","info","warn","error","fatal","child","enabled","flush","level"]},"enums":[],"namespaces":{}},"dyna:matcher":{"functions":["Levenshtein","DiceCoefficient","DiffChars","DiffWords","DiffLines"],"consts":[],"classes":{"Matcher":["firstIn","test","countIn","allIn","replaceAllIn","length","algo"],"MultiMatcher":["firstIn","test","countIn","allIn","size","states"]},"enums":[],"namespaces":{}},"dyna:mathx":{"functions":["realmin","realmax","flintmax","eps","round","roundToEven","fix","sign","signbit","trunc","modf","mod","rem","fmod","remainder","idivide","nthroot","gamma","cbrt","hypot","copysign","nextafter","expm1","log1p","log2","logb","pow2","deg2rad","rad2deg","nextpow2","scalbn","ldexp","frexp","ilogb","isInf","isNaN","erf","erfc","erfinv","erfcinv","erfcx","lgamma","gammaln","beta","betaln","psi","polygamma","gammainc","gammaincinv","betainc","betaincinv","expint","besselj","bessely","besseli","besselk","besseliScaled","besselkScaled","besselh","ellipke","ellipj","legendre","legendreP","airy","isPrime","factor","primes","gcd","lcm","factorial","abs","bitLen","popcount","nchoosek","perms","rat","linspace","logspace","cumsum","cumprod","diff"],"consts":["E","Pi","Phi","Sqrt2","SqrtE","SqrtPi","Ln2","Ln10","Log2E","Log10E","MaxInt32","MinInt32","MaxSafeInteger","MaxInt64"],"classes":{"Expression":["variables","eval"]},"enums":[],"namespaces":{"bits":["leadingZeros8","leadingZeros16","leadingZeros32","leadingZeros64","trailingZeros8","trailingZeros16","trailingZeros32","trailingZeros64","onesCount8","onesCount16","onesCount32","onesCount64","len8","len16","len32","len64","reverse8","reverse16","reverse32","reverse64","reverseBytes16","reverseBytes32","reverseBytes64","rotateLeft8","rotateLeft16","rotateLeft32","rotateLeft64","add32","add64","sub32","sub64","mul32","mul64","div32","div64","rem32","rem64","uintSize"]}},"dyna:ml":{"functions":["meanSquaredError","meanAbsoluteError","r2Score","accuracy","logLoss","confusionMatrix","precision","recall","f1","specificity","balancedAccuracy","matthewsCorrcoef","cohenKappa","fbeta","rocAuc","averagePrecision","trainTestSplit","kFold","stratifiedKFold","crossValScore","gridSearch","randomSearch","imputeMean","dropMissing"],"consts":[],"classes":{"LinearRegression":["close","dispose","closed","[Symbol.dispose]","deserialize","load","fit","predict","coef","intercept","serialize","save"],"LogisticRegression":["close","dispose","closed","[Symbol.dispose]","deserialize","load","fit","predict","predictProba","classes","coef","intercept","nIter","converged","serialize","save"],"KMeans":["close","dispose","closed","[Symbol.dispose]","deserialize","load","fit","predict","inertia","serialize","save"],"SVC":["close","dispose","closed","[Symbol.dispose]","deserialize","load","fit","predict","decisionFunction","nSupportVectors","classes","serialize","save"],"GaussianMixture":["close","dispose","closed","[Symbol.dispose]","deserialize","load","fit","predict","predictProba","weights","means","variances","logLikelihood","nIter","serialize","save"],"GaussianNB":["close","dispose","closed","[Symbol.dispose]","deserialize","load","fit","predict","predictProba","classes","serialize","save"],"DecisionTreeClassifier":["close","dispose","closed","[Symbol.dispose]","deserialize","load","fit","predict","predictProba","apply","featureImportances","depth","serialize","save"],"DecisionTreeRegressor":["close","dispose","closed","[Symbol.dispose]","deserialize","load","fit","predict","apply","featureImportances","depth","serialize","save"],"RandomForestClassifier":["close","dispose","closed","[Symbol.dispose]","deserialize","load","fit","predict","predictProba","apply","featureImportances","depth","serialize","save"],"RandomForestRegressor":["close","dispose","closed","[Symbol.dispose]","deserialize","load","fit","predict","apply","featureImportances","depth","serialize","save"],"GradientBoostingRegressor":["close","dispose","closed","[Symbol.dispose]","deserialize","load","fit","predict","apply","featureImportances","depth","serialize","save"],"GradientBoostingClassifier":["close","dispose","closed","[Symbol.dispose]","deserialize","load","fit","predict","predictProba","apply","featureImportances","depth","serialize","save"],"XGBRegressor":["close","dispose","closed","[Symbol.dispose]","deserialize","load","fit","predict","apply","featureImportances","depth","bestRounds","serialize","save"],"XGBClassifier":["close","dispose","closed","[Symbol.dispose]","deserialize","load","fit","predict","predictProba","apply","featureImportances","depth","bestRounds","serialize","save"],"PCA":["close","dispose","closed","[Symbol.dispose]","deserialize","load","fit","transform","fitTransform","inverseTransform","components","mean","explainedVariance","explainedVarianceRatio","serialize","save"],"KNClassifier":["close","dispose","closed","[Symbol.dispose]","deserialize","load","fit","predict","serialize","save"],"KNRegressor":["close","dispose","closed","[Symbol.dispose]","deserialize","load","fit","predict","serialize","save"],"DBScan":["close","dispose","closed","[Symbol.dispose]","deserialize","load","fit","labels","nClusters","eps","serialize","save"],"StandardScaler":["close","dispose","closed","[Symbol.dispose]","deserialize","load","fit","transform","fitTransform","inverseTransform","mean","std","serialize","save"],"MinMaxScaler":["close","dispose","closed","[Symbol.dispose]","deserialize","load","fit","transform","fitTransform","inverseTransform","dataMin","dataMax","serialize","save"],"CSR":["close","dispose","closed","[Symbol.dispose]","fromDense","toDense","row","rows","cols","nnz","density"],"Pipeline":["close","dispose","closed","[Symbol.dispose]","fit","predict","predictProba","transform","stage","length","fitted","estimator"]},"enums":[],"namespaces":{}},"dyna:net":{"functions":["parseAddr","parsePrefix","contains","masked","canonical","isValid","compareAddr","isLoopback","isPrivate","isMulticast","isUnspecified","isLinkLocalUnicast","isGlobalUnicast","isLinkLocalMulticast","connectHappy"],"consts":["fetch","Request","Response","Headers","FormData","AbortController","AbortSignal","HTTPClient","HTTPServer","HTTPServerAsync","App","WsClient","ContentTypeParse","ContentTypeFormat","CookieParse","CookieSerialize","ETagMatch","Negotiate","NegotiateToken","RangeParse","MultipartParse","MultipartFormat","Metrics"],"classes":{"Prefix":["contains","overlaps","masked","bits","isIPv4"],"RateLimiter":["allow","tokens","reset","stats"],"DNSResolver":["close","dispose","closed","[Symbol.dispose]","query"],"DNSServer":["close","dispose","closed","[Symbol.dispose]","start","port"],"Redis":["close","dispose","closed","[Symbol.dispose]","command","pipeline","on","protocol","ready","pending"],"PostgreSQL":["close","dispose","closed","[Symbol.dispose]","query","cancel","on","ready","statementCache","pending","backendPid","transactionStatus","parameters"],"SQLite":["close","dispose","closed","[Symbol.dispose]","query","exec","lastInsertRowId","version"],"TCPServer":["close","dispose","closed","[Symbol.dispose]","connect","start","port"],"UDPSocket":["close","dispose","closed","[Symbol.dispose]","start","send","port"],"TCPProxy":["close","dispose","closed","[Symbol.dispose]","start","stats","port"]},"enums":[],"namespaces":{}},"dyna:random":{"functions":[],"consts":[],"classes":{"Random":["nextU64","nextU53","nextFloat","nextBounded","fill","getState","setState"]},"enums":[],"namespaces":{}},"dyna:schema":{"functions":[],"consts":["Schema"],"classes":{},"enums":[],"namespaces":{}},"dyna:scrape":{"functions":[],"consts":[],"classes":{"Robots":["close","dispose","closed","[Symbol.dispose]","allows","crawlDelay","sitemaps","ruleCount"],"Extractor":["run"],"Fetcher":["close","dispose","closed","[Symbol.dispose]","get","stats"],"Crawl":["close","dispose","closed","[Symbol.dispose]","start","next","pages"]},"enums":[],"namespaces":{}},"dyna:semver":{"functions":["parse","isValid","clean","coerce","compare","eq","neq","gt","gte","lt","lte","sort","major","minor","patch","prerelease","inc","satisfies","maxSatisfying","minSatisfying"],"consts":[],"classes":{"Range":["test","filter","maxSatisfying","minSatisfying","source","setCount"]},"enums":[],"namespaces":{}},"dyna:serialize":{"functions":["MsgPackEncode","MsgPackDecode","CBOREncode","CBORDecode","CBORCanonical","ValueHash","structuredClone"],"consts":["Proto","ASN1"],"classes":{},"enums":[],"namespaces":{}},"dyna:simd":{"functions":["sum","max","min","argmax","argmin","normL1","normL2","add","sub","mul","div","abs","fma","dot","scale","addScalar","axpy","affine","sigmoid","relu","relu6","leakyRelu","elu","tanhFast","gelu","silu","softmax","logSoftmax","vexp","vlog","vsqrt","vrsqrt","vinv","distL2","distL1","distCos","distCheb","gemv","gemvT","gemm","clamp","threshold","topkIndices","f64Sum","f64Dot","f64Max","f64Min","f64Scale","f64Axpy","i32Sum","i32Min","i32Max","i32Dot","i32Add","i32Mul","i32Scale","cumsum","cummax"],"consts":[],"classes":{},"enums":[],"namespaces":{}},"dyna:structures":{"functions":[],"consts":[],"classes":{"Graph":["deserialize","addNode","addEdge","neighbors","hasEdge","nodeCount","edgeCount","bfs","dfs","dijkstra","bellmanFord","topologicalSort","connectedComponents","floydWarshall","mst","aStar","serialize"],"LRU":["deserialize","get","put","set","setWithTTL","has","delete","purgeExpired","size","capacity","stats","serialize"],"Heap":["deserialize","push","pop","peek","size","length","serialize"],"MinMaxHeap":["deserialize","push","popMin","popMax","peekMin","peekMax","size","serialize"],"SortedSet":["deserialize","add","has","delete","first","last","floor","ceil","rangeQuery","toArray","size","serialize"],"SortedMap":["deserialize","set","get","has","delete","firstKey","lastKey","floorKey","ceilKey","rangeQuery","keys","size","serialize"],"BTree":["deserialize","set","get","has","delete","firstKey","lastKey","floorKey","ceilKey","rangeQuery","keys","size","serialize"],"Deque":["deserialize","pushBack","pushFront","popFront","popBack","peekFront","peekBack","get","length","toArray","serialize"],"List":["deserialize","pushFront","pushBack","popFront","popBack","front","back","length","toArray","serialize"],"RingBuffer":["deserialize","push","get","length","capacity","full","toArray","serialize"],"BitSet":["deserialize","set","clear","flip","get","nextSet","count","and","or","xor","toArray","serialize"],"UnionFind":["deserialize","find","union","connected","count","size","serialize"],"Fenwick":["deserialize","update","prefixSum","rangeQuery","size","serialize"],"SegTree":["deserialize","update","rangeQuery","size","serialize"],"BloomFilter":["deserialize","add","mayContain","bits","hashes","serialize"],"Trie":["deserialize","insert","has","delete","keysWithPrefix","longestPrefix","size","serialize"],"Multiset":["deserialize","add","remove","count","has","setCount","delete","clear","elementSet","entrySet","size","totalSize","serialize"],"Multimap":["deserialize","put","get","count","delete","removeAt","keys","entries","size","keyCount","serialize"],"BiMap":["deserialize","set","forceSet","get","keyOf","has","hasValue","delete","deleteValue","entries","inverseEntries","clear","size","serialize"],"Table":["deserialize","put","get","has","delete","row","column","cells","size","serialize"],"RangeSet":["deserialize","add","remove","contains","encloses","intersects","ranges","complement","clear","size","measure","serialize"],"RangeMap":["deserialize","put","get","remove","entries","size","serialize"],"IntervalTree":["deserialize","insert","overlapping","at","size","serialize"],"CountMinSketch":["deserialize","add","count","merge","width","depth","totalCount","serialize"],"HyperLogLog":["deserialize","add","count","merge","precision","registers","serialize"]},"enums":[],"namespaces":{}},"dyna:sys":{"functions":["env","getEnv","setEnv","args","cwd","chDir","platform","pid","hostName","homeDir","cpuInfo","memInfo","loadAvg","uptime","diskUsage","memoryUsage","setNativeMemoryLimit","Exec","Which"],"consts":[],"classes":{},"enums":[],"namespaces":{}},"dyna:time":{"functions":["parseDuration","durationString","now","nowUnixNano","nowMillis","monotonicNano","formatRFC3339","formatUnix","parseRFC3339","date","fromUnix","parseDate","dateFromEpochDay","parseTime"],"consts":["Nanosecond","Microsecond","Millisecond","Second","Minute","Hour"],"classes":{"Duration":["years","months","days","sign","blank","toString"],"Format":["format","parse","layout"],"PlainDate":["year","month","day","dayOfWeek","dayOfYear","daysInMonth","daysInYear","inLeapYear","epochDay","add","subtract","until","compare","toString"],"PlainDateTime":["year","month","day","hour","minute","second","millisecond","epochDay","dayOfWeek","add","subtract","toPlainDate","toPlainTime","compare","toString"],"PlainTime":["hour","minute","second","millisecond","msSinceMidnight","add","subtract","compare","toString"],"RRule":["fromString","all","between","next","prev","toString"],"DateParser":["parse","locale","dayFirst"]},"enums":[],"namespaces":{}},"dyna:url":{"functions":["domainToASCII","domainToUnicode","punycodeEncode","punycodeDecode","formEncode","formDecode","encodeURIComponentStrict"],"consts":[],"classes":{"URL":["href","protocol","username","password","host","hostname","port","pathname","search","hash","origin","searchParams","toJSON","toString"],"URLSearchParams":["size","append","delete","get","getAll","has","set","sort","toString","forEach","keys","values","entries"]},"enums":[],"namespaces":{}},"dyna:uuid":{"functions":["v4","v7","v3","v5","parse","validate","version","variant","bytes","fromBytes","NanoID","NanoIDAlphabet","ULID","ULIDTime"],"consts":["NIL","MAX","NAMESPACE_DNS","NAMESPACE_URL","NAMESPACE_OID","NAMESPACE_X500"],"classes":{},"enums":[],"namespaces":{}},"dyna:validate":{"functions":["IsAlpha","IsAlphanumeric","IsAscii","IsEmail","IsCreditCard","IsIBAN","IsDomain","IsURL","IsSlug","IsUUID","IsJWT","IsSemver","IsE164"],"consts":[],"classes":{},"enums":[],"namespaces":{}},"dyna:xml":{"functions":["XMLParse","XMLStringify","XMLToObject"],"consts":[],"classes":{"SAXParser":["write","end"]},"enums":[],"namespaces":{}},"dyna:yaml":{"functions":["Parse","ParseAll","Stringify"],"consts":[],"classes":{},"enums":[],"namespaces":{}},"os":{"functions":["now","getpid","getcwd","chdir","stat","lstat","readdir","readlink","realpath","rename","remove","mkdir","symlink","utimes","open","close","read","write","seek","dup","dup2","pipe","isatty","ttySetRaw","ttyGetWinSize","exec","waitpid","kill","signal","sleep","sleepAsync","setTimeout","clearTimeout","setInterval","clearInterval","setReadHandler","setWriteHandler"],"consts":["platform","O_RDONLY","O_WRONLY","O_RDWR","O_APPEND","O_CREAT","O_EXCL","O_TRUNC","WNOHANG","SIGABRT","SIGALRM","SIGCHLD","SIGCONT","SIGFPE","SIGILL","SIGINT","SIGPIPE","SIGQUIT","SIGSEGV","SIGSTOP","SIGTERM","SIGTSTP","SIGTTIN","SIGTTOU","SIGUSR1","SIGUSR2","S_IFMT","S_IFBLK","S_IFCHR","S_IFDIR","S_IFIFO","S_IFLNK","S_IFREG","S_IFSOCK","S_ISUID","S_ISGID","Worker"],"classes":{},"enums":[],"namespaces":{},"win32_consts":["O_BINARY","O_TEXT"]},"std":{"functions":["printf","sprintf","puts","getenv","setenv","unsetenv","getenviron","exit","gc","evalScript","loadScript","loadFile","open","fdopen","tmpfile","strerror","parseExtJSON","__printObject"],"consts":["Error","SEEK_SET","SEEK_CUR","SEEK_END","in","out","err"],"classes":{},"enums":[],"namespaces":{}}};

/* ---- global layer: WHATWG globals + core prototype extensions ------- */
const GLOB = {"globals":["AbortController","AbortSignal","FormData","Headers","Request","Response","clearInterval","clearTimeout","console","fetch","performance","print","scriptArgs","setInterval","setTimeout"],"ifaces":{"AbortController":["abort"],"AbortControllerConstructor":["new"],"AbortSignal":["onabort","addEventListener","removeEventListener","throwIfAborted","_abort"],"AbortSignalConstructor":["new","abort","timeout"],"Array<T>":["isEmpty","first","last","sum","average","mean","compact","count","none","any","all","min","max","take","drop","takeLast","dropLast","sortBy","sortedIndexOf","groupBy","shuffle","sample","unique","uniq","uniqBy","intersect","intersection","difference","without","union","partition","pluck","zip","zipWith","intersperse","flatten","transpose","xprod","aperture","splitEvery","splitAt","adjust","update","move","swap","nth","init","tail","head","takeWhile","dropWhile","takeLastWhile","dropLastWhile","append","prepend","reject","insert","insertAll","removeAt","zipObj","fromPairs","median","product","scan","countBy","indexBy","remove","exclude","removeRange","splitWhen","innerJoin","startsWith","endsWith","unnest","dropRepeats","dropRepeatsWith","dropRepeatsBy","sortWith","unionWith","differenceWith","symmetricDifference","symmetricDifferenceWith","reduceBy","transduce","into","sequence","traverse","mapFromIndex","forEachFromIndex","filterFromIndex","findFromIndex","findIndexFromIndex","someFromIndex","everyFromIndex","reduceFromIndex","reduceRightFromIndex","lazy"],"ArrayConstructor":["repeat","fromAsync"],"Console":["log","info","debug","trace","warn","error","assert"],"Date":["isValid","isToday","isYesterday","isTomorrow","isFuture","isPast","isWeekday","isWeekend","isLeapYear","isSunday","isMonday","isTuesday","isWednesday","isThursday","isFriday","isSaturday","isJanuary","isFebruary","isMarch","isApril","isMay","isJune","isJuly","isAugust","isSeptember","isOctober","isNovember","isDecember","getWeekday","getISOWeek","daysInMonth","isBefore","isAfter","isBetween","millisecondsSince","millisecondsUntil","millisecondsAgo","millisecondsFromNow","secondsSince","secondsUntil","secondsAgo","secondsFromNow","minutesSince","minutesUntil","minutesAgo","minutesFromNow","hoursSince","hoursUntil","hoursAgo","hoursFromNow","daysSince","daysUntil","daysAgo","daysFromNow","weeksSince","weeksUntil","weeksAgo","weeksFromNow","monthsSince","monthsUntil","monthsAgo","monthsFromNow","yearsSince","yearsUntil","yearsAgo","yearsFromNow","addMilliseconds","addSeconds","addMinutes","addHours","addDays","addWeeks","addMonths","addYears","beginningOfDay","endOfDay","beginningOfWeek","endOfWeek","beginningOfMonth","endOfMonth","beginningOfYear","endOfYear","advance","rewind","clone","format","relative","iso","getYear","setYear","toGMTString"],"DynResource":["close","dispose"],"FormData":["append","delete","get","getAll","has","set","forEach","keys","values","entries"],"FormDataConstructor":["new"],"Headers":["append","delete","get","has","set","forEach","keys","values","entries"],"HeadersConstructor":["new"],"Map<K, V>":["getOrInsert","getOrInsertComputed"],"Number":["abs","sqrt","exp","sin","cos","tan","asin","acos","atan","negate","inc","dec","add","subtract","multiply","divide","modulo","pow","gt","gte","lt","lte","isInteger","isOdd","isEven","isMultipleOf","mathMod","clamp","log","round","ceil","floor","chr","pad","hex","format","abbr","metric","bytes","ordinalize","duration","times","upto","downto"],"NumberConstructor":["range"],"Object":["__defineGetter__","__defineSetter__","__lookupGetter__","__lookupSetter__"],"Performance":["now"],"RegExp":[],"RegExpConstructor":["escape"],"Request":["text","json","bytes","arrayBuffer"],"RequestConstructor":["new"],"RequestInit":[],"Response":["text","json","bytes","arrayBuffer","clone"],"ResponseConstructor":["new"],"ResponseInit":[],"SetConstructor":["groupBy"],"String":["lazy","isEmpty","trimPrefix","trimSuffix","trimChars","containsAny","indexOfAny","indexOfAll","equalsIgnoreCase","compareBytes","splitN","isBlank","first","last","from","to","chars","codes","reverse","insert","remove","removeAll","compact","shift","pad","capitalize","underscore","dasherize","spacify","camelize","truncate","truncateOnWord","escapeHTML","unescapeHTML","stripTags","count","toNumber","humanize","titleize","parameterize","pluralize","singularize","removeTags","forEach","format","words","lines","encodeBase64","decodeBase64","escapeURL","unescapeURL","stripAnsi","displayWidth","wrapAnsi","graphemes","isWellFormed","toWellFormed"],"SymbolConstructor":[]},"targets":{"String":["String.prototype","String"],"Array<T>":["Array.prototype","Array"],"Number":["Number.prototype","Number"],"Date":["Date.prototype","Date"],"Map<K, V>":["Map.prototype","Map"],"SetConstructor":[null,"Set"],"RegExp":["RegExp.prototype","RegExp"],"RegExpConstructor":[null,"RegExp"],"SymbolConstructor":[null,"Symbol"],"AbortSignal":["AbortSignal.prototype","AbortSignal"],"AbortController":["AbortController.prototype","AbortController"],"Headers":["Headers.prototype","Headers"],"FormData":["FormData.prototype","FormData"],"Request":["Request.prototype","Request"],"Response":["Response.prototype","Response"],"Performance":["performance",null],"Console":["console",null],"Object":["Object.prototype",null]}};
let grows = 0;
for (const g of GLOB.globals) {
    ok(typeof globalThis[g] !== "undefined", "global " + g + " exists");
}
const OBJ_TARGET = { "String.prototype": String.prototype,
                     "Array.prototype": Array.prototype,
                     "Number.prototype": Number.prototype,
                     "Object.prototype": Object.prototype,
                     "Date.prototype": Date.prototype,
                     "Map.prototype": Map.prototype,
                     "RegExp.prototype": RegExp.prototype,
                     "AbortSignal.prototype": typeof AbortSignal === "function" ? AbortSignal.prototype : null,
                     "AbortController.prototype": typeof AbortController === "function" ? AbortController.prototype : null,
                     "Headers.prototype": typeof Headers === "function" ? Headers.prototype : null,
                     "FormData.prototype": typeof FormData === "function" ? FormData.prototype : null,
                     "Request.prototype": typeof Request === "function" ? Request.prototype : null,
                     "Response.prototype": typeof Response === "function" ? Response.prototype : null,
                     "performance": typeof performance === "object" ? performance : null,
                     "console": console };
for (const name of Object.keys(GLOB.ifaces)) {
    const t = GLOB.targets[name];
    if (!t) continue;                       /* constructor-params interfaces */
    const inst = OBJ_TARGET[t[0]];
    const ctor = t[1] ? globalThis[t[1]] : null;
    if ((inst === null || inst === undefined) && (ctor === null || ctor === undefined)) {
        ok(false, "global target for " + name + " (" + t[0] + ")");
        continue;
    }
    const have = new Set();
    for (const o of [inst, ctor]) {
        if (!o) continue;
        for (const k of Object.getOwnPropertyNames(o)) have.add(k);
        const po = Object.getPrototypeOf(o);
        if (po && po !== Object.prototype && po !== Function.prototype)
            for (const k of Object.getOwnPropertyNames(po)) have.add(k);
    }
    for (const m of GLOB.ifaces[name])
        ok(have.has(m), name + "." + m + " exists at runtime");
}

function namesOf(v) {
    try { return Object.getOwnPropertyNames(v); } catch (e) { return null; }
}

/* One probe per module. Prints ROWS <n> then one "kind | name" line each. */
function probeSrc(mod) {
    return [
        'import * as ns from "' + mod + '";',
        'import * as probe_os from "os";',
        "const spec = " + JSON.stringify({
            functions: SURF[mod].functions,
            consts: SURF[mod].consts,
            enums: SURF[mod].enums,
            classes: SURF[mod].classes,
            namespaces: SURF[mod].namespaces,
            tls_functions: SURF[mod].tls_functions || [],
            tls_classes: SURF[mod].tls_classes || [],
            tls_namespaces: SURF[mod].tls_namespaces || [],
            win32_consts: SURF[mod].win32_consts || [],
        }) + ";",
        "function namesOf(v) { try { return Object.getOwnPropertyNames(v); } catch (e) { return null; } }",
        "function symHasDispose(v) {",
        "    try { return Object.getOwnPropertySymbols(v).some(function (s) { return String(s).indexOf('dispose') >= 0; }); } catch (e) { return false; }",
        "}",
        "let rows = [];",
        "const have = namesOf(ns) || [];",
        "const haveSet = new Set(have);",
        "for (const f of spec.functions)",
        "    if (!haveSet.has(f)) rows.push('missing-fn | ' + f);",
        "for (const c of spec.consts)",
        "    if (!haveSet.has(c)) rows.push('missing-const | ' + c);",
        "for (const e of spec.enums)",
        "    if (!haveSet.has(e)) rows.push('missing-enum | ' + e);",
        "if (spec.tls_functions.length && haveSet.has(spec.tls_functions[0])) {",
        "    for (const f of spec.tls_functions)",
        "        if (!haveSet.has(f)) rows.push('missing-fn | ' + f);",
        "    for (const cls of spec.tls_classes)",
        "        if (typeof ns[cls] !== 'function') rows.push('missing-class | ' + cls);",
        "    for (const nm of spec.tls_namespaces)",
        "        if (!ns[nm]) rows.push('missing-ns | ' + nm);",
        "}",
        "if (spec.win32_consts.length && String(probe_os.platform).indexOf('win') !== 0) {",
        "    for (const c of spec.win32_consts) if (haveSet.has(c)) rows.push('extra-runtime | ' + c);",
        "} else if (spec.win32_consts.length) {",
        "    for (const c of spec.win32_consts) if (!haveSet.has(c)) rows.push('missing-const | ' + c);",
        "}",
        "const decl = new Set([...spec.functions, ...spec.consts, ...spec.enums,",
        "                      ...Object.keys(spec.classes), ...Object.keys(spec.namespaces),",
        "                      ...spec.tls_functions, ...spec.tls_classes,",
        "                      ...spec.tls_namespaces]);",
        "for (const h of have)",
        "    if (!decl.has(h)) rows.push('extra-runtime | ' + h);",
        "for (const cls of Object.keys(spec.classes)) {",
        "    const members = spec.classes[cls];",
        "    const C = ns[cls];",
        "    if (typeof C !== 'function') { rows.push('missing-class | ' + cls); continue; }",
        "    let inst = [], stat = [], disp = false;",
        "    try { inst = namesOf(C.prototype) || []; } catch (e) {}",
        "    try { stat = namesOf(C) || []; } catch (e) {}",
        "    try { disp = symHasDispose(C.prototype); } catch (e) {}",
        "    const haveM = new Set([...inst, ...stat]);",
        "    for (const m of members) {",
        "        if (m === '[Symbol.dispose]') {",
        "            if (!disp) rows.push('missing-member | ' + cls + '.[Symbol.dispose]');",
        "        } else if (!haveM.has(m)) rows.push('missing-member | ' + cls + '.' + m);",
        "    }",
        "}",
        "for (const nsn of Object.keys(spec.namespaces)) {",
        "    const members = spec.namespaces[nsn];",
        "    const N = ns[nsn];",
        "    if (!N || typeof N !== 'object') { rows.push('missing-ns | ' + nsn); continue; }",
        "    const haveM = new Set(namesOf(N) || []);",
        "    for (const m of members)",
        "        if (!haveM.has(m)) rows.push('missing-ns-member | ' + nsn + '.' + m);",
        "}",
        "for (const r of rows) print(r);",
        "print('ROWS ' + rows.length);",
    ].join("\n");
}

const DYN = cwd() + "/dynajs";
const T = "/tmp/surface_" + Date.now();
os.mkdir(T, 0o755);

let mods = 0, loadFails = 0;
for (const mod of Object.keys(SURF)) {
    const src = probeSrc(mod);
    const f = T + "/probe.js";
    const fo = std.open(f, "w");
    fo.puts(src);
    fo.close();
    const r = Exec(DYN, [f], { timeoutMs: 30000 });
    if (r.code !== 0 || r.timedOut) {
        loadFails++;
        print("  LOAD-FAIL " + mod + ": " + String(r.stderr).trim().split("\n")[0]);
        continue;
    }
    mods++;
    const out = r.stdout.trim().split("\n");
    let count = -1;
    for (const line of out) {
        if (line.indexOf("ROWS ") === 0) { count = parseInt(line.slice(5), 10); continue; }
        ok(false, mod + ": " + line);
    }
    ok(count === 0, mod + ": expected 0 surface rows, got " + count +
       (count > 0 ? " (see failures above)" : ""));
}
ok(loadFails === 0, "every declared module loads (" + loadFails + " failed)");
ok(mods >= 30, "sweep covered " + mods + " modules");

os.remove(T + "/probe.js");
os.remove(T);

if (fails) {
    print("test_module_surface: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_module_surface failed");
}
print("test_module_surface: " + n + " assertions over " + mods +
      " modules, 0 failures");
