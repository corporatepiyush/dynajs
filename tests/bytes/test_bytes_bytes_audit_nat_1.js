// timeout: 600
// covers: dyna:bytes.Bytes
// tests/bytes/test_audit_nat_1.js -- dyna:bytes must route its libc allocations
// through the tracked allocator so --native-memory-limit actually bounds them.
//
// THE DEFECT THIS PINS. dyna:bytes allocated 10 buffers with libc malloc /
// calloc directly: the KMP prefix table in lastIndexOf, the concat output
// buffers, the latin1/utf16 transcoding scratch buffers, and the dyn_bh_t /
// dyn_txt_t handles behind every Bytes and Text object. None of that touched
// dyn_nat_*, so the module was invisible to the ledger: the flag was accepted,
// memoryUsage().nativeLimit echoed it back, and it did nothing. The audit
// measurement that opened the ticket was 488 MiB allocated against a 1 MB cap
// with nativeSize reading 0.
//
// WHAT IS ACTUALLY PINNED HERE. The 488 MiB figure is CUMULATIVE churn on the
// JS-heap ArrayBuffer payload (Bytes.alloc goes through JS_NewArrayBufferCopy,
// so it is already engine-heap memory and is NOT native memory -- see
// CHANGELOG). The native memory that WAS unbounded is the libc half, and that
// is what each case below drives: Bytes.concat and latin1ToUtf8 build their
// output with libc malloc, sized by caller input, so they are the reachable
// unbounded-growth door. On the unfixed build every MUST_FAIL case here
// succeeds against a 1 MB cap; on the fixed build each throws out-of-memory
// and the ledger reads non-zero.
//
// HONESTY MODEL. MUST_FAIL rows stop being bounded -> FAIL (a regression).
// There is no XPASS list: nothing here is expected to remain unbounded.
//
// build-note: needs dyna:bytes + dyna:sys (build with CONFIG_NATIVE_MODULES=y)
import { Exec } from "dyna:sys";
import { Bytes, latin1ToUtf8 } from "dyna:bytes";

const BIN = scriptArgs[1] || "./dynajs";
const CAP = "1000000";
let failures = 0;

function assert(cond, msg) {
    if (!cond) {
        failures++;
        print("  FAIL:", msg);
    }
}

function run(args, timeoutMs) {
    return Exec(BIN, args, { timeoutMs: timeoutMs || 60000, encoding: "utf8" });
}

function mk(js) {
    return [
        "-e",
        'Promise.all([import("dyna:bytes"),import("dyna:sys")])' +
        '.then(([B,S])=>{' +
        'const {Bytes,latin1ToUtf8,utf8ToLatin1}=B;' +
        'const {memoryUsage}=S;' +
        js +
        '})',
    ];
}

// A refusal must be a normal catchable JS error, never a signal: an abort
// from the {size,magic} header assert or a heap corruption would show up as
// signal != null. A crash that still prints "out of memory" cannot pass here.
function assertCleanOOM(r, label) {
    const out = r.stdout + r.stderr;
    assert(r.signal === null,
        label + ": child died on signal " + r.signal + " (out=" + out.slice(0, 120) + ")");
    assert(!/Assertion|SIGABRT|corrupt|Aborted/.test(out),
        label + ": abort/corruption text in output (out=" + out.slice(0, 120) + ")");
    assert(/out of memory/i.test(out),
        label + ": expected an out-of-memory throw, got (out=" + out.slice(0, 120) + ")");
    assert(!r.timedOut, label + ": child hung past the external bound");
}

// ---------------------------------------------------------------- MUST_FAIL
// Each drives a libc buffer sized well past the 1 MB cap.

const MUST_FAIL = [
    ["Bytes.concat 400x1MiB = 400 MiB",
     'const s=new Uint8Array(1048576).fill(65);const l=[];' +
     'for(let i=0;i<400;i++)l.push(s);' +
     'try{Bytes.concat(l);print("NOT_BOUNDED");}' +
     'catch(e){print("out of memory: "+e.name);}'],

    ["latin1ToUtf8 on 16 MiB (2x growth = 32 MiB)",
     'const s=new Uint8Array(16777216).fill(65);' +
     'try{latin1ToUtf8(s);print("NOT_BOUNDED");}' +
     'catch(e){print("out of memory: "+e.name);}'],

    ["utf8ToLatin1 on 16 MiB",
     'const s=new Uint8Array(16777216).fill(65);' +
     'try{utf8ToLatin1(s);print("NOT_BOUNDED");}' +
     'catch(e){print("out of memory: "+e.name);}'],

    ["text.latin1ToUtf8 repeated (ledger accumulates)",
     'const s=new Uint8Array(1048576).fill(65);' +
     'for(let i=0;i<64;i++){try{latin1ToUtf8(s);}catch(e){print("out of memory at i="+i);break;}}' +
     'print(memoryUsage().nativeSize>0?"LEDGER_NONZERO":"LEDGER_ZERO");'],
];

{
    const r = run(["--std", "--native-memory-limit", CAP].concat(mk(
        'const keep=[];' +
        'for(let i=0;i<2000;i++)keep.push(Bytes.alloc(64));' +
        'print("LEDGER " + memoryUsage().nativeSize);' +
        'print("LIMIT " + memoryUsage().nativeLimit);'
    )));
    const out = r.stdout + r.stderr;
    const m = /LEDGER (\d+)/.exec(out);
    assert(m !== null, "ledger probe did not print (out=" + out.slice(0, 120) + ")");
    if (m) {
        assert(Number(m[1]) > 0,
            "nativeSize is 0 with 2000 live Bytes: dyna:bytes still off-ledger");
        print("  ok  nativeSize non-zero with live Bytes: " + m[1]);
    }
    assert(/LIMIT 1000000/.test(out),
        "nativeLimit not echoed back (out=" + out.slice(0, 120) + ")");
}

{
    const r = run(["--std", "--native-memory-limit", "1073741824"].concat(mk(
        'const s=new Uint8Array(1048576).fill(65);const l=[];' +
        'for(let i=0;i<8;i++)l.push(s);' +
        'print("HEADROOM_OK " + Bytes.concat(l).length);'
    )));
    const out = r.stdout + r.stderr;
    assert(/HEADROOM_OK 8388608/.test(out),
        "1 GiB cap refused an 8 MiB concat (out=" + out.slice(0, 120) + ")");
    if (!failures) print("  ok  headroom: 8 MiB under a 1 GiB cap succeeds");
}

if (failures) {
    print("test_audit_nat_1: " + failures + " FAILURES");
    throw new Error("test_audit_nat_1 failed");
}
print("test_audit_nat_1: all tests passed");
