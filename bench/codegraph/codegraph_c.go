package main

import (
	"bufio"
	"bytes"
	"cmp"
	"crypto/sha1"
	"encoding/csv"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"io/fs"
	"maps"
	"math"
	"math/bits"
	"os"
	"path/filepath"
	"reflect"
	"regexp"
	"runtime"
	"slices"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
	"unicode"
	"unicode/utf8"
	"unsafe"
)


type arena struct {
	buf []byte
	off []int32
}

func (a *arena) add(s string) int32 {
	start := int32(len(a.buf))
	a.buf = append(a.buf, s...)
	a.off = append(a.off, int32(len(a.buf)))
	return start
}

func (a *arena) str(i int32) string {
	if i < 0 {
		return ""
	}
	end := a.off[i]
	start := int32(0)
	if i > 0 {
		start = a.off[i-1]
	}
	return string(a.buf[start:end])
}

type symArena struct{ arena }

type File struct {
	ID            int32
	pad1          uint32
	path, dir     cgStr
	basename, ext cgStr
	lang          cgStr
	ModuleID      int32
	Bytes         int32
	Lines         int32
	Sloc          int32
	BlankLines    int32
	CommentLines  int32
	DocLines      int32
	MaxLineLen    int32
	sha1          cgStr
	Parsed        int32
	IsTest        int32
	IsGenerated   int32
	IsVendored    int32
	NParseErrors  int32
	NMissingNodes int32
	ParseMs       float64

	NSymbols   int32
	NFunctions int32
	NTypes     int32
	NImports   int32
	TotalCyclo int32
	MaxCyclo   int32
	TotalRisk  int32
	pad2       uint32
}

func (f *File) Path() string     { return f.path.Str() }
func (f *File) Dir() string      { return f.dir.Str() }
func (f *File) Basename() string { return f.basename.Str() }
func (f *File) Ext() string      { return f.ext.Str() }
func (f *File) Lang() string     { return f.lang.Str() }
func (f *File) Sha1() string     { return f.sha1.Str() }

type Module struct {
	ID          int32
	pad1        uint32
	name, kind  cgStr
	NFiles      int32
	NSymbols    int32
	NPublic     int32
	Sloc        int32
	FanIn       int32
	FanOut      int32
	Instability float64
}

func (m *Module) Name() string { return m.name.Str() }
func (m *Module) Kind() string { return m.kind.Str() }

type symW struct {
	ID            int32
	FileID        int32
	ModuleID      int32
	Name          string
	QualName      string
	Kind          string
	LineStart     int32
	Signature     string
	ReturnType    string
	HasSignature  bool
	HasReturnType bool

	NParams int32

	IsPublic, IsStatic     int32
	IsAbstract, IsOverride int32
	IsTest, IsEntrypoint   int32
	IsGenerated            int32

	Sloc, NCommentLines, HasDoc int32

	Cyclomatic, Cognitive, MaxNesting     int32
	NTokens, NOperators, NOperands        int32
	NDistinctOperators, NDistinctOperands int32

	NLoops, NBranches, NReturns int32
	NSwitch, NCases             int32
	NLabels, NGotos             int32

	MaxLoopDepth, CallInLoop, AllocInLoop, IOInLoop int32
	LockInLoop, BranchInLoop                        int32

	NLocals, NCmp, NArith int32
	NShift                int32
	NFloatLit, NMagic     int32
	NNullCheck            int32

	NCalls, NDynamicCalls, NUnresolvedCalls int32
	FanIn, FanOut, NCallsites               int32
	IsRecursive                             int32

	RiskScore    int32
	NMemory      int32
	NAlloc       int32
	NIO          int32
	NStdio       int32
	NExec        int32
	NLibm        int32
	NInteger     int32
	NConcurrency int32
	NReentrancy  int32

	IsInline, IsVariadic               int32
	NPtrLocals, NDeref, NCast, NSizeof int32
	NIntrinsic, NAtomic, NRestrict     int32
	NLikely, NBuiltin                  int32

	rare int32
}

type Symbol struct {
	ID            int32
	FileID        int32
	ModuleID      int32
	name          cgStr
	qualName      cgStr
	kind          cgStr
	LineStart     int32
	signature     cgStr
	returnType    cgStr
	HasSignature  bool
	HasReturnType bool

	NParams int32

	IsPublic, IsStatic     int32
	IsAbstract, IsOverride int32
	IsTest, IsEntrypoint   int32
	IsGenerated            int32

	Sloc, NCommentLines, HasDoc int32

	Cyclomatic, Cognitive, MaxNesting     int32
	NTokens, NOperators, NOperands        int32
	NDistinctOperators, NDistinctOperands int32

	NLoops, NBranches, NReturns int32
	NSwitch, NCases             int32
	NLabels, NGotos             int32

	MaxLoopDepth, CallInLoop, AllocInLoop, IOInLoop int32
	LockInLoop, BranchInLoop                        int32

	NLocals, NCmp, NArith int32
	NShift                int32
	NFloatLit, NMagic     int32
	NNullCheck            int32

	NCalls, NDynamicCalls, NUnresolvedCalls int32
	FanIn, FanOut, NCallsites               int32
	IsRecursive                             int32

	RiskScore    int32
	NMemory      int32
	NAlloc       int32
	NIO          int32
	NStdio       int32
	NExec        int32
	NLibm        int32
	NInteger     int32
	NConcurrency int32
	NReentrancy  int32

	IsInline, IsVariadic               int32
	NPtrLocals, NDeref, NCast, NSizeof int32
	NIntrinsic, NAtomic, NRestrict     int32
	NLikely, NBuiltin                  int32

	rare int32
}

func (s *Symbol) Name() string       { return s.name.Str() }
func (s *Symbol) QualName() string   { return s.qualName.Str() }
func (s *Symbol) Kind() string       { return s.kind.Str() }
func (s *Symbol) Signature() string  { return s.signature.Str() }
func (s *Symbol) ReturnType() string { return s.returnType.Str() }

type symRare struct {
	SwitchInLoop, LibmInLoop, DivInLoop         int32
	StrlenInLoop                                int32
	RetNull, RetNeg, RetZero, RetVal, RetVoid   int32
	NFnptrCalls, NMacroCalls                    int32
	NExternalCalls                              int32
	NFree, NConstCast, NToctou                  int32
	NLockAcquire, NLockRelease                  int32
	NNarrowCast, NSignCmp, NVariadicFmt         int32
	NMemcpy, NAllocsite                         int32
	NGlobalWrite                                int32
	NErrno, NWeakRandom, NShiftVar              int32
	NReallocSelf, NVla, NGetenv                 int32
	NAssertSide                                 int32
	NFreeThenUse                                int32
	NEpoll, NUring, NKqueue, NEventWait         int32
	NEtReg, NOneshotReg, NWriteReady            int32
	NErrFlag, NRearm, NDereg, NEagain, NEintr   int32
	NUringRes, NUringRing, NUringBarrier        int32
	NUringSqpoll                                int32
	NNonblockSet, NEventCreate, NEventDestroy   int32
	NUringSqe, NUringSeen, NUringUdata          int32
	NUringLink, NUringStreamOps, NUringTeardown int32
	NEvTimeoutIndefinite, NEvTimeoutZero        int32
	NEvBatchOne, NKqTimer, NKqTimerZeroData     int32
	NKqReceipt                                  int32
	NRetNegCheck, NDomainGuard, NUcharCast      int32
	NErrnoZero, NEndptr, NVaEnd, NMapFailed     int32
	NMonotonicClock, NStackszArray              int32
	NPtrOvfCheck, NCallocTransposed, NVaArgArr  int32
}

type symDrop struct {
	BodyBytes                        int32
	HalsteadVolume, Maintainability  int32
	IsLeaf, IsRoot                   int32
	LineEnd, NLines, NDocLines       int32
	NEarlyReturns                    int32
	NControl, NHazards, NUniqueCalls int32
	NExternDeclCalls                 int32
	Visibility                       string
	NAddrof, NAllocsiteNoSizeof      int32
	NArrow, NAssign, NBitop          int32
	NCompoundAssign, NIncdec         int32
	NGlobalRead                      int32
	NLogical, NMemberAccess          int32
	NPtrParams, NStaticAssert        int32
	NStringLit, NSubscript           int32
	NTernary, NVolatile              int32
}

type symBuild struct {
	symW
	symRare
	symDrop
}

var zeroRare symRare

func (g *Graph) rv(s *Symbol) *symRare {
	if s.rare < 0 {
		return &zeroRare
	}
	return &g.rare[s.rare]
}

func (g *Graph) rw(i int) *symRare {
	s := g.Symbols[i]
	if s.rare < 0 {
		g.rare = append(g.rare, symRare{})
		s.rare = int32(len(g.rare)) - 1
	}
	return &g.rare[s.rare]
}

type paramW struct {
	SymbolID, Pos   int32
	Name            string
	HasName         bool
	Type            string
	DefaultValue    string
	HasDefaultValue bool
	IsOptional      int32
	IsVariadic      int32
	IsRef           int32
	IsMutable       int32
	IsNullable      int32
	IsGeneric       int32
	IsUntyped       int32
	TypeDepth       int32
}

type Param struct {
	SymbolID, Pos   int32
	name            cgStr
	HasName         bool
	pad1            [7]byte
	typ             cgStr
	def             cgStr
	HasDefaultValue bool
	pad2            [7]byte
	IsOptional      int32
	IsVariadic      int32
	IsRef           int32
	IsMutable       int32
	IsNullable      int32
	IsGeneric       int32
	IsUntyped       int32
	TypeDepth       int32
}

func (p *Param) Name() string    { return p.name.Str() }
func (p *Param) Type() string    { return p.typ.Str() }
func (p *Param) Default() string { return p.def.Str() }

type fieldW struct {
	SymbolID, Ordinal     int32
	Name, Type            string
	Visibility            string
	Line                  int32
	IsStatic, IsConst     int32
	IsMutable, IsNullable int32
	IsCollection          int32
	IsUntyped             int32
	HasDefault            int32
	TypeDepth             int32
}

type Field struct {
	SymbolID, Ordinal     int32
	name, typ, vis        cgStr
	Line                  int32
	pad1                  uint32
	IsStatic, IsConst     int32
	IsMutable, IsNullable int32
	IsCollection          int32
	IsUntyped             int32
	HasDefault            int32
	TypeDepth             int32
}

func (f *Field) Name() string { return f.name.Str() }
func (f *Field) Type() string { return f.typ.Str() }
func (f *Field) Vis() string  { return f.vis.Str() }

type localW struct {
	SymbolID, Ordinal  int32
	Name, Type         string
	Line               int32
	IsConst, IsMutable int32
	IsUntyped          int32
	HasInit, InLoop    int32
	ScopeDepth         int32
}

type Local struct {
	SymbolID, Ordinal  int32
	name, typ          cgStr
	Line               int32
	IsConst, IsMutable int32
	IsUntyped          int32
	HasInit, InLoop    int32
	ScopeDepth         int32
}

func (l *Local) Name() string { return l.name.Str() }
func (l *Local) Type() string { return l.typ.Str() }

type Edge struct {
	CallerID, CalleeID           int32
	NCalls, SameFile, SameModule int32
	IsSelf                       int32
}

type Callsite struct{ CallerID, CalleeID, Line int32 }

type Unresolved struct {
	CallerID  int32
	pad1      uint32
	name      cgStr
	N         int32
	FirstLine int32
}

func (u *Unresolved) Name() string { return u.name.Str() }

type Import struct {
	ID                     int32
	FileID                 int32
	target                 cgStr
	TargetID               int32
	HasTargetID            bool
	pad1                   uint32
	alias                  cgStr
	HasAlias               bool
	pad2                   [7]byte
	kind                   cgStr
	Line                   int32
	IsExternal, IsRelative int32
	IsWildcard, IsTypeOnly int32
	IsDynamic, NNames      int32
}

func (i *Import) Target() string { return i.target.Str() }
func (i *Import) Alias() string  { return i.alias.Str() }
func (i *Import) Kind() string   { return i.kind.Str() }

type Hazard struct {
	SymbolID  int32
	pad1      uint32
	pattern   cgStr
	category  cgStr
	N         int32
	FirstLine int32
}

func (h *Hazard) Pattern() string  { return h.pattern.Str() }
func (h *Hazard) Category() string { return h.category.Str() }

type Attribute struct {
	ID          int32
	SymbolID    int32
	HasSymbolID bool
	pad1        [7]byte
	FileID      int32
	pad2        uint32
	name        cgStr
	args        cgStr
	HasArgs     bool
	pad3        [7]byte
	Line        int32
	pad4        uint32
}

func (a *Attribute) Name() string { return a.name.Str() }
func (a *Attribute) Args() string { return a.args.Str() }

type Literal struct {
	ID       int32
	SymbolID int32
	HasSym   bool
	pad1     [7]byte
	FileID   int32
	pad2     uint32
	kind     cgStr
	value    cgStr
	Line     int32
	IsMagic  int32
}

func (l *Literal) Kind() string  { return l.kind.Str() }
func (l *Literal) Value() string { return l.value.Str() }

type EnumMember struct {
	SymbolID, Ordinal int32
	name              cgStr
	value             cgStr
	HasValue          bool
	pad1              [7]byte
	NFields           int32
	pad2              uint32
}

func (e *EnumMember) Name() string  { return e.name.Str() }
func (e *EnumMember) Value() string { return e.value.Str() }

type Marker struct {
	ID       int32
	FileID   int32
	SymbolID int32
	HasSym   bool
	pad1     uint32
	kind     cgStr
	Line     int32
	pad2     uint32
	text     cgStr
}

func (m *Marker) Kind() string { return m.kind.Str() }
func (m *Marker) Text() string { return m.text.Str() }

type LayoutRow struct {
	SymbolID, Ordinal  int32
	ByteOff, ByteSize  int32
	PadBefore, Exact   int32
	PtrDepth, ArrayLen int32
	IsFnptr, Depth     int32
	InUnion            int32
}

type StructSize struct {
	SymbolID, TotalSize, TailPad, TotalPad int32
	MaxAlign, Exact, NLines64              int32
}

type Declaration struct {
	ID     int32
	FileID int32
	name   cgStr
	Line   int32
	pad1   uint32
}

func (d *Declaration) Name() string { return d.name.Str() }

type AddrTaken struct {
	ID       int32
	SymbolID int32
	HasSym   bool
	pad1     [7]byte
	FileID   int32
	pad2     uint32
	name     cgStr
	Line     int32
	pad3     uint32
	kind     cgStr
}

func (a *AddrTaken) Name() string { return a.name.Str() }
func (a *AddrTaken) Kind() string { return a.kind.Str() }

type SecretCandidate struct {
	ID       int32
	SymbolID int32
	HasSym   bool
	pad1     [7]byte
	FileID   int32
	pad2     uint32
	value    cgStr
	Line     int32
	pad3     uint32
}

func (s *SecretCandidate) Value() string { return s.value.Str() }

type AllocSite struct {
	ID       int32
	SymbolID int32
	FileID   int32
	pad1     uint32
	fn       cgStr
	sizeExpr cgStr
	Line     int32
	pad2     uint32
}

func (a *AllocSite) Fn() string       { return a.fn.Str() }
func (a *AllocSite) SizeExpr() string { return a.sizeExpr.Str() }

type Memop struct {
	ID                int32
	SymbolID          int32
	FileID            int32
	pad1              uint32
	fn                cgStr
	dst, src, sizeArg cgStr
	sizeBuf, dstTail  cgStr
	Line              int32
	pad2              uint32
}

func (m *Memop) Fn() string      { return m.fn.Str() }
func (m *Memop) Dst() string     { return m.dst.Str() }
func (m *Memop) Src() string     { return m.src.Str() }
func (m *Memop) SizeArg() string { return m.sizeArg.Str() }
func (m *Memop) SizeBuf() string { return m.sizeBuf.Str() }
func (m *Memop) DstTail() string { return m.dstTail.Str() }

type Macro struct {
	SymbolID       int32
	IsFunctionlike int32
	NParams        int32
	pad1           uint32
	body           cgStr
	HasBody        bool
	pad2           [7]byte
	BodyLen        int32
	IsMultiline    int32
	NUses          int32
	pad3           uint32
}

func (m *Macro) Body() string { return m.body.Str() }

type Global struct {
	ID         int32
	FileID     int32
	ModuleID   int32
	pad1       uint32
	name       cgStr
	typ        cgStr
	Line       int32
	IsStatic   int32
	IsConst    int32
	IsVolatile int32
	IsAtomic   int32
	IsArray    int32
	PtrDepth   int32
	HasInit    int32
}

func (g *Global) Name() string { return g.name.Str() }
func (g *Global) Type() string { return g.typ.Str() }

type ConfigBlock struct {
	ID        int32
	FileID    int32
	directive cgStr
	expr      cgStr
	Line      int32
	IsConfig  int32
}

func (c *ConfigBlock) Directive() string { return c.directive.Str() }
func (c *ConfigBlock) Expr() string      { return c.expr.Str() }

type Reach struct {
	SymbolID, NTransitive, NTransitiveOut int32
}

type MakefileRule struct {
	ID     int32
	pad1   uint32
	path   cgStr
	rule   cgStr
	Line   int32
	NObjs  int32
	NSrcs  int32
	UsesAr int32
}

func (r *MakefileRule) Path() string { return r.path.Str() }
func (r *MakefileRule) Rule() string { return r.rule.Str() }

type LockRow struct {
	ID       int32
	SymbolID int32
	FileID   int32
	pad1     uint32
	name     cgStr
	Line     int32
	pad2     uint32
}

func (l *LockRow) Name() string { return l.name.Str() }

type EventOp struct {
	ID       int32
	SymbolID int32
	FileID   int32
	pad1     uint32
	family   cgStr
	fn       cgStr
	args     cgStr
	Line     int32
	pad2     uint32
}

func (e *EventOp) Family() string { return e.family.Str() }
func (e *EventOp) Fn() string     { return e.fn.Str() }
func (e *EventOp) Args() string   { return e.args.Str() }

type APIUse struct {
	ID       int32
	SymbolID int32
	FileID   int32
	pad1     uint32
	ns       cgStr
	fn       cgStr
	Line     int32
	pad2     uint32
}

func (a *APIUse) NS() string { return a.ns.Str() }
func (a *APIUse) Fn() string { return a.fn.Str() }

type IncludeCycle struct {
	ID           int32
	pad1         uint32
	aPath, bPath cgStr
	Length       int32
	pad2         uint32
	members      cgStr
}

func (c *IncludeCycle) APath() string   { return c.aPath.Str() }
func (c *IncludeCycle) BPath() string   { return c.bPath.Str() }
func (c *IncludeCycle) Members() string { return c.members.Str() }

type hazardW struct {
	SymbolID  int32
	Pattern   string
	Category  string
	N         int32
	FirstLine int32
}

type attrW struct {
	ID          int32
	SymbolID    int32
	HasSymbolID bool
	FileID      int32
	Name        string
	Args        string
	HasArgs     bool
	Line        int32
}

type importW struct {
	ID                     int32
	FileID                 int32
	Target                 string
	TargetID               int32
	HasTargetID            bool
	Alias                  string
	HasAlias               bool
	Kind                   string
	Line                   int32
	IsExternal, IsRelative int32
	IsWildcard, IsTypeOnly int32
	IsDynamic, NNames      int32
}

type literalW struct {
	ID       int32
	SymbolID int32
	HasSym   bool
	FileID   int32
	Kind     string
	Value    string
	Line     int32
	IsMagic  int32
}

type enumW struct {
	SymbolID, Ordinal int32
	Name              string
	Value             string
	HasValue          bool
	NFields           int32
}

type markerW struct {
	ID       int32
	FileID   int32
	SymbolID int32
	HasSym   bool
	Kind     string
	Line     int32
	Text     string
}

type declW struct {
	ID     int32
	FileID int32
	Name   string
	Line   int32
}

type addrW struct {
	ID       int32
	SymbolID int32
	HasSym   bool
	FileID   int32
	Name     string
	Line     int32
	Kind     string
}

type secretW struct {
	ID       int32
	SymbolID int32
	HasSym   bool
	FileID   int32
	Value    string
	Line     int32
}

type allocW struct {
	ID       int32
	SymbolID int32
	FileID   int32
	Fn       string
	SizeExpr string
	Line     int32
}

type memopW struct {
	ID                int32
	SymbolID          int32
	FileID            int32
	Fn                string
	Dst, Src, SizeArg string
	SizeBuf, DstTail  string
	Line              int32
}

type macroW struct {
	SymbolID       int32
	IsFunctionlike int32
	NParams        int32
	Body           string
	HasBody        bool
	BodyLen        int32
	IsMultiline    int32
	NUses          int32
}

type globalW struct {
	ID         int32
	FileID     int32
	ModuleID   int32
	Name       string
	Type       string
	Line       int32
	IsStatic   int32
	IsConst    int32
	IsVolatile int32
	IsAtomic   int32
	IsArray    int32
	PtrDepth   int32
	HasInit    int32
}

type cfgW struct {
	ID        int32
	FileID    int32
	Directive string
	Expr      string
	Line      int32
	IsConfig  int32
}

type lockW struct {
	ID       int32
	SymbolID int32
	FileID   int32
	Name     string
	Line     int32
}

type evopW struct {
	ID       int32
	SymbolID int32
	FileID   int32
	Family   string
	Fn       string
	Args     string
	Line     int32
}

type apiuseW struct {
	ID       int32
	SymbolID int32
	FileID   int32
	NS       string
	Fn       string
	Line     int32
}

type parseMsg struct {
	fileID int32
	line   int32
	msg    string
}

type fileOut struct {
	perr                      []parseMsg
	symA                      *symBlock
	symLo, symN               int32
	rareN                     int32
	params                    []paramW
	fields                    []fieldW
	locals                    []localW
	literals                  []literalW
	markers                   []markerW
	attrs                     []attrW
	imports                   []importW
	hazards                   []hazardW
	enums                     []enumW
	layout                    []LayoutRow
	ssize                     []StructSize
	decls                     []declW
	addrs                     []addrW
	secrets                   []secretW
	allocs                    []allocW
	memops                    []memopW
	macros                    []macroW
	macroNames                []string
	globals                   []globalW
	cfgs                      []cfgW
	locks                     []lockW
	evops                     []evopW
	apiuses                   []apiuseW
	pendSID, pendFID, pendMID []int32
	pendName                  []string
	pendLine                  [][]int32
	globalNames               []string

	funcs []fnSite
}

const symChunk = 2048

type symBlock struct {
	chunks [][]symW

	rare []symRare
}

func newSymBlockN(n int) *symBlock {
	if n < 1 {
		n = 1
	}
	return &symBlock{chunks: [][]symW{make([]symW, 0, n)}}
}

func (a *symBlock) add(s *symW) {
	n := len(a.chunks)
	if n == 0 || len(a.chunks[n-1]) == cap(a.chunks[n-1]) {
		a.chunks = append(a.chunks, make([]symW, 0, symChunk))
		n++
	}
	a.chunks[n-1] = append(a.chunks[n-1], *s)
}

func (a *symBlock) addRare(r symRare) int32 {
	a.rare = append(a.rare, r)
	return int32(len(a.rare) - 1)
}

func (a *symBlock) count() int32 {
	t := int32(0)
	for _, c := range a.chunks {
		t += int32(len(c))
	}
	return t
}

func (a *symBlock) at(lo, n int32) []symW {
	if n == 0 {
		return nil
	}
	if len(a.chunks) == 1 && int(lo)+int(n) <= len(a.chunks[0]) {
		return a.chunks[0][lo : lo+n]
	}
	out := make([]symW, 0, n)
	for int32(len(out)) < n {
		p := lo + int32(len(out))
		off := int32(0)
		ci := 0
		for ci < len(a.chunks) && off+int32(len(a.chunks[ci])) <= p {
			off += int32(len(a.chunks[ci]))
			ci++
		}
		if ci >= len(a.chunks) {
			break
		}
		co := p - off
		take := min(int32(len(a.chunks[ci]))-co, n-int32(len(out)))
		out = append(out, a.chunks[ci][co:co+take]...)
	}
	return out
}

type fnSite struct {
	static bool
	name   string
	sid    int32
	fid    int32
	mid    int32
}

type Graph struct {
	Modules []Module
	Files   []File

	// what the parser could not read, for --parse-errors. Not part of the
	// graph and not saved: it describes this run, not the code.
	ParseMsgs []parseMsg

	Symbols  []*Symbol
	symStore []Symbol

	rare           []symRare
	Params         []Param
	Fields         []Field
	Locals         []Local
	Literals       []Literal
	Markers        []Marker
	Attrs          []Attribute
	Imports        []Import
	Hazards        []Hazard
	EnumMem        []EnumMember
	Layout         []LayoutRow
	SSize          []StructSize
	Decls          []Declaration
	Addr           []AddrTaken
	Secrets        []SecretCandidate
	Allocs         []AllocSite
	Memops         []Memop
	Macros         []Macro
	Globals        []Global
	Cfgs           []ConfigBlock
	Reach          []Reach
	MkRules        []MakefileRule
	Locks          []LockRow
	EvOps          []EventOp
	APIUses        []APIUse
	apiByID        map[int32][]int32
	apiByFn        map[string][]int32
	hazIx          *hazIndex
	symIx          *symIndex
	edgeOut        *edgeCSRc
	edgeIn         *edgeCSRc
	atCache        []string
	symMark        []bool
	symsCache      []*Symbol
	symsCacheOrder string
	byNameCache    []*Symbol
	Cycles         []IncludeCycle
	Edges          []Edge
	Callsites      []Callsite
	Unres          []Unresolved

	Meta map[string]string

	byFile     map[int32][]int32
	byName     map[string][]int32
	fileByRel  map[string]int32
	fileByBase map[string]int32

	dumpW *bufio.Writer

	astBlob   []byte
	nameOrder []*Symbol

	EdgesN, MacroN, ExternN, DeclN, UnresN int32
	CallsTotal                             int32
	AddrPruned                             int32
	MakefilesRead                          int32
	FilesSkippedBig                        int32
	FilesSkippedSpecial                    int32
	FilesSkippedEscape                     int32
	FilesSkippedDenied                     int32
	WalkErrors                             int32
	FilesParsed                            int32
	FilesFailed                            int32
}

func itoa(v int) string { return strconv.Itoa(v) }

func realpath(p string) string {
	r, err := filepath.EvalSymlinks(p)
	if err != nil {
		return p
	}
	return r
}

var ef = []byte{0xEF, 0xBF, 0xBD}

func runeLen(b []byte) int {
	n := len(b)
	for _, c := range b {
		if c&0xC0 == 0x80 {
			n--
		}
	}
	return n
}

func isIdentStart(c byte) bool {
	return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
}

func isIdentPart(c byte) bool {
	return isIdentStart(c) || (c >= '0' && c <= '9') || c >= 0x80
}

func isDigit(c byte) bool { return c >= '0' && c <= '9' }

func isStripSpace(c byte) bool {
	switch c {
	case ' ', '\t', '\r', '\n', '\v', '\f', 0x1c, 0x1d, 0x1e, 0x1f, 0x85, 0xa0:
		return true
	}
	return false
}

func isHardWS(c byte) bool {
	return c == ' ' || c == '\t' || c == '\r' || c == '\n'
}

func trimSpace(s string) string {
	a := 0
	for a < len(s) && isStripSpace(s[a]) {
		a++
	}
	b := len(s)
	for b > a && isStripSpace(s[b-1]) {
		b--
	}
	return s[a:b]
}

func isSplitBound(b []byte, i int) bool {
	switch b[i] {
	case '\n', '\r', 0x0b, 0x0c, 0x1c, 0x1d, 0x1e:
		return true
	case 0xC2:
		return i+1 < len(b) && b[i+1] == 0x85
	case 0xE2:
		return i+2 < len(b) && b[i+1] == 0x80 && (b[i+2] == 0xA8 || b[i+2] == 0xA9)
	}
	return false
}
func skipBoundary(b []byte, i int) int {
	switch b[i] {
	case '\r':
		if i+1 < len(b) && b[i+1] == '\n' {
			return i + 2
		}
		return i + 1
	case 0xC2:
		return i + 2
	case 0xE2:
		return i + 3
	}
	return i + 1
}
func wordBefore(b []byte, i int) bool {
	return i > 0 && isWordByte(b[i-1])
}
func wordAfter(b []byte, i int) bool {
	return i < len(b) && isWordByte(b[i])
}
func identEndStr(s string, i int) int {
	if i >= len(s) || !isIdentStart(s[i]) {
		return i
	}
	j := i + 1
	for j < len(s) && isIdentPart(s[j]) {
		j++
	}
	return j
}
func pathBase(p string) string {
	if i := strings.LastIndexByte(p, '/'); i >= 0 {
		return p[i+1:]
	}
	return p
}

func splitBounds(b []byte) (int, []int32, int) {
	var starts []int32
	begin := 0
	i := 0
	last := 0
	for i < len(b) {
		var w int
		switch {
		case b[i] == '\n':
			w = 1
		case b[i] == '\r':
			w = 1
			if i+1 < len(b) && b[i+1] == '\n' {
				w = 2
			}
		case b[i] == 0x0b || b[i] == 0x0c || b[i] == 0x1c || b[i] == 0x1d ||
			b[i] == 0x1e:
			w = 1
		case b[i] == 0xC2 && i+1 < len(b) && b[i+1] == 0x85:
			w = 2
		case b[i] == 0xE2 && i+2 < len(b) && b[i+1] == 0x80 &&
			(b[i+2] == 0xA8 || b[i+2] == 0xA9):
			w = 3
		default:
			i++
			continue
		}
		starts = append(starts, int32(begin))
		begin = i + w
		last = i
		i += w
	}

	if begin < len(b) {
		starts = append(starts, int32(begin))
		return len(starts), starts, len(b)
	}
	if len(starts) == 0 {
		return 0, starts, 0
	}
	return len(starts), starts, last
}

func splitLines(text string) []string {
	b := []byte(text)
	n, starts, lastEnd := splitBounds(b)
	out := make([]string, 0, n)
	for i := range n {
		s := starts[i]
		e := int32(lastEnd)
		if i+1 < n {
			e = trimBreak(b, starts[i+1])
		}
		if e < s {
			e = s
		}
		out = append(out, text[s:e])
	}
	return out
}

func trimBreak(b []byte, at int32) int32 {
	j := int(at)
	if j > 0 && j-1 < len(b) && b[j-1] == '\n' {
		j--
		if j > 0 && b[j-1] == '\r' {
			j--
		}
		return int32(j)
	}
	if j >= 3 && b[j-3] == 0xE2 {
		return int32(j - 3)
	}
	if j >= 2 && b[j-2] == 0xC2 {
		return int32(j - 2)
	}
	if j > 0 {
		return int32(j - 1)
	}
	return int32(j)
}

func nlOffsets(b []byte) []int32 {
	offs := make([]int32, 0, len(b)/24+8)
	for i, c := range b {
		if c == '\n' {
			offs = append(offs, int32(i+1))
		}
	}
	return offs
}

func blankBuf(buf []byte, src []byte) []byte {
	out := buf
	if cap(out) < len(src) {
		out = make([]byte, len(src))
	} else {
		out = out[:len(src)]
	}
	copy(out, src)
	i := 0
	n := len(src)
	for i < n {
		switch {
		case src[i] == '/' && i+1 < n && src[i+1] == '*':
			j := indexFrom(src, "*/", i+2)
			end := n
			if j >= 0 {
				end = j + 2
			}
			blankSpan(out, src, i, end)
			i = end
		case src[i] == '/' && i+1 < n && src[i+1] == '/':
			j := indexByteFrom(src, '\n', i)
			end := n
			if j >= 0 {
				end = j
			}
			blankSpan(out, src, i, end)
			i = end
		case src[i] == '"' || src[i] == '\'':
			q := src[i]
			j := i + 1
			closed := -1
			for j < n {
				if src[j] == '\\' {
					j += 2
					continue
				}
				if src[j] == q {
					closed = j
					break
				}
				if src[j] == '\n' {
					break
				}
				j++
			}
			end := j
			if closed >= 0 {
				end = closed + 1
			} else {

				if end < n && src[end] == '\\' {
					end = n
				}
			}
			blankSpan(out, src, i, end)
			i = end
		default:
			i++
		}
	}
	return out
}

func blankSpan(out, src []byte, start, end int) {
	if end > len(out) {
		end = len(out)
	}
	for k := start; k < end; k++ {
		if src[k] != '\n' {
			out[k] = ' '
		}
	}
}

func indexFrom(b []byte, s string, from int) int {
	if from >= len(b) {
		return -1
	}
	i := bytes.Index(b[from:], []byte(s))
	if i < 0 {
		return -1
	}
	return from + i
}

func indexByteFrom(b []byte, c byte, from int) int {
	for i := from; i < len(b); i++ {
		if b[i] == c {
			return i
		}
	}
	return -1
}

func cgRepr(f float64) string {
	if math.IsInf(f, 1) {
		return "inf"
	}
	if math.IsInf(f, -1) {
		return "-inf"
	}
	if math.IsNaN(f) {
		return "nan"
	}
	neg := math.Signbit(f)
	if neg {
		f = -f
	}
	if f == 0 {
		if neg {
			return "-0.0"
		}
		return "0.0"
	}

	mant := strconv.FormatFloat(f, 'e', -1, 64)
	epos := strings.IndexByte(mant, 'e')
	digits := strings.Replace(mant[:epos], ".", "", 1)
	exp, _ := strconv.Atoi(mant[epos+1:])

	decpt := exp + 1
	var sb strings.Builder
	if neg {
		sb.WriteByte('-')
	}
	if decpt < -3 || decpt > 16 {
		sb.WriteByte(digits[0])
		if len(digits) > 1 {
			sb.WriteByte('.')
			sb.WriteString(digits[1:])
		}
		e := decpt - 1
		sb.WriteByte('e')
		if e < 0 {
			sb.WriteByte('-')
			e = -e
		} else {
			sb.WriteByte('+')
		}
		es := strconv.Itoa(e)
		if len(es) < 2 {
			es = "0" + es
		}
		sb.WriteString(es)
		return sb.String()
	}
	if decpt <= 0 {
		sb.WriteString("0.")
		for i := 0; i < -decpt; i++ {
			sb.WriteByte('0')
		}
		sb.WriteString(digits)
	} else if decpt >= len(digits) {
		sb.WriteString(digits)
		for i := 0; i < decpt-len(digits); i++ {
			sb.WriteByte('0')
		}
		sb.WriteString(".0")
	} else {
		sb.WriteString(digits[:decpt])
		sb.WriteByte('.')
		sb.WriteString(digits[decpt:])
	}
	return sb.String()
}

const minFileSlot = 64 << 10

var parseByteBudget = int64(2 << 20)

func fileCharge(n int64) int64 {
	if n < minFileSlot {
		return minFileSlot
	}
	return n
}

var defaultParseWorkers = runtime.GOMAXPROCS(0)

var cKeywords = map[string]bool{
	"_Atomic":        true,
	"_Bool":          true,
	"_Generic":       true,
	"_Static_assert": true,
	"__asm__":        true,
	"__attribute__":  true,
	"__extension__":  true,
	"__inline":       true,
	"__inline__":     true,
	"__restrict":     true,
	"__restrict__":   true,
	"__typeof__":     true,
	"__volatile__":   true,
	"alignas":        true,
	"alignof":        true,
	"and":            true,
	"asm":            true,
	"break":          true,
	"case":           true,
	"char":           true,
	"const":          true,
	"continue":       true,
	"default":        true,
	"defined":        true,
	"do":             true,
	"double":         true,
	"else":           true,
	"enum":           true,
	"extern":         true,
	"float":          true,
	"for":            true,
	"goto":           true,
	"if":             true,
	"inline":         true,
	"int":            true,
	"long":           true,
	"not":            true,
	"or":             true,
	"register":       true,
	"restrict":       true,
	"return":         true,
	"short":          true,
	"signed":         true,
	"sizeof":         true,
	"static":         true,
	"struct":         true,
	"switch":         true,
	"typedef":        true,
	"typeof":         true,
	"union":          true,
	"unsigned":       true,
	"void":           true,
	"volatile":       true,
	"while":          true,
}

var typeWords = map[string]bool{
	"_Atomic":  true,
	"const":    true,
	"enum":     true,
	"extern":   true,
	"inline":   true,
	"long":     true,
	"restrict": true,
	"short":    true,
	"signed":   true,
	"static":   true,
	"struct":   true,
	"union":    true,
	"unsigned": true,
	"volatile": true,
}

var hazardFuncs = map[string]string{
	"_exit":                          "control",
	"abort":                          "control",
	"accept":                         "io",
	"accept4":                        "io",
	"access":                         "io",
	"acos":                           "libm",
	"alarm":                          "control",
	"aligned_alloc":                  "alloc",
	"alloca":                         "memory",
	"asctime":                        "reentrancy",
	"asin":                           "libm",
	"asprintf":                       "memory",
	"assert":                         "control",
	"at_quick_exit":                  "control",
	"atan":                           "libm",
	"atan2":                          "libm",
	"atexit":                         "control",
	"atof":                           "integer",
	"atoi":                           "integer",
	"atol":                           "integer",
	"atoll":                          "integer",
	"atomic_compare_exchange_strong": "concurrency",
	"atomic_compare_exchange_weak":   "concurrency",
	"atomic_exchange":                "concurrency",
	"atomic_fetch_add":               "concurrency",
	"atomic_fetch_sub":               "concurrency",
	"atomic_load":                    "concurrency",
	"atomic_store":                   "concurrency",
	"atomic_thread_fence":            "concurrency",
	"basename":                       "reentrancy",
	"bcopy":                          "memory",
	"bind":                           "io",
	"bzero":                          "memory",
	"calloc":                         "alloc",
	"cbrt":                           "libm",
	"ceil":                           "libm",
	"cfree":                          "alloc",
	"chmod":                          "io",
	"chown":                          "io",
	"close":                          "io",
	"connect":                        "io",
	"cos":                            "libm",
	"crypt":                          "reentrancy",
	"ctime":                          "reentrancy",
	"dirname":                        "reentrancy",
	"dlclose":                        "exec",
	"dlmopen":                        "exec",
	"dlopen":                         "exec",
	"dlsym":                          "exec",
	"drand48":                        "reentrancy",
	"erf":                            "libm",
	"execl":                          "exec",
	"execle":                         "exec",
	"execlp":                         "exec",
	"execv":                          "exec",
	"execve":                         "exec",
	"execvp":                         "exec",
	"execvpe":                        "exec",
	"exit":                           "control",
	"exp":                            "libm",
	"expf":                           "libm",
	"faccessat":                      "io",
	"fchmod":                         "io",
	"fchown":                         "io",
	"fclose":                         "stdio",
	"fdatasync":                      "io",
	"fexecve":                        "exec",
	"fflush":                         "stdio",
	"fgetc":                          "stdio",
	"fgets":                          "stdio",
	"floor":                          "libm",
	"fmod":                           "libm",
	"fopen":                          "stdio",
	"fork":                           "exec",
	"fprintf":                        "stdio",
	"fputc":                          "stdio",
	"fputs":                          "stdio",
	"fread":                          "stdio",
	"free":                           "alloc",
	"freopen":                        "io",
	"fscanf":                         "memory",
	"fsync":                          "io",
	"ftruncate":                      "io",
	"fwrite":                         "stdio",
	"g_free":                         "alloc",
	"g_malloc":                       "alloc",
	"getaddrinfo":                    "io",
	"getcwd":                         "memory",
	"getenv":                         "reentrancy",
	"getgrgid":                       "reentrancy",
	"getgrnam":                       "reentrancy",
	"gethostbyaddr":                  "reentrancy",
	"gethostbyname":                  "io",
	"getpwnam":                       "reentrancy",
	"getpwuid":                       "reentrancy",
	"gets":                           "memory",
	"getwd":                          "memory",
	"gmtime":                         "reentrancy",
	"hypot":                          "libm",
	"inet_pton":                      "io",
	"initstate":                      "reentrancy",
	"kill":                           "control",
	"lgamma":                         "libm",
	"link":                           "io",
	"listen":                         "io",
	"localtime":                      "reentrancy",
	"log":                            "libm",
	"log10":                          "libm",
	"log2":                           "libm",
	"logf":                           "libm",
	"longjmp":                        "control",
	"lrand48":                        "reentrancy",
	"lseek":                          "io",
	"malloc":                         "alloc",
	"memalign":                       "alloc",
	"memccpy":                        "memory",
	"memchr":                         "memory",
	"memcmp":                         "memory",
	"memcpy":                         "memory",
	"memmove":                        "memory",
	"mempcpy":                        "memory",
	"memset":                         "memory",
	"mkdtemp":                        "io",
	"mkstemp":                        "io",
	"mktemp":                         "io",
	"mmap":                           "io",
	"mrand48":                        "reentrancy",
	"munmap":                         "io",
	"open":                           "io",
	"openat":                         "io",
	"opendir":                        "io",
	"pause":                          "control",
	"popen":                          "exec",
	"posix_memalign":                 "alloc",
	"posix_spawn":                    "exec",
	"posix_spawnp":                   "exec",
	"pow":                            "libm",
	"powf":                           "libm",
	"pread":                          "io",
	"printf":                         "stdio",
	"pthread_barrier_wait":           "concurrency",
	"pthread_cancel":                 "concurrency",
	"pthread_cond_broadcast":         "concurrency",
	"pthread_cond_signal":            "concurrency",
	"pthread_cond_wait":              "concurrency",
	"pthread_create":                 "concurrency",
	"pthread_detach":                 "concurrency",
	"pthread_join":                   "concurrency",
	"pthread_kill":                   "concurrency",
	"pthread_mutex_lock":             "concurrency",
	"pthread_mutex_trylock":          "concurrency",
	"pthread_mutex_unlock":           "concurrency",
	"pthread_rwlock_rdlock":          "concurrency",
	"pthread_rwlock_unlock":          "concurrency",
	"pthread_rwlock_wrlock":          "concurrency",
	"putchar":                        "stdio",
	"putenv":                         "reentrancy",
	"puts":                           "stdio",
	"pwrite":                         "io",
	"quick_exit":                     "control",
	"raise":                          "control",
	"rand":                           "reentrancy",
	"random":                         "reentrancy",
	"read":                           "io",
	"readdir":                        "io",
	"readlink":                       "io",
	"readv":                          "io",
	"realloc":                        "alloc",
	"reallocarray":                   "alloc",
	"reallocf":                       "alloc",
	"realpath":                       "memory",
	"recv":                           "io",
	"recvfrom":                       "io",
	"recvmsg":                        "io",
	"remove":                         "io",
	"rename":                         "io",
	"round":                          "libm",
	"scanf":                          "memory",
	"sched_yield":                    "concurrency",
	"sem_post":                       "concurrency",
	"sem_trywait":                    "concurrency",
	"sem_wait":                       "concurrency",
	"send":                           "io",
	"sendfile":                       "io",
	"sendmsg":                        "io",
	"sendto":                         "io",
	"setenv":                         "reentrancy",
	"setjmp":                         "control",
	"setlocale":                      "reentrancy",
	"sigaction":                      "control",
	"siglongjmp":                     "control",
	"signal":                         "control",
	"sigprocmask":                    "control",
	"sigsetjmp":                      "control",
	"sigsuspend":                     "control",
	"sin":                            "libm",
	"snprintf":                       "memory",
	"splice":                         "io",
	"sprintf":                        "memory",
	"sqrt":                           "libm",
	"sqrtf":                          "libm",
	"srand":                          "reentrancy",
	"srandom":                        "reentrancy",
	"sscanf":                         "memory",
	"stpcpy":                         "memory",
	"stpncpy":                        "memory",
	"strcat":                         "memory",
	"strchr":                         "memory",
	"strcmp":                         "memory",
	"strcpy":                         "memory",
	"strdup":                         "alloc",
	"strdupa":                        "alloc",
	"strerror":                       "reentrancy",
	"strlcat":                        "memory",
	"strlcpy":                        "memory",
	"strlen":                         "memory",
	"strncat":                        "memory",
	"strncmp":                        "memory",
	"strncpy":                        "memory",
	"strndup":                        "alloc",
	"strndupa":                       "alloc",
	"strnlen":                        "memory",
	"strrchr":                        "memory",
	"strsep":                         "memory",
	"strstr":                         "memory",
	"strtod":                         "integer",
	"strtof":                         "integer",
	"strtoimax":                      "integer",
	"strtok":                         "memory",
	"strtok_r":                       "memory",
	"strtol":                         "integer",
	"strtoll":                        "integer",
	"strtoul":                        "integer",
	"strtoull":                       "integer",
	"strtoumax":                      "integer",
	"swprintf":                       "memory",
	"symlink":                        "io",
	"syslog":                         "stdio",
	"system":                         "exec",
	"tan":                            "libm",
	"tempnam":                        "io",
	"tgamma":                         "libm",
	"tmpfile":                        "reentrancy",
	"tmpnam":                         "io",
	"trunc":                          "libm",
	"truncate":                       "io",
	"ttyname":                        "reentrancy",
	"umask":                          "io",
	"unlink":                         "io",
	"unlinkat":                       "io",
	"valloc":                         "alloc",
	"vasprintf":                      "memory",
	"vfork":                          "exec",
	"vfprintf":                       "stdio",
	"vfscanf":                        "memory",
	"vprintf":                        "stdio",
	"vsnprintf":                      "stdio",
	"vsprintf":                       "memory",
	"vsscanf":                        "memory",
	"wcscat":                         "memory",
	"wcscpy":                         "memory",
	"wcsncpy":                        "memory",
	"wordexp":                        "exec",
	"write":                          "io",
	"writev":                         "io",
	"xcalloc":                        "alloc",
	"xmalloc":                        "alloc",
	"xrealloc":                       "alloc",
	"xstrdup":                        "alloc",
}

var hazardCategories = [...]string{"memory", "alloc", "io", "stdio", "exec", "libm", "integer", "concurrency", "control", "reentrancy"}

var libcKnown = map[string]bool{
	"_exit":                          true,
	"abort":                          true,
	"abs":                            true,
	"accept":                         true,
	"accept4":                        true,
	"access":                         true,
	"acos":                           true,
	"acosh":                          true,
	"alarm":                          true,
	"aligned_alloc":                  true,
	"alignof":                        true,
	"alphasort":                      true,
	"arc4random":                     true,
	"asctime":                        true,
	"asctime_r":                      true,
	"asin":                           true,
	"asinh":                          true,
	"asprintf":                       true,
	"atan":                           true,
	"atan2":                          true,
	"atanh":                          true,
	"atexit":                         true,
	"atoi":                           true,
	"atol":                           true,
	"atoll":                          true,
	"atomic_compare_exchange_strong": true,
	"atomic_compare_exchange_weak":   true,
	"atomic_exchange":                true,
	"atomic_fetch_add":               true,
	"atomic_fetch_and":               true,
	"atomic_fetch_or":                true,
	"atomic_fetch_sub":               true,
	"atomic_fetch_xor":               true,
	"atomic_flag_test_and_set":       true,
	"atomic_init":                    true,
	"atomic_is_lock_free":            true,
	"atomic_load":                    true,
	"atomic_store":                   true,
	"atomic_thread_fence":            true,
	"backtrace":                      true,
	"backtrace_symbols":              true,
	"basename":                       true,
	"bcopy":                          true,
	"bind":                           true,
	"brk":                            true,
	"bsearch":                        true,
	"bzero":                          true,
	"calloc":                         true,
	"cbrt":                           true,
	"ceil":                           true,
	"chdir":                          true,
	"chmod":                          true,
	"chown":                          true,
	"clearerr":                       true,
	"clock":                          true,
	"clock_getres":                   true,
	"clock_gettime":                  true,
	"clock_settime":                  true,
	"close":                          true,
	"closedir":                       true,
	"closelog":                       true,
	"connect":                        true,
	"copysign":                       true,
	"cos":                            true,
	"cosh":                           true,
	"creat":                          true,
	"crypt":                          true,
	"ctime":                          true,
	"ctime_r":                        true,
	"daemon":                         true,
	"difftime":                       true,
	"dirname":                        true,
	"div":                            true,
	"dlclose":                        true,
	"dlerror":                        true,
	"dlopen":                         true,
	"dlsym":                          true,
	"dprintf":                        true,
	"dup":                            true,
	"dup2":                           true,
	"epoll_create":                   true,
	"epoll_create1":                  true,
	"epoll_ctl":                      true,
	"epoll_wait":                     true,
	"erf":                            true,
	"erfc":                           true,
	"execl":                          true,
	"execle":                         true,
	"execlp":                         true,
	"execv":                          true,
	"execve":                         true,
	"execvp":                         true,
	"execvpe":                        true,
	"exit":                           true,
	"exp":                            true,
	"exp2":                           true,
	"expf":                           true,
	"expm1":                          true,
	"fabs":                           true,
	"fabsf":                          true,
	"fchmod":                         true,
	"fchown":                         true,
	"fclose":                         true,
	"fcntl":                          true,
	"fdatasync":                      true,
	"fdim":                           true,
	"fdopen":                         true,
	"feof":                           true,
	"ferror":                         true,
	"fflush":                         true,
	"fgetc":                          true,
	"fgets":                          true,
	"fileno":                         true,
	"floor":                          true,
	"fmax":                           true,
	"fmin":                           true,
	"fmod":                           true,
	"fmodf":                          true,
	"fopen":                          true,
	"fork":                           true,
	"fprintf":                        true,
	"fputc":                          true,
	"fputs":                          true,
	"fread":                          true,
	"free":                           true,
	"freeaddrinfo":                   true,
	"freopen":                        true,
	"frexp":                          true,
	"fscanf":                         true,
	"fseek":                          true,
	"fseeko":                         true,
	"fstat":                          true,
	"fstatat":                        true,
	"fsync":                          true,
	"ftell":                          true,
	"ftello":                         true,
	"ftok":                           true,
	"ftruncate":                      true,
	"fwrite":                         true,
	"gai_strerror":                   true,
	"getaddrinfo":                    true,
	"getc":                           true,
	"getchar":                        true,
	"getcwd":                         true,
	"getegid":                        true,
	"getenv":                         true,
	"geteuid":                        true,
	"getgid":                         true,
	"getgrgid":                       true,
	"getgrnam":                       true,
	"gethostbyname":                  true,
	"gethostbyname_r":                true,
	"getnameinfo":                    true,
	"getopt":                         true,
	"getopt_long":                    true,
	"getpeername":                    true,
	"getpgid":                        true,
	"getpid":                         true,
	"getppid":                        true,
	"getpwnam":                       true,
	"getpwuid":                       true,
	"getrlimit":                      true,
	"getrusage":                      true,
	"gets":                           true,
	"getsockname":                    true,
	"getsockopt":                     true,
	"gettimeofday":                   true,
	"getuid":                         true,
	"glob":                           true,
	"globfree":                       true,
	"gmtime":                         true,
	"gmtime_r":                       true,
	"htonl":                          true,
	"htons":                          true,
	"hypot":                          true,
	"iconv":                          true,
	"iconv_close":                    true,
	"iconv_open":                     true,
	"inet_addr":                      true,
	"inet_ntoa":                      true,
	"inet_ntop":                      true,
	"inet_pton":                      true,
	"ioctl":                          true,
	"isalnum":                        true,
	"isalpha":                        true,
	"isatty":                         true,
	"isblank":                        true,
	"iscntrl":                        true,
	"isdigit":                        true,
	"isfinite":                       true,
	"isgraph":                        true,
	"isinf":                          true,
	"islower":                        true,
	"isnan":                          true,
	"isprint":                        true,
	"ispunct":                        true,
	"isspace":                        true,
	"isupper":                        true,
	"isxdigit":                       true,
	"kevent":                         true,
	"kill":                           true,
	"killpg":                         true,
	"kqueue":                         true,
	"labs":                           true,
	"ldexp":                          true,
	"ldiv":                           true,
	"lgamma":                         true,
	"link":                           true,
	"listen":                         true,
	"llabs":                          true,
	"lldiv":                          true,
	"llround":                        true,
	"localtime":                      true,
	"localtime_r":                    true,
	"log":                            true,
	"log10":                          true,
	"log1p":                          true,
	"log2":                           true,
	"logf":                           true,
	"longjmp":                        true,
	"lround":                         true,
	"lseek":                          true,
	"lstat":                          true,
	"madvise":                        true,
	"malloc":                         true,
	"mbstowcs":                       true,
	"memalign":                       true,
	"memchr":                         true,
	"memcmp":                         true,
	"memcpy":                         true,
	"memmove":                        true,
	"mempcpy":                        true,
	"memrchr":                        true,
	"memset":                         true,
	"mkdir":                          true,
	"mkdtemp":                        true,
	"mkstemp":                        true,
	"mktime":                         true,
	"mlock":                          true,
	"mmap":                           true,
	"modf":                           true,
	"mprotect":                       true,
	"msgget":                         true,
	"msgrcv":                         true,
	"msgsnd":                         true,
	"msync":                          true,
	"munlock":                        true,
	"munmap":                         true,
	"nan":                            true,
	"nanosleep":                      true,
	"nearbyint":                      true,
	"nice":                           true,
	"ntohl":                          true,
	"ntohs":                          true,
	"offsetof":                       true,
	"open":                           true,
	"openat":                         true,
	"opendir":                        true,
	"openlog":                        true,
	"pause":                          true,
	"pclose":                         true,
	"perror":                         true,
	"pipe":                           true,
	"pipe2":                          true,
	"poll":                           true,
	"popen":                          true,
	"posix_memalign":                 true,
	"posix_spawn":                    true,
	"posix_spawnp":                   true,
	"pow":                            true,
	"powf":                           true,
	"ppoll":                          true,
	"pread":                          true,
	"printf":                         true,
	"pselect":                        true,
	"pthread_atfork":                 true,
	"pthread_attr_destroy":           true,
	"pthread_attr_init":              true,
	"pthread_attr_setdetachstate":    true,
	"pthread_attr_setstacksize":      true,
	"pthread_cancel":                 true,
	"pthread_cond_broadcast":         true,
	"pthread_cond_destroy":           true,
	"pthread_cond_init":              true,
	"pthread_cond_signal":            true,
	"pthread_cond_timedwait":         true,
	"pthread_cond_wait":              true,
	"pthread_create":                 true,
	"pthread_detach":                 true,
	"pthread_equal":                  true,
	"pthread_exit":                   true,
	"pthread_getname_np":             true,
	"pthread_getspecific":            true,
	"pthread_join":                   true,
	"pthread_key_create":             true,
	"pthread_key_delete":             true,
	"pthread_kill":                   true,
	"pthread_mutex_destroy":          true,
	"pthread_mutex_init":             true,
	"pthread_mutex_lock":             true,
	"pthread_mutex_trylock":          true,
	"pthread_mutex_unlock":           true,
	"pthread_mutexattr_destroy":      true,
	"pthread_mutexattr_init":         true,
	"pthread_mutexattr_settype":      true,
	"pthread_once":                   true,
	"pthread_rwlock_destroy":         true,
	"pthread_rwlock_init":            true,
	"pthread_rwlock_rdlock":          true,
	"pthread_rwlock_tryrdlock":       true,
	"pthread_rwlock_trywrlock":       true,
	"pthread_rwlock_unlock":          true,
	"pthread_rwlock_wrlock":          true,
	"pthread_self":                   true,
	"pthread_setname_np":             true,
	"pthread_setspecific":            true,
	"putc":                           true,
	"putchar":                        true,
	"putenv":                         true,
	"puts":                           true,
	"pwrite":                         true,
	"qsort":                          true,
	"raise":                          true,
	"rand":                           true,
	"random":                         true,
	"read":                           true,
	"readdir":                        true,
	"readlink":                       true,
	"readv":                          true,
	"realloc":                        true,
	"reallocarray":                   true,
	"realpath":                       true,
	"recv":                           true,
	"recvfrom":                       true,
	"recvmsg":                        true,
	"remainder":                      true,
	"remove":                         true,
	"rename":                         true,
	"rewind":                         true,
	"rewinddir":                      true,
	"rint":                           true,
	"rmdir":                          true,
	"round":                          true,
	"sbrk":                           true,
	"scandir":                        true,
	"scanf":                          true,
	"sched_getaffinity":              true,
	"sched_setaffinity":              true,
	"sched_yield":                    true,
	"select":                         true,
	"sem_destroy":                    true,
	"sem_getvalue":                   true,
	"sem_init":                       true,
	"sem_post":                       true,
	"sem_trywait":                    true,
	"sem_wait":                       true,
	"semget":                         true,
	"semop":                          true,
	"send":                           true,
	"sendmsg":                        true,
	"sendto":                         true,
	"setbuf":                         true,
	"setegid":                        true,
	"setenv":                         true,
	"seteuid":                        true,
	"setgid":                         true,
	"setjmp":                         true,
	"setpgid":                        true,
	"setrlimit":                      true,
	"setsid":                         true,
	"setsockopt":                     true,
	"settimeofday":                   true,
	"setuid":                         true,
	"setvbuf":                        true,
	"shmat":                          true,
	"shmctl":                         true,
	"shmdt":                          true,
	"shmget":                         true,
	"shutdown":                       true,
	"sigaction":                      true,
	"sigaddset":                      true,
	"sigdelset":                      true,
	"sigemptyset":                    true,
	"sigfillset":                     true,
	"sigismember":                    true,
	"siglongjmp":                     true,
	"signal":                         true,
	"signbit":                        true,
	"sigprocmask":                    true,
	"sigsetjmp":                      true,
	"sigsuspend":                     true,
	"sigwait":                        true,
	"sin":                            true,
	"sinh":                           true,
	"sizeof":                         true,
	"sleep":                          true,
	"snprintf":                       true,
	"socket":                         true,
	"socketpair":                     true,
	"sprintf":                        true,
	"sqrt":                           true,
	"sqrtf":                          true,
	"sqrtl":                          true,
	"srand":                          true,
	"srandom":                        true,
	"sscanf":                         true,
	"stat":                           true,
	"strcasecmp":                     true,
	"strcasestr":                     true,
	"strcat":                         true,
	"strchr":                         true,
	"strcmp":                         true,
	"strcpy":                         true,
	"strcspn":                        true,
	"strdup":                         true,
	"strdupa":                        true,
	"strerror":                       true,
	"strerror_r":                     true,
	"strftime":                       true,
	"strlcat":                        true,
	"strlcpy":                        true,
	"strlen":                         true,
	"strncasecmp":                    true,
	"strncat":                        true,
	"strncmp":                        true,
	"strncpy":                        true,
	"strndup":                        true,
	"strnlen":                        true,
	"strpbrk":                        true,
	"strptime":                       true,
	"strrchr":                        true,
	"strsep":                         true,
	"strsignal":                      true,
	"strspn":                         true,
	"strstr":                         true,
	"strtod":                         true,
	"strtof":                         true,
	"strtok":                         true,
	"strtok_r":                       true,
	"strtol":                         true,
	"strtold":                        true,
	"strtoll":                        true,
	"strtoul":                        true,
	"strtoull":                       true,
	"symlink":                        true,
	"sync":                           true,
	"sysconf":                        true,
	"syslog":                         true,
	"system":                         true,
	"tan":                            true,
	"tanh":                           true,
	"tempnam":                        true,
	"tgamma":                         true,
	"time":                           true,
	"timegm":                         true,
	"tmpfile":                        true,
	"tmpnam":                         true,
	"tolower":                        true,
	"toupper":                        true,
	"trunc":                          true,
	"truncate":                       true,
	"ttyname":                        true,
	"umask":                          true,
	"uname":                          true,
	"ungetc":                         true,
	"unlink":                         true,
	"unlinkat":                       true,
	"unsetenv":                       true,
	"usleep":                         true,
	"va_arg":                         true,
	"va_copy":                        true,
	"va_end":                         true,
	"va_start":                       true,
	"valloc":                         true,
	"vasprintf":                      true,
	"vdprintf":                       true,
	"vfork":                          true,
	"vfprintf":                       true,
	"vprintf":                        true,
	"vsnprintf":                      true,
	"vsprintf":                       true,
	"vsscanf":                        true,
	"wait":                           true,
	"wait3":                          true,
	"wait4":                          true,
	"waitpid":                        true,
	"wcscpy":                         true,
	"wcslen":                         true,
	"wcstombs":                       true,
	"write":                          true,
	"writev":                         true,
}

var builtinPrefixes = [...]string{"__builtin_", "__atomic_", "__sync_", "__asan_", "__msan_", "__tsan_", "__c11_atomic_", "_InterlockedExchange"}

var funcToNS = map[string]string{
	"FD_CLR":                                 "sys/select.h",
	"FD_COPY":                                "sys/select.h",
	"FD_ISSET":                               "sys/select.h",
	"FD_SET":                                 "sys/select.h",
	"FD_ZERO":                                "sys/select.h",
	"F_DUPFD":                                "fcntl.h",
	"F_DUPFD_CLOEXEC":                        "fcntl.h",
	"F_GETFD":                                "fcntl.h",
	"F_GETFL":                                "fcntl.h",
	"F_GETLK":                                "fcntl.h",
	"F_RDLCK":                                "fcntl.h",
	"F_SETFD":                                "fcntl.h",
	"F_SETFL":                                "fcntl.h",
	"F_SETLK":                                "fcntl.h",
	"F_SETLKW":                               "fcntl.h",
	"F_UNLCK":                                "fcntl.h",
	"F_WRLCK":                                "fcntl.h",
	"O_CLOEXEC":                              "fcntl.h",
	"_Exit":                                  "stdlib.h",
	"__builtin_add_overflow":                 "__builtin_*",
	"__builtin_alloca":                       "__builtin_*",
	"__builtin_bswap16":                      "__builtin_*",
	"__builtin_bswap32":                      "__builtin_*",
	"__builtin_bswap64":                      "__builtin_*",
	"__builtin_choose_expr":                  "__builtin_*",
	"__builtin_clz":                          "__builtin_*",
	"__builtin_clzl":                         "__builtin_*",
	"__builtin_clzll":                        "__builtin_*",
	"__builtin_constant_p":                   "__builtin_*",
	"__builtin_counted_by_ref":               "__builtin_*",
	"__builtin_ctz":                          "__builtin_*",
	"__builtin_ctzl":                         "__builtin_*",
	"__builtin_ctzll":                        "__builtin_*",
	"__builtin_debugtrap":                    "__builtin_*",
	"__builtin_expect":                       "__builtin_*",
	"__builtin_extract_return_addr":          "__builtin_*",
	"__builtin_frame_address":                "__builtin_*",
	"__builtin_ia32_pause":                   "__builtin_*",
	"__builtin_memcmp":                       "__builtin_*",
	"__builtin_memcpy":                       "__builtin_*",
	"__builtin_memset":                       "__builtin_*",
	"__builtin_mul_overflow":                 "__builtin_*",
	"__builtin_offsetof":                     "__builtin_*",
	"__builtin_popcount":                     "__builtin_*",
	"__builtin_popcountl":                    "__builtin_*",
	"__builtin_popcountll":                   "__builtin_*",
	"__builtin_prefetch":                     "__builtin_*",
	"__builtin_return_address":               "__builtin_*",
	"__builtin_strcpy":                       "__builtin_*",
	"__builtin_strlen":                       "__builtin_*",
	"__builtin_sub_overflow":                 "__builtin_*",
	"__builtin_trap":                         "__builtin_*",
	"__builtin_types_compatible_p":           "__builtin_*",
	"__builtin_unreachable":                  "__builtin_*",
	"_exit":                                  "unistd.h",
	"_longjmp":                               "setjmp.h",
	"_setjmp":                                "setjmp.h",
	"_tolower":                               "ctype.h",
	"_toupper":                               "ctype.h",
	"a64l":                                   "stdlib.h",
	"abort":                                  "stdlib.h",
	"abs":                                    "stdlib.h",
	"accept":                                 "sys/socket.h",
	"accept4":                                "sys/socket.h",
	"access":                                 "unistd.h",
	"accessx_np":                             "unistd.h",
	"acct":                                   "unistd.h",
	"acos":                                   "math.h",
	"acosf":                                  "math.h",
	"acosh":                                  "math.h",
	"acoshf":                                 "math.h",
	"acoshl":                                 "math.h",
	"acosl":                                  "math.h",
	"add_profil":                             "unistd.h",
	"adjtime":                                "sys/time.h",
	"alarm":                                  "unistd.h",
	"aligned_alloc":                          "stdlib.h",
	"alloca":                                 "stdlib.h",
	"alphasort":                              "dirent.h",
	"arc4random":                             "stdlib.h",
	"arc4random_addrandom":                   "stdlib.h",
	"arc4random_buf":                         "stdlib.h",
	"arc4random_stir":                        "stdlib.h",
	"arc4random_uniform":                     "stdlib.h",
	"asctime":                                "time.h",
	"asctime_r":                              "time.h",
	"asin":                                   "math.h",
	"asinf":                                  "math.h",
	"asinh":                                  "math.h",
	"asinhf":                                 "math.h",
	"asinhl":                                 "math.h",
	"asinl":                                  "math.h",
	"asprintf":                               "stdio.h",
	"assert":                                 "assert.h",
	"at_quick_exit":                          "stdlib.h",
	"atan":                                   "math.h",
	"atan2":                                  "math.h",
	"atan2f":                                 "math.h",
	"atan2l":                                 "math.h",
	"atanf":                                  "math.h",
	"atanh":                                  "math.h",
	"atanhf":                                 "math.h",
	"atanhl":                                 "math.h",
	"atanl":                                  "math.h",
	"atexit":                                 "stdlib.h",
	"atexit_b":                               "stdlib.h",
	"atof":                                   "stdlib.h",
	"atoi":                                   "stdlib.h",
	"atol":                                   "stdlib.h",
	"atoll":                                  "stdlib.h",
	"bcmp":                                   "string.h",
	"bcopy":                                  "string.h",
	"bind":                                   "sys/socket.h",
	"brk":                                    "unistd.h",
	"bsd_signal":                             "signal.h",
	"bsearch":                                "stdlib.h",
	"bsearch_b":                              "stdlib.h",
	"btowc":                                  "wchar.h",
	"bzero":                                  "string.h",
	"calloc":                                 "stdlib.h",
	"cbrt":                                   "math.h",
	"cbrtf":                                  "math.h",
	"cbrtl":                                  "math.h",
	"ceil":                                   "math.h",
	"ceilf":                                  "math.h",
	"ceill":                                  "math.h",
	"cgetcap":                                "stdlib.h",
	"cgetclose":                              "stdlib.h",
	"cgetent":                                "stdlib.h",
	"cgetfirst":                              "stdlib.h",
	"cgetmatch":                              "stdlib.h",
	"cgetnext":                               "stdlib.h",
	"cgetnum":                                "stdlib.h",
	"cgetset":                                "stdlib.h",
	"cgetstr":                                "stdlib.h",
	"cgetustr":                               "stdlib.h",
	"chdir":                                  "unistd.h",
	"checkuseraccess":                        "fcntl.h",
	"chflags":                                "sys/stat.h",
	"chmod":                                  "sys/stat.h",
	"chmodx_np":                              "sys/stat.h",
	"chown":                                  "unistd.h",
	"chroot":                                 "unistd.h",
	"clearerr":                               "stdio.h",
	"clearerr_unlocked":                      "stdio.h",
	"clock":                                  "time.h",
	"clock_getres":                           "time.h",
	"clock_gettime":                          "time.h",
	"clock_gettime_nsec_np":                  "time.h",
	"clock_settime":                          "time.h",
	"close":                                  "unistd.h",
	"close_range":                            "unistd.h",
	"closedir":                               "dirent.h",
	"closefrom":                              "unistd.h",
	"closelog":                               "syslog.h",
	"confstr":                                "unistd.h",
	"connect":                                "sys/socket.h",
	"connectx":                               "sys/socket.h",
	"copysign":                               "math.h",
	"copysignf":                              "math.h",
	"copysignl":                              "math.h",
	"cos":                                    "math.h",
	"cosf":                                   "math.h",
	"cosh":                                   "math.h",
	"coshf":                                  "math.h",
	"coshl":                                  "math.h",
	"cosl":                                   "math.h",
	"creat":                                  "fcntl.h",
	"crypt":                                  "unistd.h",
	"ctermid_r":                              "stdio.h",
	"ctime":                                  "time.h",
	"ctime_r":                                "time.h",
	"daemon":                                 "stdlib.h",
	"devname":                                "stdlib.h",
	"devname_r":                              "stdlib.h",
	"difftime":                               "time.h",
	"digittoint":                             "ctype.h",
	"dirfd":                                  "dirent.h",
	"disconnectx":                            "sys/socket.h",
	"div":                                    "stdlib.h",
	"dladdr":                                 "dlfcn.h",
	"dlclose":                                "dlfcn.h",
	"dlerror":                                "dlfcn.h",
	"dlmopen":                                "dlfcn.h",
	"dlopen":                                 "dlfcn.h",
	"dlopen_preflight":                       "dlfcn.h",
	"dlsym":                                  "dlfcn.h",
	"dlvsym":                                 "dlfcn.h",
	"dprintf":                                "stdio.h",
	"drand48":                                "stdlib.h",
	"drem":                                   "math.h",
	"dup":                                    "unistd.h",
	"dup2":                                   "unistd.h",
	"dup3":                                   "unistd.h",
	"duplocale":                              "locale.h",
	"ecvt":                                   "stdlib.h",
	"elif":                                   "math.h",
	"encrypt":                                "unistd.h",
	"endusershell":                           "unistd.h",
	"erand48":                                "stdlib.h",
	"erf":                                    "math.h",
	"erfc":                                   "math.h",
	"erfcf":                                  "math.h",
	"erfcl":                                  "math.h",
	"erff":                                   "math.h",
	"erfl":                                   "math.h",
	"errno":                                  "errno.h",
	"exchangedata":                           "fcntl.h",
	"execl":                                  "unistd.h",
	"execle":                                 "unistd.h",
	"execlp":                                 "unistd.h",
	"execv":                                  "unistd.h",
	"execvP":                                 "unistd.h",
	"execve":                                 "unistd.h",
	"execveat":                               "unistd.h",
	"execvp":                                 "unistd.h",
	"execvpe":                                "unistd.h",
	"exit":                                   "stdlib.h",
	"exp":                                    "math.h",
	"exp2":                                   "math.h",
	"exp2f":                                  "math.h",
	"exp2l":                                  "math.h",
	"expf":                                   "math.h",
	"expl":                                   "math.h",
	"explicit_bzero":                         "string.h",
	"expm1":                                  "math.h",
	"expm1f":                                 "math.h",
	"expm1l":                                 "math.h",
	"fabs":                                   "math.h",
	"fabsf":                                  "math.h",
	"fabsl":                                  "math.h",
	"faccessat":                              "unistd.h",
	"fchdir":                                 "unistd.h",
	"fchflags":                               "sys/stat.h",
	"fchmod":                                 "sys/stat.h",
	"fchmodat":                               "sys/stat.h",
	"fchmodx_np":                             "sys/stat.h",
	"fchown":                                 "unistd.h",
	"fclose":                                 "stdio.h",
	"fcntl":                                  "fcntl.h",
	"fcntl_getpath":                          "fcntl.h",
	"fcntl_nosync":                           "fcntl.h",
	"fcvt":                                   "stdlib.h",
	"fdatasync":                              "unistd.h",
	"fdclosedir":                             "dirent.h",
	"fdim":                                   "math.h",
	"fdimf":                                  "math.h",
	"fdiml":                                  "math.h",
	"fdopen":                                 "stdio.h",
	"fdopendir":                              "dirent.h",
	"fdscandir":                              "dirent.h",
	"fdscandir_b":                            "dirent.h",
	"feof":                                   "stdio.h",
	"feof_unlocked":                          "stdio.h",
	"ferror":                                 "stdio.h",
	"ferror_unlocked":                        "stdio.h",
	"fexecve":                                "unistd.h",
	"fflagstostr":                            "unistd.h",
	"fflush":                                 "stdio.h",
	"ffs":                                    "string.h",
	"ffsctl":                                 "unistd.h",
	"ffsl":                                   "string.h",
	"ffsll":                                  "string.h",
	"fgetattrlist":                           "unistd.h",
	"fgetc":                                  "stdio.h",
	"fgetln":                                 "stdio.h",
	"fgetpos":                                "stdio.h",
	"fgets":                                  "stdio.h",
	"fgetwc":                                 "wchar.h",
	"fgetwln":                                "wchar.h",
	"fgetws":                                 "wchar.h",
	"fileno":                                 "stdio.h",
	"fileno_unlocked":                        "stdio.h",
	"finite":                                 "math.h",
	"flock":                                  "fcntl.h",
	"flockfile":                              "stdio.h",
	"floor":                                  "math.h",
	"floorf":                                 "math.h",
	"floorl":                                 "math.h",
	"fls":                                    "string.h",
	"flsl":                                   "string.h",
	"flsll":                                  "string.h",
	"fma":                                    "math.h",
	"fmaf":                                   "math.h",
	"fmal":                                   "math.h",
	"fmax":                                   "math.h",
	"fmaxf":                                  "math.h",
	"fmaxl":                                  "math.h",
	"fmemopen":                               "stdio.h",
	"fmin":                                   "math.h",
	"fminf":                                  "math.h",
	"fminl":                                  "math.h",
	"fmod":                                   "math.h",
	"fmodf":                                  "math.h",
	"fmodl":                                  "math.h",
	"fmtcheck":                               "stdio.h",
	"fopen":                                  "stdio.h",
	"fork":                                   "unistd.h",
	"format_arg":                             "stdio.h",
	"fpathconf":                              "unistd.h",
	"fpclassify":                             "math.h",
	"fpos_t":                                 "stdio.h",
	"fprintf":                                "stdio.h",
	"fpurge":                                 "stdio.h",
	"fputc":                                  "stdio.h",
	"fputs":                                  "stdio.h",
	"fputwc":                                 "wchar.h",
	"fputws":                                 "wchar.h",
	"fread":                                  "stdio.h",
	"free":                                   "stdlib.h",
	"free_aligned_sized":                     "stdlib.h",
	"free_sized":                             "stdlib.h",
	"freelocale":                             "locale.h",
	"freopen":                                "stdio.h",
	"frexp":                                  "math.h",
	"frexpf":                                 "math.h",
	"frexpl":                                 "math.h",
	"fropen":                                 "stdio.h",
	"fscanf":                                 "stdio.h",
	"fsctl":                                  "unistd.h",
	"fseek":                                  "stdio.h",
	"fseeko":                                 "stdio.h",
	"fsetattrlist":                           "unistd.h",
	"fsetpos":                                "stdio.h",
	"fstat":                                  "sys/stat.h",
	"fstat64":                                "sys/stat.h",
	"fstatat":                                "sys/stat.h",
	"fstatx64_np":                            "sys/stat.h",
	"fstatx_np":                              "sys/stat.h",
	"fsync":                                  "unistd.h",
	"fsync_volume_np":                        "unistd.h",
	"ftell":                                  "stdio.h",
	"ftello":                                 "stdio.h",
	"ftruncate":                              "unistd.h",
	"ftrylockfile":                           "stdio.h",
	"funlockfile":                            "stdio.h",
	"funopen":                                "stdio.h",
	"futimens":                               "fcntl.h",
	"futimes":                                "fcntl.h",
	"fwide":                                  "wchar.h",
	"fwopen":                                 "stdio.h",
	"fwprintf":                               "wchar.h",
	"fwrite":                                 "stdio.h",
	"fwscanf":                                "wchar.h",
	"gamma":                                  "math.h",
	"gcvt":                                   "stdlib.h",
	"getattrlist":                            "fcntl.h",
	"getattrlistat":                          "fcntl.h",
	"getbsize":                               "stdlib.h",
	"getc":                                   "stdio.h",
	"getc_unlocked":                          "stdio.h",
	"getchar":                                "stdio.h",
	"getchar_unlocked":                       "stdio.h",
	"getcwd":                                 "unistd.h",
	"getdate":                                "time.h",
	"getdelim":                               "stdio.h",
	"getdirentries":                          "dirent.h",
	"getdirentriesattr":                      "unistd.h",
	"getdomainname":                          "unistd.h",
	"getdtablesize":                          "unistd.h",
	"getegid":                                "unistd.h",
	"getenv":                                 "stdlib.h",
	"geteuid":                                "unistd.h",
	"getgid":                                 "unistd.h",
	"getgrouplist":                           "unistd.h",
	"getgroups":                              "unistd.h",
	"gethostid":                              "unistd.h",
	"gethostname":                            "unistd.h",
	"getiopolicy_np":                         "sys/resource.h",
	"getitimer":                              "sys/time.h",
	"getline":                                "stdio.h",
	"getloadavg":                             "stdlib.h",
	"getlogin":                               "unistd.h",
	"getlogin_r":                             "unistd.h",
	"getmode":                                "unistd.h",
	"getopt":                                 "getopt.h",
	"getopt_long":                            "getopt.h",
	"getopt_long_only":                       "getopt.h",
	"getpagesize":                            "unistd.h",
	"getpass":                                "unistd.h",
	"getpath_np":                             "fcntl.h",
	"getpeereid":                             "unistd.h",
	"getpeername":                            "sys/socket.h",
	"getpgid":                                "unistd.h",
	"getpgrp":                                "unistd.h",
	"getpid":                                 "unistd.h",
	"getppid":                                "unistd.h",
	"getpriority":                            "sys/resource.h",
	"getprogname":                            "stdlib.h",
	"getrlimit":                              "sys/resource.h",
	"getrusage":                              "sys/resource.h",
	"gets":                                   "stdio.h",
	"getsgroups_np":                          "unistd.h",
	"getsid":                                 "unistd.h",
	"getsockname":                            "sys/socket.h",
	"getsockopt":                             "sys/socket.h",
	"getsubopt":                              "stdlib.h",
	"gettimeofday":                           "sys/time.h",
	"getuid":                                 "unistd.h",
	"getusershell":                           "unistd.h",
	"getw":                                   "stdio.h",
	"getwc":                                  "wchar.h",
	"getwchar":                               "wchar.h",
	"getwd":                                  "unistd.h",
	"getwgroups_np":                          "unistd.h",
	"gmtime":                                 "time.h",
	"gmtime_r":                               "time.h",
	"grantpt":                                "stdlib.h",
	"heapsort":                               "stdlib.h",
	"heapsort_b":                             "stdlib.h",
	"hypot":                                  "math.h",
	"hypotf":                                 "math.h",
	"hypotl":                                 "math.h",
	"ilogb":                                  "math.h",
	"ilogbf":                                 "math.h",
	"ilogbl":                                 "math.h",
	"index":                                  "string.h",
	"initgroups":                             "unistd.h",
	"initstate":                              "stdlib.h",
	"iruserok":                               "unistd.h",
	"iruserok_sa":                            "unistd.h",
	"isalnum":                                "ctype.h",
	"isalpha":                                "ctype.h",
	"isascii":                                "ctype.h",
	"isatty":                                 "unistd.h",
	"isblank":                                "ctype.h",
	"iscntrl":                                "ctype.h",
	"isdigit":                                "ctype.h",
	"isfinite":                               "math.h",
	"isgraph":                                "ctype.h",
	"isgreater":                              "math.h",
	"isgreaterequal":                         "math.h",
	"ishexnumber":                            "ctype.h",
	"isideogram":                             "ctype.h",
	"isinf":                                  "math.h",
	"isless":                                 "math.h",
	"islessequal":                            "math.h",
	"islessgreater":                          "math.h",
	"islower":                                "ctype.h",
	"isnan":                                  "math.h",
	"isnormal":                               "math.h",
	"isnumber":                               "ctype.h",
	"isphonogram":                            "ctype.h",
	"isprint":                                "ctype.h",
	"ispunct":                                "ctype.h",
	"isrune":                                 "ctype.h",
	"issetugid":                              "unistd.h",
	"isspace":                                "ctype.h",
	"isspecial":                              "ctype.h",
	"isunordered":                            "math.h",
	"isupper":                                "ctype.h",
	"isxdigit":                               "ctype.h",
	"j0":                                     "math.h",
	"j1":                                     "math.h",
	"jn":                                     "math.h",
	"jrand48":                                "stdlib.h",
	"kdebug_signpost":                        "unistd.h",
	"kill":                                   "signal.h",
	"killpg":                                 "signal.h",
	"l64a":                                   "stdlib.h",
	"labs":                                   "stdlib.h",
	"lchflags":                               "sys/stat.h",
	"lchmod":                                 "sys/stat.h",
	"lchown":                                 "unistd.h",
	"lcong48":                                "stdlib.h",
	"ldexp":                                  "math.h",
	"ldexpf":                                 "math.h",
	"ldexpl":                                 "math.h",
	"ldiv":                                   "stdlib.h",
	"lgamma":                                 "math.h",
	"lgamma_r":                               "math.h",
	"lgammaf":                                "math.h",
	"lgammaf_r":                              "math.h",
	"lgammal":                                "math.h",
	"lgammal_r":                              "math.h",
	"link":                                   "unistd.h",
	"listen":                                 "sys/socket.h",
	"llabs":                                  "stdlib.h",
	"lldiv":                                  "stdlib.h",
	"llrint":                                 "math.h",
	"llrintf":                                "math.h",
	"llrintl":                                "math.h",
	"llround":                                "math.h",
	"llroundf":                               "math.h",
	"llroundl":                               "math.h",
	"localeconv":                             "locale.h",
	"localtime":                              "time.h",
	"localtime_r":                            "time.h",
	"lockf":                                  "unistd.h",
	"log":                                    "math.h",
	"log10":                                  "math.h",
	"log10f":                                 "math.h",
	"log10l":                                 "math.h",
	"log1p":                                  "math.h",
	"log1pf":                                 "math.h",
	"log1pl":                                 "math.h",
	"log2":                                   "math.h",
	"log2f":                                  "math.h",
	"log2l":                                  "math.h",
	"logb":                                   "math.h",
	"logbf":                                  "math.h",
	"logbl":                                  "math.h",
	"logf":                                   "math.h",
	"logl":                                   "math.h",
	"longjmp":                                "setjmp.h",
	"longjmperror":                           "setjmp.h",
	"lrand48":                                "stdlib.h",
	"lrint":                                  "math.h",
	"lrintf":                                 "math.h",
	"lrintl":                                 "math.h",
	"lround":                                 "math.h",
	"lroundf":                                "math.h",
	"lroundl":                                "math.h",
	"lseek":                                  "unistd.h",
	"lstat":                                  "sys/stat.h",
	"lstat64":                                "sys/stat.h",
	"lstatx64_np":                            "sys/stat.h",
	"lstatx_np":                              "sys/stat.h",
	"lutimes":                                "sys/time.h",
	"madvise":                                "sys/mman.h",
	"mallinfo":                               "stdlib.h",
	"mallinfo2":                              "stdlib.h",
	"malloc":                                 "stdlib.h",
	"mallopt":                                "stdlib.h",
	"math_errhandling":                       "math.h",
	"mblen":                                  "stdlib.h",
	"mbrlen":                                 "wchar.h",
	"mbrtowc":                                "wchar.h",
	"mbsinit":                                "wchar.h",
	"mbsnrtowcs":                             "wchar.h",
	"mbsrtowcs":                              "wchar.h",
	"mbstowcs":                               "stdlib.h",
	"mbtowc":                                 "stdlib.h",
	"memalign":                               "stdlib.h",
	"memalignment":                           "string.h",
	"memccpy":                                "string.h",
	"memchr":                                 "string.h",
	"memcmp":                                 "string.h",
	"memcpy":                                 "string.h",
	"memmem":                                 "string.h",
	"memmove":                                "string.h",
	"memset":                                 "string.h",
	"memset_explicit":                        "string.h",
	"memset_pattern16":                       "string.h",
	"memset_pattern4":                        "string.h",
	"memset_pattern8":                        "string.h",
	"memset_s":                               "string.h",
	"mergesort":                              "stdlib.h",
	"mergesort_b":                            "stdlib.h",
	"mincore":                                "sys/mman.h",
	"minherit":                               "sys/mman.h",
	"mkdir":                                  "sys/stat.h",
	"mkdirat":                                "sys/stat.h",
	"mkdirx_np":                              "sys/stat.h",
	"mkdtemp":                                "stdio.h",
	"mkdtempat_np":                           "unistd.h",
	"mkfifo":                                 "sys/stat.h",
	"mkfifoat":                               "sys/stat.h",
	"mkfifox_np":                             "sys/stat.h",
	"mknod":                                  "sys/stat.h",
	"mknodat":                                "sys/stat.h",
	"mkostemp":                               "unistd.h",
	"mkostemps":                              "unistd.h",
	"mkostempsat_np":                         "unistd.h",
	"mkpath_np":                              "unistd.h",
	"mkpathat_np":                            "unistd.h",
	"mkstemp":                                "stdio.h",
	"mkstemp_dprotected_np":                  "unistd.h",
	"mkstemps":                               "stdio.h",
	"mkstempsat_np":                          "unistd.h",
	"mktemp":                                 "stdlib.h",
	"mktime":                                 "time.h",
	"mlock":                                  "sys/mman.h",
	"mlockall":                               "sys/mman.h",
	"mmap":                                   "sys/mman.h",
	"modf":                                   "math.h",
	"modff":                                  "math.h",
	"modfl":                                  "math.h",
	"mprotect":                               "sys/mman.h",
	"mrand48":                                "stdlib.h",
	"msync":                                  "sys/mman.h",
	"munlock":                                "sys/mman.h",
	"munlockall":                             "sys/mman.h",
	"munmap":                                 "sys/mman.h",
	"nan":                                    "math.h",
	"nanf":                                   "math.h",
	"nanl":                                   "math.h",
	"nanosleep":                              "time.h",
	"nearbyint":                              "math.h",
	"nearbyintf":                             "math.h",
	"nearbyintl":                             "math.h",
	"newlocale":                              "locale.h",
	"nextafter":                              "math.h",
	"nextafterf":                             "math.h",
	"nextafterl":                             "math.h",
	"nexttoward":                             "math.h",
	"nexttowardf":                            "math.h",
	"nexttowardl":                            "math.h",
	"nfssvc":                                 "unistd.h",
	"nice":                                   "unistd.h",
	"nrand48":                                "stdlib.h",
	"open":                                   "fcntl.h",
	"open_memstream":                         "stdio.h",
	"open_wmemstream":                        "wchar.h",
	"openat":                                 "fcntl.h",
	"opendir":                                "dirent.h",
	"openlog":                                "syslog.h",
	"pathconf":                               "unistd.h",
	"pause":                                  "unistd.h",
	"pclose":                                 "stdio.h",
	"perror":                                 "errno.h",
	"pfctlinput":                             "sys/socket.h",
	"pipe":                                   "unistd.h",
	"pipe2":                                  "unistd.h",
	"poll":                                   "sys/select.h",
	"popen":                                  "stdio.h",
	"posix2time":                             "time.h",
	"posix_fadvise":                          "fcntl.h",
	"posix_fallocate":                        "fcntl.h",
	"posix_madvise":                          "sys/mman.h",
	"posix_memalign":                         "stdlib.h",
	"posix_openpt":                           "stdlib.h",
	"pow":                                    "math.h",
	"powf":                                   "math.h",
	"powl":                                   "math.h",
	"ppoll":                                  "sys/select.h",
	"pread":                                  "unistd.h",
	"preadv":                                 "sys/uio.h",
	"printf":                                 "stdio.h",
	"prlimit":                                "sys/resource.h",
	"process_vm_readv":                       "sys/uio.h",
	"process_vm_writev":                      "sys/uio.h",
	"profil":                                 "unistd.h",
	"program_invocation_name":                "errno.h",
	"pselect":                                "sys/select.h",
	"psignal":                                "signal.h",
	"psort":                                  "stdlib.h",
	"psort_b":                                "stdlib.h",
	"psort_r":                                "stdlib.h",
	"pthread_atfork":                         "pthread.h",
	"pthread_attr_destroy":                   "pthread.h",
	"pthread_attr_getdetachstate":            "pthread.h",
	"pthread_attr_getguardsize":              "pthread.h",
	"pthread_attr_getinheritsched":           "pthread.h",
	"pthread_attr_getschedparam":             "pthread.h",
	"pthread_attr_getschedpolicy":            "pthread.h",
	"pthread_attr_getscope":                  "pthread.h",
	"pthread_attr_getstack":                  "pthread.h",
	"pthread_attr_getstackaddr":              "pthread.h",
	"pthread_attr_getstacksize":              "pthread.h",
	"pthread_attr_init":                      "pthread.h",
	"pthread_attr_setdetachstate":            "pthread.h",
	"pthread_attr_setguardsize":              "pthread.h",
	"pthread_attr_setinheritsched":           "pthread.h",
	"pthread_attr_setschedparam":             "pthread.h",
	"pthread_attr_setschedpolicy":            "pthread.h",
	"pthread_attr_setscope":                  "pthread.h",
	"pthread_attr_setstack":                  "pthread.h",
	"pthread_attr_setstackaddr":              "pthread.h",
	"pthread_attr_setstacksize":              "pthread.h",
	"pthread_cancel":                         "pthread.h",
	"pthread_cleanup_pop":                    "pthread.h",
	"pthread_cleanup_push":                   "pthread.h",
	"pthread_cond_broadcast":                 "pthread.h",
	"pthread_cond_destroy":                   "pthread.h",
	"pthread_cond_init":                      "pthread.h",
	"pthread_cond_signal":                    "pthread.h",
	"pthread_cond_signal_thread_np":          "pthread.h",
	"pthread_cond_timedwait":                 "pthread.h",
	"pthread_cond_timedwait_relative_np":     "pthread.h",
	"pthread_cond_wait":                      "pthread.h",
	"pthread_condattr_destroy":               "pthread.h",
	"pthread_condattr_getpshared":            "pthread.h",
	"pthread_condattr_init":                  "pthread.h",
	"pthread_condattr_setpshared":            "pthread.h",
	"pthread_cpu_number_np":                  "pthread.h",
	"pthread_create":                         "pthread.h",
	"pthread_create_suspended_np":            "pthread.h",
	"pthread_detach":                         "pthread.h",
	"pthread_equal":                          "pthread.h",
	"pthread_exit":                           "pthread.h",
	"pthread_from_mach_thread_np":            "pthread.h",
	"pthread_get_stackaddr_np":               "pthread.h",
	"pthread_get_stacksize_np":               "pthread.h",
	"pthread_getconcurrency":                 "pthread.h",
	"pthread_getname_np":                     "pthread.h",
	"pthread_getschedparam":                  "pthread.h",
	"pthread_getspecific":                    "pthread.h",
	"pthread_getugid_np":                     "unistd.h",
	"pthread_is_threaded_np":                 "pthread.h",
	"pthread_jit_write_freeze_callbacks_np":  "pthread.h",
	"pthread_jit_write_protect_np":           "pthread.h",
	"pthread_jit_write_protect_supported_np": "pthread.h",
	"pthread_jit_write_with_callback_np":     "pthread.h",
	"pthread_join":                           "pthread.h",
	"pthread_key_create":                     "pthread.h",
	"pthread_key_delete":                     "pthread.h",
	"pthread_kill":                           "pthread.h",
	"pthread_mach_thread_np":                 "pthread.h",
	"pthread_main_np":                        "pthread.h",
	"pthread_mutex_destroy":                  "pthread.h",
	"pthread_mutex_getprioceiling":           "pthread.h",
	"pthread_mutex_init":                     "pthread.h",
	"pthread_mutex_lock":                     "pthread.h",
	"pthread_mutex_setprioceiling":           "pthread.h",
	"pthread_mutex_trylock":                  "pthread.h",
	"pthread_mutex_unlock":                   "pthread.h",
	"pthread_mutexattr_destroy":              "pthread.h",
	"pthread_mutexattr_getpolicy_np":         "pthread.h",
	"pthread_mutexattr_getprioceiling":       "pthread.h",
	"pthread_mutexattr_getprotocol":          "pthread.h",
	"pthread_mutexattr_getpshared":           "pthread.h",
	"pthread_mutexattr_gettype":              "pthread.h",
	"pthread_mutexattr_init":                 "pthread.h",
	"pthread_mutexattr_setpolicy_np":         "pthread.h",
	"pthread_mutexattr_setprioceiling":       "pthread.h",
	"pthread_mutexattr_setprotocol":          "pthread.h",
	"pthread_mutexattr_setpshared":           "pthread.h",
	"pthread_mutexattr_settype":              "pthread.h",
	"pthread_once":                           "pthread.h",
	"pthread_rwlock_destroy":                 "pthread.h",
	"pthread_rwlock_init":                    "pthread.h",
	"pthread_rwlock_rdlock":                  "pthread.h",
	"pthread_rwlock_tryrdlock":               "pthread.h",
	"pthread_rwlock_trywrlock":               "pthread.h",
	"pthread_rwlock_unlock":                  "pthread.h",
	"pthread_rwlock_wrlock":                  "pthread.h",
	"pthread_rwlockattr_destroy":             "pthread.h",
	"pthread_rwlockattr_getpshared":          "pthread.h",
	"pthread_rwlockattr_init":                "pthread.h",
	"pthread_rwlockattr_setpshared":          "pthread.h",
	"pthread_self":                           "pthread.h",
	"pthread_setcancelstate":                 "pthread.h",
	"pthread_setcanceltype":                  "pthread.h",
	"pthread_setconcurrency":                 "pthread.h",
	"pthread_setname_np":                     "pthread.h",
	"pthread_setschedparam":                  "pthread.h",
	"pthread_setspecific":                    "pthread.h",
	"pthread_setugid_np":                     "unistd.h",
	"pthread_sigmask":                        "pthread.h",
	"pthread_testcancel":                     "pthread.h",
	"pthread_threadid_np":                    "pthread.h",
	"pthread_yield_np":                       "pthread.h",
	"ptsname":                                "stdlib.h",
	"ptsname_r":                              "stdlib.h",
	"putc":                                   "stdio.h",
	"putc_unlocked":                          "stdio.h",
	"putchar":                                "stdio.h",
	"putchar_unlocked":                       "stdio.h",
	"putenv":                                 "stdlib.h",
	"puts":                                   "stdio.h",
	"putw":                                   "stdio.h",
	"putwc":                                  "wchar.h",
	"putwchar":                               "wchar.h",
	"pwrite":                                 "unistd.h",
	"pwritev":                                "sys/uio.h",
	"qsort":                                  "stdlib.h",
	"qsort_b":                                "stdlib.h",
	"qsort_r":                                "stdlib.h",
	"quick_exit":                             "stdlib.h",
	"radixsort":                              "stdlib.h",
	"raise":                                  "signal.h",
	"rand":                                   "stdlib.h",
	"rand_r":                                 "stdlib.h",
	"random":                                 "stdlib.h",
	"rcmd":                                   "unistd.h",
	"rcmd_af":                                "unistd.h",
	"read":                                   "unistd.h",
	"readdir":                                "dirent.h",
	"readdir_r":                              "dirent.h",
	"readlink":                               "unistd.h",
	"readv":                                  "sys/uio.h",
	"realloc":                                "stdlib.h",
	"reallocarray":                           "stdlib.h",
	"realpath":                               "stdlib.h",
	"reboot":                                 "unistd.h",
	"recv":                                   "sys/socket.h",
	"recvfrom":                               "sys/socket.h",
	"recvmmsg":                               "sys/socket.h",
	"recvmsg":                                "sys/socket.h",
	"remainder":                              "math.h",
	"remainderf":                             "math.h",
	"remainderl":                             "math.h",
	"remove":                                 "stdio.h",
	"remquo":                                 "math.h",
	"remquof":                                "math.h",
	"remquol":                                "math.h",
	"rename":                                 "stdio.h",
	"revoke":                                 "unistd.h",
	"rewind":                                 "stdio.h",
	"rewinddir":                              "dirent.h",
	"rindex":                                 "string.h",
	"rint":                                   "math.h",
	"rintf":                                  "math.h",
	"rintl":                                  "math.h",
	"rinttol":                                "math.h",
	"rmdir":                                  "unistd.h",
	"round":                                  "math.h",
	"roundf":                                 "math.h",
	"roundl":                                 "math.h",
	"roundtol":                               "math.h",
	"rpmatch":                                "stdlib.h",
	"rresvport":                              "unistd.h",
	"rresvport_af":                           "unistd.h",
	"ruserok":                                "unistd.h",
	"sbrk":                                   "unistd.h",
	"scalb":                                  "math.h",
	"scalbln":                                "math.h",
	"scalblnf":                               "math.h",
	"scalblnl":                               "math.h",
	"scalbn":                                 "math.h",
	"scalbnf":                                "math.h",
	"scalbnl":                                "math.h",
	"scandir":                                "dirent.h",
	"scandir_b":                              "dirent.h",
	"scandirat":                              "dirent.h",
	"scandirat_b":                            "dirent.h",
	"scanf":                                  "stdio.h",
	"searchfs":                               "unistd.h",
	"secure_getenv":                          "stdlib.h",
	"seed48":                                 "stdlib.h",
	"seekdir":                                "dirent.h",
	"select":                                 "sys/select.h",
	"send":                                   "sys/socket.h",
	"sendfile":                               "sys/socket.h",
	"sendmmsg":                               "sys/socket.h",
	"sendmsg":                                "sys/socket.h",
	"sendto":                                 "sys/socket.h",
	"setattrlist":                            "fcntl.h",
	"setattrlistat":                          "fcntl.h",
	"setbuf":                                 "stdio.h",
	"setbuffer":                              "stdio.h",
	"setdomainname":                          "unistd.h",
	"setegid":                                "unistd.h",
	"setenv":                                 "stdlib.h",
	"seteuid":                                "unistd.h",
	"setgid":                                 "unistd.h",
	"setgroups":                              "unistd.h",
	"sethostid":                              "unistd.h",
	"sethostname":                            "unistd.h",
	"setiopolicy_np":                         "sys/resource.h",
	"setitimer":                              "sys/time.h",
	"setjmp":                                 "setjmp.h",
	"setkey":                                 "stdlib.h",
	"setlinebuf":                             "stdio.h",
	"setlocale":                              "locale.h",
	"setlogin":                               "unistd.h",
	"setlogmask":                             "syslog.h",
	"setmode":                                "unistd.h",
	"setpgid":                                "unistd.h",
	"setpgrp":                                "unistd.h",
	"setpriority":                            "sys/resource.h",
	"setprogname":                            "stdlib.h",
	"setregid":                               "unistd.h",
	"setreuid":                               "unistd.h",
	"setrgid":                                "unistd.h",
	"setrlimit":                              "sys/resource.h",
	"setruid":                                "unistd.h",
	"setsgroups_np":                          "unistd.h",
	"setsid":                                 "unistd.h",
	"setsockopt":                             "sys/socket.h",
	"setstate":                               "stdlib.h",
	"settimeofday":                           "sys/time.h",
	"setuid":                                 "unistd.h",
	"setusershell":                           "unistd.h",
	"setvbuf":                                "stdio.h",
	"setwgroups_np":                          "unistd.h",
	"shm_open":                               "sys/mman.h",
	"shm_unlink":                             "sys/mman.h",
	"shutdown":                               "sys/socket.h",
	"sigaction":                              "signal.h",
	"sigaddset":                              "signal.h",
	"sigaltstack":                            "signal.h",
	"sigblock":                               "signal.h",
	"sigdelset":                              "signal.h",
	"sigemptyset":                            "signal.h",
	"sigfillset":                             "signal.h",
	"sighold":                                "signal.h",
	"sigignore":                              "signal.h",
	"siginterrupt":                           "signal.h",
	"sigismember":                            "signal.h",
	"siglongjmp":                             "setjmp.h",
	"signal":                                 "signal.h",
	"signbit":                                "math.h",
	"significand":                            "math.h",
	"sigpause":                               "signal.h",
	"sigpending":                             "signal.h",
	"sigprocmask":                            "signal.h",
	"sigrelse":                               "signal.h",
	"sigset":                                 "signal.h",
	"sigsetjmp":                              "setjmp.h",
	"sigsetmask":                             "signal.h",
	"sigsuspend":                             "signal.h",
	"sigtimedwait":                           "signal.h",
	"sigvec":                                 "signal.h",
	"sigwait":                                "signal.h",
	"sigwaitinfo":                            "signal.h",
	"sin":                                    "math.h",
	"sinf":                                   "math.h",
	"sinh":                                   "math.h",
	"sinhf":                                  "math.h",
	"sinhl":                                  "math.h",
	"sinl":                                   "math.h",
	"sleep":                                  "unistd.h",
	"snprintf":                               "stdio.h",
	"sockatmark":                             "sys/socket.h",
	"socket":                                 "sys/socket.h",
	"socketpair":                             "sys/socket.h",
	"sprintf":                                "stdio.h",
	"sqrt":                                   "math.h",
	"sqrtf":                                  "math.h",
	"sqrtl":                                  "math.h",
	"sradixsort":                             "stdlib.h",
	"srand":                                  "stdlib.h",
	"srand48":                                "stdlib.h",
	"sranddev":                               "stdlib.h",
	"srandom":                                "stdlib.h",
	"srandomdev":                             "stdlib.h",
	"sscanf":                                 "stdio.h",
	"stat":                                   "sys/stat.h",
	"stat64":                                 "sys/stat.h",
	"static_assert":                          "assert.h",
	"statx":                                  "sys/stat.h",
	"statx64_np":                             "sys/stat.h",
	"statx_np":                               "sys/stat.h",
	"stpcpy":                                 "string.h",
	"stpncpy":                                "string.h",
	"strcasecmp":                             "string.h",
	"strcasestr":                             "string.h",
	"strcat":                                 "string.h",
	"strchr":                                 "string.h",
	"strchrnul":                              "string.h",
	"strcmp":                                 "string.h",
	"strcoll":                                "string.h",
	"strcpy":                                 "string.h",
	"strcspn":                                "string.h",
	"strdup":                                 "string.h",
	"strerror":                               "errno.h",
	"strerror_r":                             "errno.h",
	"strftime":                               "time.h",
	"strlcat":                                "string.h",
	"strlcpy":                                "string.h",
	"strlen":                                 "string.h",
	"strmode":                                "string.h",
	"strncasecmp":                            "string.h",
	"strncat":                                "string.h",
	"strncmp":                                "string.h",
	"strncpy":                                "string.h",
	"strndup":                                "string.h",
	"strnlen":                                "string.h",
	"strnstr":                                "string.h",
	"strpbrk":                                "string.h",
	"strptime":                               "time.h",
	"strrchr":                                "string.h",
	"strsep":                                 "string.h",
	"strsignal":                              "string.h",
	"strsignal_r":                            "string.h",
	"strspn":                                 "string.h",
	"strstr":                                 "string.h",
	"strtod":                                 "stdlib.h",
	"strtof":                                 "stdlib.h",
	"strtofflags":                            "unistd.h",
	"strtoimax":                              "stdlib.h",
	"strtok":                                 "string.h",
	"strtok_r":                               "string.h",
	"strtol":                                 "stdlib.h",
	"strtold":                                "stdlib.h",
	"strtoll":                                "stdlib.h",
	"strtonum":                               "stdlib.h",
	"strtoq":                                 "stdlib.h",
	"strtoul":                                "stdlib.h",
	"strtoull":                               "stdlib.h",
	"strtoumax":                              "stdlib.h",
	"strtouq":                                "stdlib.h",
	"strxfrm":                                "string.h",
	"swab":                                   "string.h",
	"swapon":                                 "unistd.h",
	"swprintf":                               "wchar.h",
	"swscanf":                                "wchar.h",
	"symlink":                                "unistd.h",
	"sync":                                   "unistd.h",
	"sync_volume_np":                         "unistd.h",
	"syscall":                                "unistd.h",
	"sysconf":                                "unistd.h",
	"sysctl":                                 "sys/sysctl.h",
	"sysctlbyname":                           "sys/sysctl.h",
	"sysctlnametomib":                        "sys/sysctl.h",
	"syslog":                                 "syslog.h",
	"system":                                 "stdlib.h",
	"tan":                                    "math.h",
	"tanf":                                   "math.h",
	"tanh":                                   "math.h",
	"tanhf":                                  "math.h",
	"tanhl":                                  "math.h",
	"tanl":                                   "math.h",
	"tcgetpgrp":                              "unistd.h",
	"tcsetpgrp":                              "unistd.h",
	"telldir":                                "dirent.h",
	"tempnam":                                "stdio.h",
	"tgamma":                                 "math.h",
	"tgammaf":                                "math.h",
	"tgammal":                                "math.h",
	"time":                                   "time.h",
	"time2posix":                             "time.h",
	"timegm":                                 "time.h",
	"timelocal":                              "time.h",
	"timeradd":                               "sys/time.h",
	"timerclear":                             "sys/time.h",
	"timercmp":                               "sys/time.h",
	"timerisset":                             "sys/time.h",
	"timersub":                               "sys/time.h",
	"timespec_get":                           "time.h",
	"timespec_getres":                        "time.h",
	"timevalcmp":                             "sys/time.h",
	"timezone":                               "time.h",
	"timingsafe_bcmp":                        "string.h",
	"timingsafe_memcmp":                      "string.h",
	"tmpfile":                                "stdio.h",
	"tmpnam":                                 "stdio.h",
	"toascii":                                "ctype.h",
	"tolower":                                "ctype.h",
	"toupper":                                "ctype.h",
	"trunc":                                  "math.h",
	"truncate":                               "unistd.h",
	"truncf":                                 "math.h",
	"truncl":                                 "math.h",
	"ttyname":                                "unistd.h",
	"ttyname_r":                              "unistd.h",
	"ttyslot":                                "unistd.h",
	"tzset":                                  "time.h",
	"tzsetwall":                              "time.h",
	"ualarm":                                 "unistd.h",
	"umask":                                  "sys/stat.h",
	"umaskx_np":                              "sys/stat.h",
	"undelete":                               "unistd.h",
	"ungetc":                                 "stdio.h",
	"ungetwc":                                "wchar.h",
	"unlink":                                 "unistd.h",
	"unlinkat":                               "unistd.h",
	"unlockpt":                               "stdlib.h",
	"unsetenv":                               "stdlib.h",
	"unwhiteout":                             "unistd.h",
	"uselocale":                              "locale.h",
	"usleep":                                 "unistd.h",
	"utimensat":                              "fcntl.h",
	"utimes":                                 "fcntl.h",
	"va_arg":                                 "stdarg.h",
	"va_copy":                                "stdarg.h",
	"va_end":                                 "stdarg.h",
	"va_list":                                "stdarg.h",
	"va_start":                               "stdarg.h",
	"valloc":                                 "stdlib.h",
	"vasprintf":                              "stdio.h",
	"vdprintf":                               "stdio.h",
	"vfork":                                  "unistd.h",
	"vfprintf":                               "stdio.h",
	"vfscanf":                                "stdio.h",
	"vfwprintf":                              "wchar.h",
	"vfwscanf":                               "wchar.h",
	"vprintf":                                "stdio.h",
	"vscanf":                                 "stdio.h",
	"vsnprintf":                              "stdio.h",
	"vsprintf":                               "stdio.h",
	"vsscanf":                                "stdio.h",
	"vswprintf":                              "wchar.h",
	"vswscanf":                               "wchar.h",
	"vsyslog":                                "syslog.h",
	"vwprintf":                               "wchar.h",
	"vwscanf":                                "wchar.h",
	"wait":                                   "sys/wait.h",
	"wait3":                                  "sys/wait.h",
	"wait4":                                  "sys/wait.h",
	"waitid":                                 "sys/wait.h",
	"waitpid":                                "sys/wait.h",
	"wcpcpy":                                 "wchar.h",
	"wcpncpy":                                "wchar.h",
	"wcrtomb":                                "wchar.h",
	"wcscasecmp":                             "wchar.h",
	"wcscat":                                 "wchar.h",
	"wcschr":                                 "wchar.h",
	"wcscmp":                                 "wchar.h",
	"wcscoll":                                "wchar.h",
	"wcscpy":                                 "wchar.h",
	"wcscspn":                                "wchar.h",
	"wcsdup":                                 "wchar.h",
	"wcsftime":                               "wchar.h",
	"wcslcat":                                "wchar.h",
	"wcslcpy":                                "wchar.h",
	"wcslen":                                 "wchar.h",
	"wcsncasecmp":                            "wchar.h",
	"wcsncat":                                "wchar.h",
	"wcsncmp":                                "wchar.h",
	"wcsncpy":                                "wchar.h",
	"wcsnlen":                                "wchar.h",
	"wcsnrtombs":                             "wchar.h",
	"wcspbrk":                                "wchar.h",
	"wcsrchr":                                "wchar.h",
	"wcsrtombs":                              "wchar.h",
	"wcsspn":                                 "wchar.h",
	"wcsstr":                                 "wchar.h",
	"wcstod":                                 "wchar.h",
	"wcstof":                                 "wchar.h",
	"wcstok":                                 "wchar.h",
	"wcstol":                                 "wchar.h",
	"wcstold":                                "wchar.h",
	"wcstoll":                                "wchar.h",
	"wcstombs":                               "stdlib.h",
	"wcstoul":                                "wchar.h",
	"wcstoull":                               "wchar.h",
	"wcswidth":                               "wchar.h",
	"wcsxfrm":                                "wchar.h",
	"wctob":                                  "wchar.h",
	"wctomb":                                 "stdlib.h",
	"wcwidth":                                "wchar.h",
	"wmemchr":                                "wchar.h",
	"wmemcmp":                                "wchar.h",
	"wmemcpy":                                "wchar.h",
	"wmemmove":                               "wchar.h",
	"wmemset":                                "wchar.h",
	"wprintf":                                "wchar.h",
	"write":                                  "unistd.h",
	"writev":                                 "sys/uio.h",
	"wscanf":                                 "wchar.h",
	"y0":                                     "math.h",
	"y1":                                     "math.h",
	"yield":                                  "pthread.h",
	"yn":                                     "math.h",
}

var nsNames = [...]string{"__builtin_*", "assert.h", "ctype.h", "dirent.h", "dlfcn.h", "errno.h", "fcntl.h", "getopt.h", "locale.h", "math.h", "pthread.h", "setjmp.h", "signal.h", "stdarg.h", "stdio.h", "stdlib.h", "string.h", "sys/mman.h", "sys/resource.h", "sys/select.h", "sys/socket.h", "sys/stat.h", "sys/sysctl.h", "sys/time.h", "sys/uio.h", "sys/wait.h", "syslog.h", "time.h", "unistd.h", "wchar.h"}

var guardNS = map[string]map[string]bool{
	"calloc_sizeof_first": {"alloc": true, "stdlib.h": true},
	"domain":              {"math.h": true},
	"endptr":              {"stdlib.h": true},
	"errno_zero":          {"stdlib.h": true},
	"map_failed":          {"sys/mman.h": true},
	"monotonic":           {"sys/time.h": true, "time.h": true},
	"ptr_ovf_check":       {"stdio.h": true, "stdlib.h": true, "string.h": true, "unistd.h": true},
	"ret_neg":             {"dirent.h": true, "fcntl.h": true, "stdio.h": true, "sys/mman.h": true, "sys/select.h": true, "sys/socket.h": true, "sys/stat.h": true, "sys/uio.h": true, "sys/wait.h": true, "unistd.h": true},
	"stacksz_array":       {"pthread.h": true, "signal.h": true, "unistd.h": true},
	"uchar":               {"ctype.h": true},
	"va_arg_array":        {"stdarg.h": true},
	"va_end":              {"stdarg.h": true},
}

var guardNames = [...]string{"ret_neg", "domain", "uchar", "errno_zero", "endptr", "va_end", "map_failed", "monotonic", "stacksz_array", "ptr_ovf_check", "calloc_sizeof_first", "va_arg_array"}

var guardCol = map[string]string{"ret_neg": "n_ret_neg_check", "domain": "n_domain_guard", "uchar": "n_uchar_cast", "errno_zero": "n_errno_zero", "endptr": "n_endptr", "va_end": "n_va_end", "map_failed": "n_map_failed", "monotonic": "n_monotonic_clock", "stacksz_array": "n_stacksz_array", "ptr_ovf_check": "n_ptr_ovf_check", "calloc_sizeof_first": "n_calloc_transposed", "va_arg_array": "n_va_arg_array"}

var magicOK = map[int64]bool{-1: true, 0: true, 1: true, 2: true, 3: true, 4: true, 6: true, 7: true, 8: true, 10: true, 12: true, 16: true, 24: true, 32: true, 60: true, 64: true, 100: true, 128: true, 255: true, 256: true, 365: true, 512: true, 1000: true, 1024: true, 4096: true, 65535: true}

var lockGeneric = map[string]bool{"lck": true, "lock": true, "locks": true, "mutex": true, "sem": true, "spin": true, "spinlock": true}

var eventWaitFn = map[string]bool{"epoll_pwait": true, "epoll_pwait2": true, "epoll_wait": true, "io_uring_enter": true, "io_uring_peek_cqe": true, "io_uring_sqring_wait": true, "io_uring_submit_and_wait": true, "io_uring_wait_cqe": true, "io_uring_wait_cqes": true, "kevent": true}

var eventCreateFn = map[string]bool{"epoll_create": true, "epoll_create1": true, "io_uring_queue_init": true, "io_uring_queue_init_mem": true, "io_uring_queue_init_params": true, "io_uring_setup": true, "kqueue": true, "kqueue1": true, "kqueuex": true}

var typeSizeTable = map[string]int32{
	"_Bool":              1,
	"bool":               1,
	"char":               1,
	"double":             8,
	"float":              4,
	"gid_t":              4,
	"int":                4,
	"int16_t":            2,
	"int32_t":            4,
	"int64_t":            8,
	"int8_t":             1,
	"intptr_t":           8,
	"long":               8,
	"long double":        16,
	"long int":           8,
	"long long":          8,
	"mode_t":             4,
	"off_t":              8,
	"pid_t":              4,
	"pthread_t":          8,
	"ptrdiff_t":          8,
	"short":              2,
	"short int":          2,
	"signed char":        1,
	"size_t":             8,
	"ssize_t":            8,
	"time_t":             8,
	"uid_t":              4,
	"uint16_t":           2,
	"uint32_t":           4,
	"uint64_t":           8,
	"uint8_t":            1,
	"uintptr_t":          8,
	"unsigned":           4,
	"unsigned char":      1,
	"unsigned int":       4,
	"unsigned long":      8,
	"unsigned long int":  8,
	"unsigned long long": 8,
	"unsigned short":     2,
	"unsigned short int": 2,
}

var typeAlignTable = map[string]int32{"long double": 16}

const ptrSize = 8

var evTimeoutIdx = map[string]int{"epoll_pwait": 3, "epoll_pwait2": 3, "epoll_wait": 3, "kevent": 5}

var evBatchIdx = map[string]int{"epoll_pwait": 2, "epoll_pwait2": 2, "epoll_wait": 2, "kevent": 4}

var libcAllocNames = map[string]bool{"malloc": true, "calloc": true, "realloc": true, "free": true, "aligned_alloc": true, "posix_memalign": true, "memalign": true, "valloc": true, "reallocarray": true, "strdup": true, "strndup": true}

type columnShape struct {
	name string
	kind string
	opt  bool
}

type tableShape struct {
	name string
	note string
	cols []columnShape
}

var graphShape = []tableShape{
	{name: "addr_taken",
		note: "the address of a local taken and handed to something else",
		cols: []columnShape{
			{"id", "int", false},
			{"symbol_id", "int", true},
			{"file_id", "int", false},
			{"name", "text", false},
			{"line", "int", false},
			{"kind", "text", false},
		}},
	{name: "allocsites",
		note: "every allocation site, with the size the compiler saw",
		cols: []columnShape{
			{"id", "int", false},
			{"symbol_id", "int", false},
			{"file_id", "int", false},
			{"fn", "text", false},
			{"size_expr", "text", false},
			{"line", "int", false},
		}},
	{name: "apiuse",
		note: "standard-library entry points reached, and how often",
		cols: []columnShape{
			{"id", "int", false},
			{"symbol_id", "int", false},
			{"file_id", "int", false},
			{"ns", "text", false},
			{"fn", "text", false},
			{"line", "int", false},
		}},
	{name: "attributes",
		note: "the annotation/attribute forms this language has",
		cols: []columnShape{
			{"id", "int", false},
			{"symbol_id", "int", true},
			{"file_id", "int", false},
			{"name", "text", false},
			{"args", "text", true},
			{"line", "int", false},
		}},
	{name: "callsites",
		note: "every distinct line a resolved call was seen on",
		cols: []columnShape{
			{"caller_id", "int", false},
			{"callee_id", "int", false},
			{"line", "int", false},
		}},
	{name: "config_blocks",
		note: "a #if/#ifdef/#else block and the condition on it",
		cols: []columnShape{
			{"id", "int", false},
			{"file_id", "int", false},
			{"directive", "text", false},
			{"expr", "text", false},
			{"line", "int", false},
			{"is_config", "int", false},
		}},
	{name: "declarations",
		note: "a declaration that is not a function: var, const, typedef",
		cols: []columnShape{
			{"id", "int", false},
			{"file_id", "int", false},
			{"name", "text", false},
			{"line", "int", false},
		}},
	{name: "edges",
		note: "the call graph, one row per (caller, callee)",
		cols: []columnShape{
			{"caller_id", "int", false},
			{"callee_id", "int", false},
			{"n_calls", "int", false},
			{"same_file", "int", false},
			{"same_module", "int", false},
			{"is_self", "int", false},
		}},
	{name: "enum_members",
		note: "the cases of an enum, with their values",
		cols: []columnShape{
			{"symbol_id", "int", false},
			{"ordinal", "int", false},
			{"name", "text", false},
			{"value", "text", true},
			{"n_fields", "int", false},
		}},
	{name: "eventops",
		note: "event-loop registration: what was registered and where",
		cols: []columnShape{
			{"id", "int", false},
			{"symbol_id", "int", false},
			{"file_id", "int", false},
			{"family", "text", false},
			{"fn", "text", false},
			{"args", "text", false},
			{"line", "int", false},
		}},
	{name: "fields",
		note: "declared properties and constants",
		cols: []columnShape{
			{"symbol_id", "int", false},
			{"ordinal", "int", false},
			{"name", "text", false},
			{"type", "text", false},
			{"visibility", "text", false},
			{"line", "int", false},
			{"is_static", "int", false},
			{"is_const", "int", false},
			{"is_mutable", "int", false},
			{"is_nullable", "int", false},
			{"is_collection", "int", false},
			{"is_untyped", "int", false},
			{"has_default", "int", false},
			{"type_depth", "int", false},
		}},
	{name: "files",
		note: "one row per discovered source file",
		cols: []columnShape{
			{"id", "int", false},
			{"path", "text", false},
			{"dir", "text", false},
			{"basename", "text", false},
			{"ext", "text", false},
			{"lang", "text", false},
			{"module_id", "int", true},
			{"bytes", "int", false},
			{"lines", "int", false},
			{"sloc", "int", false},
			{"blank_lines", "int", false},
			{"comment_lines", "int", false},
			{"doc_lines", "int", false},
			{"max_line_len", "int", false},
			{"sha1", "text", false},
			{"parsed", "int", false},
			{"is_test", "int", false},
			{"is_generated", "int", false},
			{"is_vendored", "int", false},
			{"n_parse_errors", "int", false},
			{"n_missing_nodes", "int", false},
			{"parse_ms", "real", false},
			{"n_symbols", "int", false},
			{"n_functions", "int", false},
			{"n_types", "int", false},
			{"n_imports", "int", false},
			{"total_cyclo", "int", false},
			{"max_cyclo", "int", false},
			{"total_risk", "int", false},
		}},
	{name: "globals",
		note: "a mutable global, and the threads that can see it",
		cols: []columnShape{
			{"id", "int", false},
			{"file_id", "int", false},
			{"module_id", "int", true},
			{"name", "text", false},
			{"type", "text", false},
			{"line", "int", false},
			{"is_static", "int", false},
			{"is_const", "int", false},
			{"is_volatile", "int", false},
			{"is_atomic", "int", false},
			{"is_array", "int", false},
			{"ptr_depth", "int", false},
			{"has_init", "int", false},
		}},
	{name: "hazards",
		note: "dangerous call families, one row per (symbol, pattern)",
		cols: []columnShape{
			{"symbol_id", "int", false},
			{"pattern", "text", false},
			{"category", "text", false},
			{"n", "int", false},
			{"first_line", "int", false},
		}},
	{name: "imports",
		note: "use statements and includes",
		cols: []columnShape{
			{"id", "int", false},
			{"file_id", "int", false},
			{"target", "text", false},
			{"target_id", "int", true},
			{"alias", "text", true},
			{"kind", "text", false},
			{"line", "int", false},
			{"is_external", "int", false},
			{"is_relative", "int", false},
			{"is_wildcard", "int", false},
			{"is_type_only", "int", false},
			{"is_dynamic", "int", false},
			{"n_names", "int", false},
		}},
	{name: "include_cycles",
		note: "a cycle in the include graph",
		cols: []columnShape{
			{"id", "int", false},
			{"a_path", "text", false},
			{"b_path", "text", false},
			{"length", "int", false},
			{"members", "text", false},
		}},
	{name: "layout",
		note: "a struct or union layout: offsets, sizes, padding",
		cols: []columnShape{
			{"symbol_id", "int", false},
			{"ordinal", "int", false},
			{"byte_off", "int", false},
			{"byte_size", "int", false},
			{"pad_before", "int", false},
			{"exact", "int", false},
			{"ptr_depth", "int", false},
			{"array_len", "int", false},
			{"is_fnptr", "int", false},
			{"depth", "int", false},
			{"in_union", "int", false},
		}},
	{name: "literals",
		note: "string and numeric literals inside a body",
		cols: []columnShape{
			{"id", "int", false},
			{"symbol_id", "int", true},
			{"file_id", "int", false},
			{"kind", "text", false},
			{"value", "text", false},
			{"line", "int", false},
			{"is_magic", "int", false},
		}},
	{name: "locals",
		note: "declared but never written: there is no local-variable inventory",
		cols: []columnShape{
			{"symbol_id", "int", false},
			{"ordinal", "int", false},
			{"name", "text", false},
			{"type", "text", false},
			{"line", "int", false},
			{"is_const", "int", false},
			{"is_mutable", "int", false},
			{"is_untyped", "int", false},
			{"has_init", "int", false},
			{"in_loop", "int", false},
			{"scope_depth", "int", false},
		}},
	{name: "locks",
		note: "a lock, and whether it is taken in a consistent order",
		cols: []columnShape{
			{"id", "int", false},
			{"symbol_id", "int", false},
			{"file_id", "int", false},
			{"name", "text", false},
			{"line", "int", false},
		}},
	{name: "macros",
		note: "a macro invocation, and which definition it expands to",
		cols: []columnShape{
			{"symbol_id", "int", false},
			{"is_functionlike", "int", false},
			{"n_params", "int", false},
			{"body", "text", true},
			{"body_len", "int", false},
			{"is_multiline", "int", false},
			{"n_uses", "int", false},
		}},
	{name: "makefile_rules",
		note: "a make target, its prerequisites, and its recipe",
		cols: []columnShape{
			{"id", "int", false},
			{"path", "text", false},
			{"rule", "text", false},
			{"line", "int", false},
			{"n_objs", "int", false},
			{"n_srcs", "int", false},
			{"uses_ar", "int", false},
		}},
	{name: "markers",
		note: "TODO/FIXME/HACK and friends",
		cols: []columnShape{
			{"id", "int", false},
			{"file_id", "int", false},
			{"symbol_id", "int", true},
			{"kind", "text", false},
			{"line", "int", false},
			{"text", "text", true},
		}},
	{name: "memops",
		note: "raw memory operations: allocation, copy, free, and overlap",
		cols: []columnShape{
			{"id", "int", false},
			{"symbol_id", "int", false},
			{"file_id", "int", false},
			{"fn", "text", false},
			{"dst", "text", false},
			{"src", "text", false},
			{"size_arg", "text", false},
			{"size_buf", "text", false},
			{"dst_tail", "text", false},
			{"line", "int", false},
		}},
	{name: "meta",
		note: "run facts: what was parsed, with what, and what was skipped",
		cols: []columnShape{
			{"key", "text", false},
			{"value", "text", false},
		}},
	{name: "modules",
		note: "grouping key for files: the first two directory levels",
		cols: []columnShape{
			{"id", "int", false},
			{"name", "text", false},
			{"kind", "text", false},
			{"n_files", "int", false},
			{"n_symbols", "int", false},
			{"n_public", "int", false},
			{"sloc", "int", false},
			{"fan_in", "int", false},
			{"fan_out", "int", false},
			{"instability", "real", false},
		}},
	{name: "params",
		note: "parameter list, keyed by symbol",
		cols: []columnShape{
			{"symbol_id", "int", false},
			{"pos", "int", false},
			{"name", "text", true},
			{"type", "text", false},
			{"default_value", "text", true},
			{"is_optional", "int", false},
			{"is_variadic", "int", false},
			{"is_ref", "int", false},
			{"is_mutable", "int", false},
			{"is_nullable", "int", false},
			{"is_generic", "int", false},
			{"is_untyped", "int", false},
			{"type_depth", "int", false},
		}},
	{name: "reach",
		note: "a symbol reachable from an entry point, and at what depth",
		cols: []columnShape{
			{"symbol_id", "int", false},
			{"n_transitive", "int", false},
			{"n_transitive_out", "int", false},
		}},
	{name: "secret_candidates",
		note: "credential-shaped string literals",
		cols: []columnShape{
			{"id", "int", false},
			{"symbol_id", "int", true},
			{"file_id", "int", false},
			{"value", "text", false},
			{"line", "int", false},
		}},
	{name: "struct_size",
		note: "the size and alignment of a declared struct",
		cols: []columnShape{
			{"symbol_id", "int", false},
			{"total_size", "int", false},
			{"tail_pad", "int", false},
			{"total_pad", "int", false},
			{"max_align", "int", false},
			{"exact", "int", false},
			{"n_lines_64", "int", false},
		}},
	{name: "sym_fts",
		note: "a contentless full-text index over (name, qual_name, signature): every column reads back empty, and the four shadow b-tree tables beside it are not reproduced at all",
		cols: []columnShape{
			{"name", "any", true},
			{"qual_name", "any", true},
			{"signature", "any", true},
		}},
	{name: "symbols",
		note: "the hub: every function, method, closure, hook and type",
		cols: []columnShape{
			{"id", "int", false},
			{"file_id", "int", false},
			{"module_id", "int", true},
			{"parent_id", "int", true},
			{"name", "text", false},
			{"qual_name", "text", false},
			{"kind", "text", false},
			{"line_start", "int", false},
			{"byte_start", "int", false},
			{"byte_end", "int", false},
			{"signature", "text", true},
			{"return_type", "text", true},
			{"n_params", "int", false},
			{"n_optional_params", "int", false},
			{"n_generic_params", "int", false},
			{"n_overloads", "int", false},
			{"arity_rank", "int", false},
			{"is_public", "int", false},
			{"is_static", "int", false},
			{"is_async", "int", false},
			{"is_generator", "int", false},
			{"is_abstract", "int", false},
			{"is_override", "int", false},
			{"is_exported", "int", false},
			{"is_test", "int", false},
			{"is_deprecated", "int", false},
			{"is_entrypoint", "int", false},
			{"is_generated", "int", false},
			{"sloc", "int", false},
			{"n_comment_lines", "int", false},
			{"has_doc", "int", false},
			{"cyclomatic", "int", false},
			{"cognitive", "int", false},
			{"max_nesting", "int", false},
			{"n_tokens", "int", false},
			{"n_operators", "int", false},
			{"n_operands", "int", false},
			{"n_distinct_operators", "int", false},
			{"n_distinct_operands", "int", false},
			{"n_loops", "int", false},
			{"n_branches", "int", false},
			{"n_returns", "int", false},
			{"n_switch", "int", false},
			{"n_cases", "int", false},
			{"n_try", "int", false},
			{"n_catch", "int", false},
			{"n_catch_broad", "int", false},
			{"n_catch_empty", "int", false},
			{"n_finally", "int", false},
			{"n_throw", "int", false},
			{"n_labels", "int", false},
			{"n_gotos", "int", false},
			{"max_loop_depth", "int", false},
			{"call_in_loop", "int", false},
			{"alloc_in_loop", "int", false},
			{"io_in_loop", "int", false},
			{"await_in_loop", "int", false},
			{"lock_in_loop", "int", false},
			{"concat_in_loop", "int", false},
			{"regex_in_loop", "int", false},
			{"query_in_loop", "int", false},
			{"branch_in_loop", "int", false},
			{"n_locals", "int", false},
			{"n_cmp", "int", false},
			{"n_shift", "int", false},
			{"n_arith", "int", false},
			{"n_regex_lit", "int", false},
			{"n_float_lit", "int", false},
			{"n_magic", "int", false},
			{"n_null_check", "int", false},
			{"n_lambda", "int", false},
			{"n_closure_capture", "int", false},
			{"n_calls", "int", false},
			{"n_dynamic_calls", "int", false},
			{"n_unresolved_calls", "int", false},
			{"fan_in", "int", false},
			{"fan_out", "int", false},
			{"n_callsites", "int", false},
			{"is_recursive", "int", false},
			{"risk_score", "int", false},
			{"n_memory", "int", false},
			{"n_alloc", "int", false},
			{"n_io", "int", false},
			{"n_stdio", "int", false},
			{"n_exec", "int", false},
			{"n_libm", "int", false},
			{"n_integer", "int", false},
			{"n_concurrency", "int", false},
			{"n_reentrancy", "int", false},
			{"is_inline", "int", false},
			{"is_variadic", "int", false},
			{"n_ptr_locals", "int", false},
			{"n_deref", "int", false},
			{"n_cast", "int", false},
			{"n_sizeof", "int", false},
			{"n_intrinsic", "int", false},
			{"n_atomic", "int", false},
			{"n_restrict", "int", false},
			{"n_likely", "int", false},
			{"n_builtin", "int", false},
			{"switch_in_loop", "int", false},
			{"libm_in_loop", "int", false},
			{"div_in_loop", "int", false},
			{"strlen_in_loop", "int", false},
			{"ret_null", "int", false},
			{"ret_neg", "int", false},
			{"ret_zero", "int", false},
			{"ret_val", "int", false},
			{"ret_void", "int", false},
			{"n_fnptr_calls", "int", false},
			{"n_macro_calls", "int", false},
			{"n_external_calls", "int", false},
			{"n_free", "int", false},
			{"n_const_cast", "int", false},
			{"n_toctou", "int", false},
			{"n_lock_acquire", "int", false},
			{"n_lock_release", "int", false},
			{"n_narrow_cast", "int", false},
			{"n_sign_cmp", "int", false},
			{"n_variadic_fmt", "int", false},
			{"n_memcpy", "int", false},
			{"n_allocsite", "int", false},
			{"n_global_write", "int", false},
			{"n_errno", "int", false},
			{"n_weak_random", "int", false},
			{"n_shift_var", "int", false},
			{"n_realloc_self", "int", false},
			{"n_vla", "int", false},
			{"n_getenv", "int", false},
			{"n_assert_side", "int", false},
			{"n_free_then_use", "int", false},
			{"n_epoll", "int", false},
			{"n_uring", "int", false},
			{"n_kqueue", "int", false},
			{"n_event_wait", "int", false},
			{"n_et_reg", "int", false},
			{"n_oneshot_reg", "int", false},
			{"n_write_ready", "int", false},
			{"n_err_flag", "int", false},
			{"n_rearm", "int", false},
			{"n_dereg", "int", false},
			{"n_eagain", "int", false},
			{"n_eintr", "int", false},
			{"n_uring_res", "int", false},
			{"n_uring_ring", "int", false},
			{"n_uring_barrier", "int", false},
			{"n_uring_sqpoll", "int", false},
			{"n_nonblock_set", "int", false},
			{"n_event_create", "int", false},
			{"n_event_destroy", "int", false},
			{"n_uring_sqe", "int", false},
			{"n_uring_seen", "int", false},
			{"n_uring_udata", "int", false},
			{"n_uring_link", "int", false},
			{"n_uring_stream_ops", "int", false},
			{"n_uring_teardown", "int", false},
			{"n_ev_timeout_indefinite", "int", false},
			{"n_ev_timeout_zero", "int", false},
			{"n_ev_batch_one", "int", false},
			{"n_kq_timer", "int", false},
			{"n_kq_timer_zero_data", "int", false},
			{"n_kq_receipt", "int", false},
			{"n_ret_neg_check", "int", false},
			{"n_domain_guard", "int", false},
			{"n_uchar_cast", "int", false},
			{"n_errno_zero", "int", false},
			{"n_endptr", "int", false},
			{"n_va_end", "int", false},
			{"n_map_failed", "int", false},
			{"n_monotonic_clock", "int", false},
			{"n_stacksz_array", "int", false},
			{"n_ptr_ovf_check", "int", false},
			{"n_calloc_transposed", "int", false},
			{"n_va_arg_array", "int", false},
		}},
	{name: "unresolved_calls",
		note: "a call we saw but could not point at a definition",
		cols: []columnShape{
			{"caller_id", "int", false},
			{"name", "text", false},
			{"n", "int", false},
			{"first_line", "int", false},
		}},
}

func schemaNative() string {
	var b strings.Builder
	for _, t := range graphShape {
		b.WriteString("table ")
		b.WriteString(t.name)
		b.WriteString(" -- ")
		b.WriteString(t.note)
		b.WriteByte('\n')
		line := "  "
		for i, c := range t.cols {
			piece := c.name
			if c.opt {
				piece += "?"
			}
			piece += " " + c.kind
			if i > 0 && len(line)+1+len(piece) > schemaLine {
				b.WriteString(line)
				b.WriteByte('\n')
				line = "  "
			}
			if len(line) > 2 {
				line += " "
			}
			line += piece
		}
		if len(line) > 2 {
			b.WriteString(line)
			b.WriteByte('\n')
		}
		b.WriteByte('\n')
	}
	b.WriteString("no query layer: there is no expression language here. Every\n")
	b.WriteString("question is a loop over a slice, a lookup of a column, and a reduce.\n")
	return b.String()
}

const schemaLine = 78

var skipDirs = map[string]bool{
	".git": true, ".hg": true, ".svn": true, ".jj": true, ".idea": true,
	".vscode": true, ".vs": true, ".claude": true,
	"node_modules": true, "bower_components": true, "vendor": true,
	"third_party": true, "thirdparty": true, "external": true,
	"externals": true, "deps": true, "Godeps": true, "_vendor": true,
	"__pycache__": true, ".mypy_cache": true, ".pytest_cache": true,
	".ruff_cache": true, ".tox": true, ".venv": true, "venv": true,
	"env": true, ".env": true, "virtualenv": true,
	"build": true, "_build": true, "dist": true, "out": true, "target": true,
	"bin": true, "obj": true, ".gradle": true,
	".next": true, ".nuxt": true, ".svelte-kit": true, ".parcel-cache": true,
	".turbo": true, ".cache": true,
	"coverage": true, "htmlcov": true, ".nyc_output": true,
	"site-packages": true,

	"CMakeFiles": true, ".deps": true, ".libs": true, "autom4te.cache": true,
}

var cExts = map[string]bool{".c": true, ".h": true, ".inc": true, ".cc": true}

const (
	maxFileBytes = 4 * 1024 * 1024
	maxLineBytes = 1024 * 1024
)

var generatedMarkers = []string{
	"@generated", "DO NOT EDIT", "Code generated by", "AUTO-GENERATED",
	"autogenerated", "This file was automatically generated",
	"Generated by the protocol buffer compiler", "@flow-generated",
}

type discoverResult struct {
	files []File
	parse []int32
	mods  []Module
}

func moduleOf(rel string) string {
	parts := strings.Split(filepath.ToSlash(rel), "/")
	if len(parts) <= 1 {
		return "(root)"
	}
	head := parts[:len(parts)-1]
	switch head[0] {
	case "src", "lib", "source", "internal", "pkg", "app":
		if len(head) > 3 {
			head = head[:3]
		}
	default:
		if len(head) > 2 {
			head = head[:2]
		}
	}
	return strings.Join(head, "/")
}

func moduleKind(name string) string {
	switch {
	case matchPathSeg(name, "test", "tests", "test-d", "spec", "specs",
		"__tests__", "__snapshots__", "testing", "e2e", "integration-tests",
		"integration_tests", "testdata", "test_data", "test-data", "fixture",
		"fixtures"):
		return "test"
	case matchVendorPath(name):
		return "vendor"
	case matchDirWord(name, "example", "examples", "sample", "samples",
		"demo", "demos"):
		return "example"
	case matchDirWord(name, "tool", "tools", "script", "scripts", "cmd", "bin"):
		return "tool"
	}
	return "source"
}

func matchPathSeg(p string, words ...string) bool {
	lower := strings.ToLower(p)
	for seg := range strings.SplitSeq(lower, "/") {
		if slices.Contains(words, seg) {
			return true
		}
	}
	return false
}

func matchVendorPath(p string) bool {
	lower := strings.ToLower(p)
	for seg := range strings.SplitSeq(lower, "/") {
		switch seg {
		case "vendor", "third_party", "thirdparty", "external",
			"node_modules", "deps":
			return true
		}
	}
	return false
}

func matchDirWord(p string, words ...string) bool {
	lower := strings.ToLower(p)
	for seg := range strings.SplitSeq(lower, "/") {
		if slices.Contains(words, seg) {
			return true
		}
	}
	return false
}

func isGeneratedName(name string) bool {
	lower := strings.ToLower(name)
	for _, pat := range []string{".min.", ".bundle.", ".gen.", ".generated.",
		".pb.", ".g.", "_pb2", ".designer."} {
		if strings.Contains(lower, pat) {
			return true
		}
	}
	if strings.HasSuffix(lower, ".g.dart") {
		return true
	}
	if strings.HasPrefix(lower, "zz_generated") {
		return true
	}
	return false
}

func isGenerated(name, head string) bool {
	if isGeneratedName(name) {
		return true
	}
	for _, m := range generatedMarkers {
		if strings.Contains(head, m) {
			return true
		}
	}
	return false
}

func cTestName(fn string) bool {
	if strings.HasPrefix(fn, "test_") || strings.HasPrefix(fn, "t_") {
		return true
	}
	return strings.HasSuffix(fn, "_test.c") || strings.HasSuffix(fn, "_test.h")
}

var cmtPrefixes = [...]string{"//", "#", "/*", "*", "*/", `"""`, "'''", "--", ";;", "%"}

type walker struct {
	g        *Graph
	root     string
	modID    map[string]int32
	realRoot string
	res      discoverResult
	opts     runOpts
	cands    []fileCand
}

type runOpts struct {
	includeTests     bool
	includeGenerated bool
	includeVendored  bool
	quiet            bool
}

func discover(root string, g *Graph, opts runOpts) []int32 {
	w := &walker{g: g, root: root, modID: map[string]int32{},
		realRoot: realpath(root), opts: opts}
	w.walk(root, false)
	w.admit()
	for i := range w.res.files {
		f := &w.res.files[i]
		if f.Parsed == 1 {
			w.res.parse = append(w.res.parse, f.ID)
		}
	}
	g.Modules = w.res.mods
	g.Files = w.res.files
	return w.res.parse
}

func (w *walker) walk(dir string, viaLink bool) {
	ents, err := os.ReadDir(dir)
	if err != nil {
		w.g.WalkErrors++
		return
	}
	type ent struct {
		name  string
		isDir bool
		link  bool
	}
	files := make([]ent, 0, len(ents))
	dirs := make([]ent, 0, len(ents))
	for _, e := range ents {
		name := e.Name()
		switch {
		case e.Type()&fs.ModeSymlink != 0:
			st, err := os.Stat(filepath.Join(dir, name))
			if err != nil {
				continue
			}
			if st.IsDir() {

				dirs = append(dirs, ent{name, true, true})
				continue
			}
			files = append(files, ent{name, false, true})
		case e.IsDir():
			dirs = append(dirs, ent{name, true, false})
		default:
			files = append(files, ent{name, false, false})
		}
	}
	sort.Slice(files, func(i, j int) bool { return files[i].name < files[j].name })
	sort.Slice(dirs, func(i, j int) bool { return dirs[i].name < dirs[j].name })
	for _, e := range files {
		w.consider(filepath.Join(dir, e.name), e.name, viaLink || e.link)
	}
	for _, e := range dirs {

		if e.link || skipDirs[e.name] || strings.HasPrefix(e.name, ".") {
			continue
		}
		w.walk(filepath.Join(dir, e.name), viaLink)
	}
}

var source [][]byte

// consider runs the checks that decide whether a file is read at all. They
// are serial and in walk order, because that order is the file id. The read
// itself, and everything computed from the bytes, is left to measure().
func (w *walker) consider(full, fn string, viaLink bool) {
	ext := filepath.Ext(fn)
	if !cExts[ext] {
		return
	}
	st, err := os.Stat(full)
	if err != nil {
		return
	}
	if !st.Mode().IsRegular() {
		w.g.FilesSkippedSpecial++
		return
	}

	if viaLink {
		if rp := realpath(full); rp != full {
			if !strings.HasPrefix(rp, w.realRoot+string(filepath.Separator)) {
				w.g.FilesSkippedEscape++
				return
			}
		}
	}
	w.cands = append(w.cands, fileCand{full: full, fn: fn, ext: ext,
		tooBig: st.Size() > maxFileBytes})
}

type fileCand struct {
	full, fn, ext string
	tooBig        bool

	denied, longLine bool
	data             []byte
	nLines, maxLine  int
	codeL, cmtL      int32
	sum              string
}

// measure reads one candidate and computes what depends only on its bytes.
// It touches nothing shared, so candidates are measured in parallel.
func (c *fileCand) measure() {
	if !c.tooBig {
		data, err := os.ReadFile(c.full)
		if err != nil {
			c.denied = true
			return
		}
		c.data = data
		if hasLongLine(data) {
			c.longLine = true
		}
	}
	c.nLines, _, _, _, c.maxLine = fileLineStatsBytes(c.data)
	c.codeL, c.cmtL, _ = classifyLines(c.data)
	if len(c.data) > 0 {
		h := sha1.Sum(c.data)
		c.sum = hex.EncodeToString(h[:])
	}
}

// admit turns the measured candidates into file rows, in walk order.
func (w *walker) admit() {
	var wg sync.WaitGroup
	var next atomic.Int64
	for k := 0; k < min(defaultParseWorkers, len(w.cands)); k++ {
		wg.Go(func() {
			for {
				i := int(next.Add(1)) - 1
				if i >= len(w.cands) {
					return
				}
				w.cands[i].measure()
			}
		})
	}
	wg.Wait()
	for i := range w.cands {
		c := &w.cands[i]
		if c.denied {
			w.g.FilesSkippedDenied++
			continue
		}
		tooBig := c.tooBig || c.longLine
		if tooBig {
			w.g.FilesSkippedBig++
		}
		data, fn := c.data, c.fn
		rel := filepath.ToSlash(mustRel(w.root, c.full))
		sloc, cmt := int(c.codeL), int(c.cmtL)
		blank := max(c.nLines-sloc-cmt, 0)
		head := data
		if len(head) > 2000 {
			head = head[:2000]
		}
		test := b2i(matchPathSeg(rel, "test", "tests", "test-d", "spec", "specs",
			"__tests__", "__snapshots__", "testing", "e2e", "integration-tests",
			"integration_tests", "testdata", "test_data", "test-data", "fixture",
			"fixtures") || cTestName(fn))
		gen := b2i(isGenerated(fn, string(head)))
		vend := b2i(matchVendorPath(rel))
		mid := w.module(rel)
		parse := b2i(!tooBig && len(data) != 0 &&
			(w.opts.includeTests || test == 0) &&
			(w.opts.includeGenerated || gen == 0) &&
			(w.opts.includeVendored || vend == 0))

		id := int32(len(w.res.files) + 1)
		source = append(source, data)
		w.res.files = append(w.res.files, File{})
		fl := &w.res.files[len(w.res.files)-1]
		cgZeroRow(unsafe.Pointer(fl), unsafe.Sizeof(*fl))
		fl.ID = id
		fl.path = cgPut(rel)
		fl.dir = cgPut(pathDir(rel))
		fl.basename = cgPut(fn)
		fl.ext = cgPut(c.ext)
		fl.lang = cgPut("c")
		fl.ModuleID = mid
		fl.Bytes = int32(len(data))
		fl.Lines = int32(c.nLines)
		fl.Sloc = int32(sloc)
		fl.BlankLines = int32(blank)
		fl.CommentLines = int32(cmt)
		fl.MaxLineLen = int32(c.maxLine)
		fl.sha1 = cgPut(c.sum)
		fl.Parsed = parse
		fl.IsTest = test
		fl.IsGenerated = gen
		fl.IsVendored = vend
		c.data = nil
	}
	w.cands = nil
}

func mustRel(base, p string) string {
	r, err := filepath.Rel(base, p)
	if err != nil {
		return filepath.Base(p)
	}
	return r
}

func (w *walker) module(rel string) int32 {
	name := moduleOf(rel)
	if id, ok := w.modID[name]; ok {
		return id
	}
	id := int32(len(w.res.mods) + 1)
	w.res.mods = append(w.res.mods, Module{})
	mo := &w.res.mods[len(w.res.mods)-1]
	cgZeroRow(unsafe.Pointer(mo), unsafe.Sizeof(*mo))
	mo.ID = id
	mo.name = cgPut(name)
	mo.kind = cgPut(moduleKind(name))
	w.modID[name] = id
	return id
}

func hasLongLine(data []byte) bool {
	n := 0
	for _, c := range data {
		if c == '\n' {
			n = 0
			continue
		}
		n++
		if n > maxLineBytes {
			return true
		}
	}
	return false
}

// classifyLines counts the lines of C text that hold code, the lines that
// hold only comment, and the lines that hold nothing. A line with both is
// code. It tracks block comments across lines and does not look for comment
// openers inside string or character literals.
//
// It replaces two prefix tests: one compared a line's first THREE bytes for
// equality with a two-byte opener and so never matched (a 959-line file
// reported 3 comment lines), the other took any line starting with `*` for a
// comment, `*p = 0;` included. And "sloc" was every non-blank line, licence
// header and all.
func classifyLines(b []byte) (code, cmt, blank int32) {
	inBlock := false
	hasCode, hasCmt := false, false
	endLine := func() {
		switch {
		case hasCode:
			code++
		case hasCmt:
			cmt++
		default:
			blank++
		}
		hasCode, hasCmt = false, false
	}
	n := len(b)
	for i := 0; i < n; i++ {
		c := b[i]
		if c == '\n' || c == '\r' {
			if c == '\r' && i+1 < n && b[i+1] == '\n' {
				i++
			}
			endLine()
			continue
		}
		if inBlock {
			if c == '*' && i+1 < n && b[i+1] == '/' {
				inBlock = false
				hasCmt = true
				i++
			} else if c != ' ' && c != '\t' {
				hasCmt = true
			}
			continue
		}
		switch c {
		case ' ', '\t', '\f', '\v':
		case '/':
			if i+1 < n && b[i+1] == '/' {
				hasCmt = true
				for i+1 < n && b[i+1] != '\n' && b[i+1] != '\r' {
					i++
				}
			} else if i+1 < n && b[i+1] == '*' {
				inBlock, hasCmt = true, true
				i++
			} else {
				hasCode = true
			}
		case '"', '\'':
			hasCode = true
			for i+1 < n && b[i+1] != c && b[i+1] != '\n' && b[i+1] != '\r' {
				if b[i+1] == '\\' && i+2 < n && b[i+2] != '\n' && b[i+2] != '\r' {
					i++
				}
				i++
			}
			if i+1 < n && b[i+1] == c {
				i++
			}
		default:
			hasCode = true
		}
	}
	if hasCode || hasCmt {
		endLine()
	}
	return
}

func fileLineStatsBytes(b []byte) (n, sloc, blank, cmt, maxLen int) {
	add := func(seg []byte) {
		if len(trimSpaceBytes(seg)) != 0 {
			sloc++
		} else {
			blank++
		}
		if isCommentSeg(seg) {
			cmt++
		}
		if r := runeLen(seg); r > maxLen {
			maxLen = r
		}
		n++
	}

	start, i := 0, 0
	for i < len(b) {
		if isSplitBound(b, i) {
			add(b[start:i])
			i = skipBoundary(b, i)
			start = i
			continue
		}
		i++
	}
	if start < len(b) {
		add(b[start:])
	}
	return
}

func isCommentSeg(s []byte) bool {
	i := 0
	for i < len(s) && isStripSpace(s[i]) {
		i++
	}
	head := s[i:]
	if len(head) > 3 {
		head = head[:3]
	}
	for _, p := range cmtPrefixes {
		if string(head) == p {
			return len(trimSpaceBytes(s)) != 0
		}
	}
	return false
}

func pathDir(rel string) string {
	i := strings.LastIndexByte(rel, '/')
	if i < 0 {
		return "."
	}
	return rel[:i]
}

func b2i(b bool) int32 {
	if b {
		return 1
	}
	return 0
}

func identEnd(b []byte, i int) int {
	if i >= len(b) || !isIdentStart(b[i]) {
		return i
	}
	j := i + 1
	for j < len(b) && isIdentPart(b[j]) {
		j++
	}
	return j
}

func skipHardWS(b []byte, i int) int {
	for i < len(b) && isHardWS(b[i]) {
		i++
	}
	return i
}

func skipHT(b []byte, i int) int {
	for i < len(b) && (b[i] == ' ' || b[i] == '\t') {
		i++
	}
	return i
}

func isWordByte(c byte) bool { return isIdentPart(c) }

func wEnd(b []byte, i int) int {
	for i < len(b) && isWordByte(b[i]) {
		i++
	}
	return i
}

func countStr(b []byte, s string) int {
	if len(s) == 0 {
		return 0
	}
	return bytes.Count(b, []byte(s))
}

func hasStr(b []byte, s string) bool { return bytes.Contains(b, []byte(s)) }

func countCast(b []byte) int {
	n := 0
	for i := 0; i+1 < len(b); i++ {
		if b[i] != '(' {
			continue
		}
		j := skipHardWS(b, i+1)
		if j+5 < len(b) && string(b[j:j+5]) == "const" && isHardWS(b[j+5]) {
			j = skipHardWS(b, j+5)
		}
		e := identEnd(b, j)
		if e == j {
			continue
		}
		k := skipHardWS(b, e)
		star := 0
		for k < len(b) && b[k] == '*' {
			star++
			k++
		}
		if star == 0 {
			continue
		}
		k = skipHardWS(b, k)
		if k < len(b) && b[k] == ')' {
			n++
			i = k
		}
	}
	return n
}

var compoundOps = [...]string{"+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="}

func intrinsicName(w string) bool {
	lo := func(c byte) bool {
		return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'
	}
	splitAt := func(from int) bool {
		for j := from; j+1 < len(w); j++ {
			if w[j] != '_' {
				continue
			}
			ok := true
			for k := from; k < j; k++ {
				if !lo(w[k]) {
					ok = false
					break
				}
			}
			if !ok {
				continue
			}
			for k := j + 1; k < len(w); k++ {
				if !lo(w[k]) {
					ok = false
					break
				}
			}
			if ok {
				return true
			}
		}
		return false
	}
	if len(w) < 3 {
		return false
	}
	if w[0] == 'v' {
		return splitAt(1)
	}
	if strings.HasPrefix(w, "_mm") {
		return splitAt(3)
	}
	if strings.HasPrefix(w, "sv") {
		return splitAt(2)
	}
	return false
}

var blankPool = sync.Pool{New: func() any { return new([]byte) }}

func trimSpaceBytes(s []byte) []byte {
	a := 0
	for a < len(s) && isStripSpace(s[a]) {
		a++
	}
	b := len(s)
	for b > a && isStripSpace(s[b-1]) {
		b--
	}
	return s[a:b]
}

func i32(v int) int32 { return int32(v) }

func countPtrCast(b []byte) int { return countCast(b) }

func countMulSizeof(b []byte) int {
	n := 0
	for i := 0; i < len(b); {
		if b[i] == '*' {
			j := skipHardWS(b, i+1)
			if isWordAt(b, j, "sizeof") {
				n++
				i = j + 6
				continue
			}
		}

		if isWordAt(b, i, "sizeof") && !wordAfter(b, i+6) {
			j := skipHardWS(b, i+6)
			if j < len(b) && b[j] == '(' {
				k := indexByteFrom(b, ')', j+1)
				if k >= 0 {
					k = skipHardWS(b, k+1)
					if k < len(b) && b[k] == '*' {
						n++
						i = k + 1
						continue
					}
				}
			}
		}
		i++
	}
	return n
}

var fixedBufTypes = []string{"unsigned char", "uint8_t", "int8_t", "char"}

func countFixedBuffer(b []byte) int {
	n := 0
	for i := 0; i < len(b); i++ {
		if !isIdentStart(b[i]) || wordBefore(b, i) {
			continue
		}
		e := identEnd(b, i)
		w := string(b[i:e])
		ok := false
		for _, t := range fixedBufTypes {
			if w == t || (t == "unsigned char" && w == "unsigned") {
				ok = true
			}
		}
		if !ok {
			continue
		}
		j := skipHardWS(b, e)
		k := identEnd(b, j)
		if k == j {
			continue
		}
		j = skipHardWS(b, k)
		if j >= len(b) || b[j] != '[' {
			continue
		}
		j = skipHT(b, j+1)
		st := j
		for j < len(b) && isDigit(b[j]) {
			j++
		}
		if j == st {
			continue
		}
		m := skipHT(b, j)
		if m < len(b) && b[m] == ']' {
			n++
			i = m
		}
	}
	return n
}

func countVLAPat(b []byte) int {
	n := 0
	for i := 0; i < len(b); i++ {
		if !isIdentStart(b[i]) || wordBefore(b, i) {
			continue
		}
		e := identEnd(b, i)
		w := string(b[i:e])
		switch w {
		case "char", "int", "uint8_t", "double", "float":
		default:
			continue
		}
		j := skipHardWS(b, e)
		k := identEnd(b, j)
		if k == j {
			continue
		}
		j = skipHardWS(b, k)
		if j >= len(b) || b[j] != '[' {
			continue
		}
		j = skipHT(b, j+1)
		if j >= len(b) || !(b[j] == '_' || (b[j] >= 'a' && b[j] <= 'z')) {
			continue
		}
		m := identEnd(b, j)
		m = skipHardWS(b, m)
		if m < len(b) && b[m] == ']' {
			n++
			i = m
		}
	}
	return n
}

func countSignedCmp(b []byte) int {
	n := 0
	for i := 0; i+3 <= len(b); i++ {
		if !isWordAt(b, i, "int") || wordBefore(b, i) || wordAfter(b, i+3) {
			continue
		}
		j := skipHardWS(b, i+3)
		k := wEnd(b, j)
		if k == j {
			continue
		}
		j = skipHardWS(b, k)
		if j >= len(b) || b[j] != '=' || (j+1 < len(b) && b[j+1] == '=') {
			continue
		}
		j = skipHardWS(b, j+1)
		k = wEnd(b, j)
		if k == j {
			continue
		}
		j = skipHardWS(b, k)
		if j >= len(b) || b[j] != '-' {
			continue
		}
		j = skipHardWS(b, j+1)
		k = wEnd(b, j)
		if k > j {
			n++
		}
	}
	return n
}

type memopHitUnused struct{}

// lockWord classifies one word of an identifier: 1 acquire, 2 release, 3 a lock
// word that is neither (mutex, sem), 0 not about locking. Words, because the
// substring test this replaces matched `block`, `unblock`, `clock` and
// `deadlock`: on redis 5 of 6 rows in the locks table were not locks.
func lockWord(w string) int {
	switch w {
	case "lock", "trylock", "rdlock", "wrlock", "tryrdlock", "trywrlock",
		"timedlock", "timedrdlock", "timedwrlock", "spinlock", "acquire":
		return 1
	case "unlock", "release":
		return 2
	case "mutex", "mtx", "rwlock", "spin", "sem", "semaphore", "futex", "lck",
		"locked", "unlocked":
		return 3
	}
	return 0
}

// lockShape splits name on `_` and on lower-to-upper case changes and reports
// (is about a lock, acquires, releases). `acquire` and `release` only count
// beside a real lock word, since both are ordinary verbs.
func lockShape(name string) (shaped, acq, rel bool) {
	var words [12]string
	nw := 0
	start := 0
	flush := func(end int) {
		if end > start && nw < len(words) {
			words[nw] = asciiLower(name[start:end])
			nw++
		}
		start = end
	}
	for i := 0; i < len(name); i++ {
		c := name[i]
		switch {
		case c == '_':
			flush(i)
			start = i + 1
		case c >= 'A' && c <= 'Z' && i > 0 && name[i-1] >= 'a' && name[i-1] <= 'z':
			flush(i)
		}
	}
	flush(len(name))
	hard := false
	verbAcq, verbRel := false, false
	for _, w := range words[:nw] {
		switch lockWord(w) {
		case 1:
			if w == "acquire" {
				verbAcq = true
			} else {
				acq, hard = true, true
			}
		case 2:
			if w == "release" {
				verbRel = true
			} else {
				rel, hard = true, true
			}
		case 3:
			hard = true
		}
	}
	if !hard {
		// advisory file locks: whether one acquires is in its ARGUMENT
		// (LOCK_UN, F_ULOCK), which the caller checks
		switch name {
		case "flock", "lockf", "flockfile", "ftrylockfile":
			return true, true, false
		case "funlockfile":
			return true, false, true
		}
		return false, false, false
	}
	if rel || verbRel {
		return true, false, true
	}
	if w0 := words[0]; nw > 1 && w0 == "sem" {
		switch words[nw-1] {
		case "wait", "trywait", "timedwait":
			return true, true, false
		case "post":
			return true, false, true
		}
	}
	return true, acq || verbAcq, false
}

var narrowTypes = [...]string{"uint8_t", "int8_t", "uint16_t", "int16_t",
	"uint32_t", "int32_t"}

func countNarrowCasts(b []byte) int {
	n := 0
	for i := 0; i+1 < len(b); i++ {
		if b[i] != '(' {
			continue
		}
		j := skipHardWS(b, i+1)
		hit := false
		for _, t := range narrowTypes {
			if isWordAt(b, j, t) && !wordAfter(b, j+len(t)) {
				hit = true
				break
			}
		}
		if !hit {
			continue
		}
		j += 0
		for _, t := range narrowTypes {
			if isWordAt(b, j, t) {
				j += len(t)
				break
			}
		}
		j = skipHardWS(b, j)
		if j >= len(b) || b[j] != ')' {
			continue
		}
		j = skipHardWS(b, j+1)
		if j < len(b) && b[j] == '(' {
			j = skipHardWS(b, j+1)
		}
		if j < len(b) && (isIdentStart(b[j]) || b[j] == '&' || b[j] == '*') {
			n++
			i = j
		}
	}
	return n
}

type signCmpHit struct{ name string }

func gRetNeg(b []byte) int {
	n := 0
	for i := 0; i < len(b); {
		if m := retNegArm(b, i); m > 0 {
			n++
			i += m
			continue
		}
		i++
	}
	return n
}

func retNegArm(b []byte, i int) int {
	switch b[i] {
	case '<':
		if i+1 < len(b) && b[i+1] == '=' {
			j := skipHardWS(b, i+2)
			if j+1 < len(b) && b[j] == '-' && b[j+1] == '1' {
				return j + 2 - i
			}
			return 0
		}
		j := skipHardWS(b, i+1)
		if j < len(b) && b[j] == '0' {
			return j + 1 - i
		}
	case '=', '!':
		if i+1 < len(b) && b[i+1] == '=' {
			j := skipHardWS(b, i+2)
			if j+1 < len(b) && b[j] == '-' && b[j+1] == '1' {
				return j + 2 - i
			}
		}
	case '-':
		if i > 0 && isWordByte(b[i-1]) {
			return 0
		}
		j := skipHardWS(b, i+1)
		if j < len(b) && b[j] == '1' {
			k := skipHardWS(b, j+1)
			if isWordAt(b, k, "==") || isWordAt(b, k, "!=") {
				return k + 2 - i
			}
		}
	}
	return 0
}

func gDomain(b []byte) int {
	n := 0
	for i := 0; i < len(b); {
		if m := domainArm(b, i); m > 0 {
			n++
			i += m
			continue
		}
		i++
	}
	return n
}

func domainArm(b []byte, i int) int {
	if isIdentStart(b[i]) && !wordBefore(b, i) {
		w := b[i:identEnd(b, i)]
		if string(w) == "if" || string(w) == "fabs" {
			j := skipHardWS(b, i+len(w))
			if j < len(b) && b[j] == '(' {
				return j + 1 - i
			}
		}
	}
	if isWordAt(b, i, "clamp") && !wordBefore(b, i) && !wordAfter(b, i+5) {
		return 5
	}
	if b[i] != '>' && b[i] != '<' {
		return 0
	}
	if i+1 < len(b) && b[i+1] == '=' {
		return 2
	}
	j := skipHardWS(b, i+1)
	if j >= len(b) {
		return 0
	}
	switch {
	case b[j] == '0':
		return j + 1 - i
	case b[j] == '1' && b[i] == '<':
		return j + 1 - i
	case b[j] == '-' && b[i] == '>' && j+1 < len(b) && b[j+1] == '1':
		return j + 2 - i
	}
	return 0
}

func gUchar(b []byte) int {
	n := 0
	for i := 0; i+1 < len(b); i++ {
		if b[i] != '(' {
			continue
		}
		j := skipHardWS(b, i+1)
		if !isWordAt(b, j, "unsigned") {
			continue
		}
		j = skipHardWS(b, j+8)
		if !isWordAt(b, j, "char") {
			continue
		}
		j = skipHardWS(b, j+4)
		if j < len(b) && b[j] == ')' {
			n++
			i = j
		}
	}
	return n
}

func gErrnoZero(b []byte) int {
	n := 0
	for i := 0; i+5 <= len(b); i++ {
		if !isWordAt(b, i, "errno") || wordBefore(b, i) || wordAfter(b, i+5) {
			continue
		}
		j := skipHardWS(b, i+5)
		if j < len(b) && b[j] == '=' && (j+1 >= len(b) || b[j+1] != '=') {
			k := skipHardWS(b, j+1)
			if k < len(b) && b[k] == '0' {
				n++
			}
		}
	}
	return n
}

func gEndptr(b []byte) int {
	n := 0
	for i := 0; i < len(b); {
		if isWordAt(b, i, "endptr") {
			n++
			i += 6
			continue
		}
		if b[i] == '&' {
			j := skipHardWS(b, i+1)
			k := wEnd(b, j)
			if k > j {
				k = skipHardWS(b, k)
				if k < len(b) && b[k] == ')' {
					k = skipHardWS(b, k+1)
					if k < len(b) && b[k] == ';' {
						n++
						i = k + 1
						continue
					}
				}
			}
		}
		i++
	}
	return n
}

func gVaEnd(b []byte) int { return countCall(b, "va_end") }

func gMapFailed(b []byte) int { return countWord(b, "MAP_FAILED") }

func gMonotonic(b []byte) int {
	return countWord(b, "CLOCK_MONOTONIC") + countWord(b, "CLOCK_MONOTONIC_RAW") +
		countWord(b, "CLOCK_BOOTTIME")
}

func gStackszArray(b []byte) int {
	n := 0
	for i := range b {
		if !isWordByte(b[i]) {
			continue
		}
		j := skipHardWS(b, identEnd(b, i))
		if j >= len(b) || b[j] != '[' {
			continue
		}
		k := skipHT(b, j+1)
		for _, w := range [...]string{"SIGSTKSZ", "MINSIGSTKSZ", "PTHREAD_STACK_MIN"} {
			if isWordAt(b, k, w) {
				m := skipHT(b, k+len(w))
				if m < len(b) && b[m] == ']' {
					n++
				}
			}
		}
	}
	return n
}

func gPtrOvf(b []byte) int {
	n := 0
	for i := range b {

		if !isWordByte(b[i]) || (i > 0 && isWordByte(b[i-1])) {
			continue
		}
		e := identEnd(b, i)
		name := string(b[i:e])
		j := skipHardWS(b, e)
		if j >= len(b) || b[j] != '+' {
			continue
		}
		j = skipHardWS(b, j+1)
		k := j
		for k < len(b) && isWordByte(b[k]) {
			k++
		}
		if k == j {
			continue
		}
		k = skipHardWS(b, k)
		if k >= len(b) || b[k] != '<' {
			continue
		}
		k = skipHardWS(b, k+1)
		if isWordAt(b, k, name) && !wordAfter(b, k+len(name)) {
			n++
		}
	}
	return n
}

func gCallocTransposed(b []byte) int {
	n := 0
	for i := 0; i+6 <= len(b); i++ {
		if !isWordAt(b, i, "calloc") || wordBefore(b, i) || wordAfter(b, i+6) {
			continue
		}
		j := skipHardWS(b, i+6)
		if j >= len(b) || b[j] != '(' {
			continue
		}
		j = skipHT(b, j+1)
		if isWordAt(b, j, "sizeof") {
			n++
		}
	}
	return n
}

func gVaArgArray(b []byte) int {
	n := 0
	for i := 0; i+6 <= len(b); i++ {
		if !isWordAt(b, i, "va_arg") || wordBefore(b, i) || wordAfter(b, i+6) {
			continue
		}
		j := skipHardWS(b, i+6)
		if j >= len(b) || b[j] != '(' {
			continue
		}
		region := indexByteFrom(b, ';', j+1)
		if region < 0 {
			region = len(b)
		}
		if region > len(b) {
			region = len(b)
		}
		for q := j + 1; q < region; q++ {
			if q+1 < region && b[q] == '(' {
				break
			}
			if b[q] == '[' {
				r := indexByteFrom(b, ']', q+1)
				if r > 0 && r < region {
					n++
				}
				break
			}
		}
	}
	return n
}

func countWord(b []byte, w string) int {
	n := 0
	for i := 0; i+len(w) <= len(b); i++ {
		if !isWordAt(b, i, w) {
			continue
		}
		if wordBefore(b, i) || wordAfter(b, i+len(w)) {
			continue
		}
		n++
		i += len(w) - 1
	}
	return n
}

type evFlags struct {
	etReg, oneshotReg, writeReady, errFlag, rearm, dereg int
	eagain, eintr, uringRes, uringSqpoll                 int
	nonblockSet, eventDestroy, uringSqe, uringSeen       int
	uringUdata, uringLink, uringStreamOps, uringTeardown int
	kqTimer, kqReceipt, uringRing, uringBarrier          int
}

func countFlagWords(b []byte, words ...string) int {
	n := 0
	for _, w := range words {
		n += countWord(b, w)
	}
	return n
}

func scanEvFlags(b []byte) evFlags {
	var f evFlags
	f.etReg = countFlagWords(b, "EPOLLET", "EV_CLEAR")
	f.oneshotReg = countFlagWords(b, "EPOLLONESHOT", "EV_ONESHOT", "EV_DISPATCH")
	f.writeReady = countFlagWords(b, "EPOLLOUT", "EVFILT_WRITE")
	f.errFlag = countFlagWords(b, "EPOLLERR", "EPOLLHUP", "EV_EOF", "EV_ERROR")
	f.rearm = countFlagWords(b, "EPOLL_CTL_MOD", "EV_ENABLE")
	f.dereg = countFlagWords(b, "EPOLL_CTL_DEL", "EV_DELETE", "EV_DISABLE")
	f.eagain = countFlagWords(b, "EAGAIN", "EWOULDBLOCK")
	f.eintr = countWord(b, "EINTR")
	f.uringSqpoll = countWord(b, "IORING_SETUP_SQPOLL")
	f.nonblockSet = countFlagWords(b, "O_NONBLOCK", "SOCK_NONBLOCK", "FIONBIO", "F_SETFL")
	f.uringSqe = countCall(b, "io_uring_get_sqe")
	f.uringSeen = countCall(b, "io_uring_cqe_seen") + countCall(b, "io_uring_cq_advance")
	f.uringUdata = scanUringUdata(b)
	f.uringLink = countWord(b, "IOSQE_IO_LINK")
	f.uringStreamOps = scanStreamOps(b)
	f.uringTeardown = countCall(b, "io_uring_queue_exit")
	f.kqTimer = countWord(b, "EVFILT_TIMER")
	f.kqReceipt = countWord(b, "EV_RECEIPT")
	f.eventDestroy = countCall(b, "close", "io_uring_queue_exit") +
		scanUnregister(b)
	f.uringRing = scanUringRing(b)
	f.uringBarrier = scanUringBarrier(b)
	f.uringRes = scanUringRes(b)
	return f
}

func scanUringRes(b []byte) int {
	n := 0
	for i := 0; i+3 <= len(b); i++ {
		if isWordAt(b, i, "res") && !wordBefore(b, i) && !wordAfter(b, i+3) {
			j := skipHardWS(b, i+3)
			if j < len(b) && b[j] == '<' {
				k := skipHardWS(b, j+1)
				if k < len(b) && b[k] == '0' {
					n++
				}
			}
			if i >= 2 && b[i-1] == '-' && b[i-2] == '>' {
				n++
			}
			if i >= 1 && b[i-1] == '.' {
				n++
			}
		}
	}
	return n
}

func scanUringUdata(b []byte) int {
	n := countCall2(b, "io_uring_sqe_set_data")
	for i := 0; i+9 <= len(b); i++ {
		if !isWordAt(b, i, "user_data") || wordBefore(b, i) || wordAfter(b, i+9) {
			continue
		}
		j := skipHardWS(b, i+9)
		if j < len(b) && b[j] == '=' && (j+1 >= len(b) || b[j+1] != '=') {
			k := skipHardWS(b, j+1)
			if k < len(b) && b[k] != ';' {
				n++
			}
		}
	}
	return n
}

func countCall2(b []byte, prefix string) int {
	n := 0
	for i := 0; i+len(prefix) <= len(b); i++ {
		if !isWordAt(b, i, prefix) || wordBefore(b, i) {
			continue
		}
		e := identEnd(b, i+len(prefix))
		j := skipHardWS(b, e)
		if j < len(b) && b[j] == '(' {
			n++
			i = e - 1
		}
	}
	return n
}

func scanStreamOps(b []byte) int {
	n := 0
	for i := 0; i+15 <= len(b); i++ {
		if !isWordAt(b, i, "io_uring_prep_") || wordBefore(b, i) {
			continue
		}
		p := i + 15
		for _, suf := range [...]string{"sendmsg", "sendzc", "send", "recvmsg",
			"recv", "send_zc"} {
			if !isWordAt(b, p, suf) {
				continue
			}
			e := p + len(suf)
			if wordAfter(b, e) {
				continue
			}
			j := skipHardWS(b, e)
			if j < len(b) && b[j] == '(' {
				n++
				i = e - 1
			}
			break
		}
	}
	return n
}

func scanUnregister(b []byte) int { return countCall2(b, "io_uring_unregister") }

func scanUringRing(b []byte) int {
	inClass := func(c byte) bool {
		return isWordByte(c) || c == '.' || c == '>' || c == '-'
	}
	n := 0
	i := 0
	for i < len(b) {
		if !isWordAt(b, i, "ring") {
			i++
			continue
		}

		st := i
		for st > 0 && isWordByte(b[st-1]) {
			st--
		}
		if st > 0 && !isWordAt(b, st, "") && b[st-1] == 0 {
			i++
			continue
		}
		_ = st

		rs := i
		for rs > 0 && inClass(b[rs-1]) && !isHardWS(b[rs-1]) {
			rs--
		}
		re := i + 4
		for re < len(b) && inClass(b[re]) && !isHardWS(b[re]) {
			re++
		}
		matched := false
		g := min(re-i-4, 16)
		for ; g >= 0; g-- {
			q := i + 4 + g
			if q+4 > re && q+4 > len(b) {
				continue
			}
			if !isWordAt(b, q, "head") && !isWordAt(b, q, "tail") {
				continue
			}
			if !wordAfter(b, q+4) {
				continue
			}
			if q == 0 || !isWordByte(b[q-1]) {
				n++
				matched = true
				i = q + 4 - 1
				break
			}
		}
		if !matched {
			i++
		}
	}
	return n
}

func scanUringBarrier(b []byte) int {
	n := 0
	for i := 0; i < len(b); i++ {
		if !isIdentStart(b[i]) || wordBefore(b, i) {
			continue
		}
		e := identEnd(b, i)
		hit := ""
		switch w := string(b[i:e]); {
		case w == "io_uring_smp_load_acquire", w == "io_uring_smp_store_release",
			w == "smp_load_acquire", w == "smp_store_release",
			w == "atomic_load_explicit", w == "atomic_store_explicit",
			w == "atomic_load", w == "atomic_store",
			w == "__atomic_load_n", w == "__atomic_store_n",
			w == "__atomic_load", w == "__atomic_store":
			hit = w
		}
		if hit == "" {
			continue
		}
		j := skipHardWS(b, e)
		if j < len(b) && b[j] == '(' {
			n++
			i = e - 1
		}
	}
	return n
}

func countCall(b []byte, names ...string) int {
	n := 0
	for i := 0; i < len(b); i++ {
		if !isIdentStart(b[i]) || wordBefore(b, i) {
			continue
		}
		e := identEnd(b, i)
		w := string(b[i:e])
		if slices.Contains(names, w) {
			p := skipHardWS(b, e)
			if p < len(b) && b[p] == '(' {
				n++
				i = e - 1
			}
		}
	}
	return n
}

func countWeakRandom(b []byte) int {
	return countCall(b, "rand", "random", "srand", "srandom", "drand48",
		"lrand48", "mrand48")
}

func countShiftVar(b []byte) int {
	n := 0
	for i := 0; i+1 < len(b); i++ {
		if b[i] != '<' && b[i] != '>' || b[i] != b[i+1] {
			continue
		}
		j := skipHardWS(b, i+2)
		if j < len(b) && (b[j] == '(' || isIdentStart(b[j])) {
			n++
			i = j
		}
	}
	return n
}

func countReallocSelf(b []byte) int {
	n := 0
	for i := range b {
		if !isIdentStart(b[i]) || wordBefore(b, i) {
			continue
		}
		e := identEnd(b, i)
		name := string(b[i:e])
		j := skipHardWS(b, e)
		if j >= len(b) || b[j] != '=' || (j+1 < len(b) && b[j+1] == '=') {
			continue
		}
		j = skipHardWS(b, j+1)

		run := identEnd(b, j)
		ok := false
		for k := run; k >= j; k-- {
			if !isWordAt(b, k, "realloc") || wordAfter(b, k+7) {
				continue
			}
			p := skipHardWS(b, k+7)
			if p >= len(b) || b[p] != '(' {
				continue
			}
			p = skipHardWS(b, p+1)
			if !isWordAt(b, p, name) || wordAfter(b, p+len(name)) {
				continue
			}
			q := skipHardWS(b, p+len(name))
			if q < len(b) && b[q] == ',' {
				ok = true
				break
			}
		}
		if ok {
			n++
		}
	}
	return n
}

func countAssertSide(b []byte) int {
	n := 0
	alt := func(i int) bool {
		if isWordAt(b, i, "strcpy") || isWordAt(b, i, "memcpy") ||
			isWordAt(b, i, "malloc") || isWordAt(b, i, "printf") {
			return true
		}
		if i+1 < len(b) && b[i] == '=' && b[i+1] != '=' {
			// `==`, `!=`, `<=` and `>=` end in an `=` that assigns nothing;
			// `<<=` and `>>=` do assign
			prev := byte(0)
			if i > 0 {
				prev = b[i-1]
			}
			switch prev {
			case '=', '!':
				return false
			case '<', '>':
				return i > 1 && b[i-2] == prev
			}
			return true
		}
		return i+1 < len(b) && ((b[i] == '+' && b[i+1] == '+') ||
			(b[i] == '-' && b[i+1] == '-'))
	}
	for i := 0; i+6 <= len(b); i++ {
		if !isWordAt(b, i, "assert") || wordBefore(b, i) || wordAfter(b, i+6) {
			continue
		}
		p := skipHardWS(b, i+6)
		if p >= len(b) || b[p] != '(' {
			continue
		}
		region := indexByteFrom(b, ';', p+1)
		if region < 0 {
			region = len(b)
		}
		found := -1
		for q := p + 1; q < region; q++ {
			if alt(q) {
				found = q
				break
			}
		}
		if found < 0 {
			continue
		}
		end := -1
		for q := region - 1; q > found; q-- {
			if b[q] == ')' {
				end = q
				break
			}
		}
		if end < 0 {
			continue
		}
		n++
		i = end
	}
	return n
}

type freeHit struct {
	name    string
	afterPl int
}

func scanFrees(b []byte, fn func(freeHit)) {
	for i := 0; i < len(b); i++ {
		if !isIdentStart(b[i]) || wordBefore(b, i) {
			continue
		}
		e := identEnd(b, i)
		hit := false
		for j := e; j >= i; j-- {
			if isWordAt(b, j, "free") && !wordAfter(b, j+4) {
				p := skipHardWS(b, j+4)
				if p < len(b) && b[p] == '(' {
					p = skipHardWS(b, p+1)
					q := identEnd(b, p)
					if q > p && q < len(b) && b[q] == ')' {
						fn(freeHit{name: string(b[p:q]), afterPl: q + 1})
						hit = true
					}
				}
				break
			}
		}
		if hit {
			i = e - 1
		}
	}
}

func derefAfter(tail []byte, name string) bool {
	for i := range tail {
		if isWordAt(tail, i, name) {
			if i == 0 || (tail[i-1] != '.' && !isWordByte(tail[i-1])) {
				j := skipHardWS(tail, i+len(name))
				if j < len(tail) && (tail[j] == '[' ||
					(tail[j] == '-' && j+1 < len(tail) && tail[j+1] == '>')) {
					return true
				}
			}
		}
		if tail[i] == '*' {
			j := skipHardWS(tail, i+1)
			if isWordAt(tail, j, name) {
				k := j + len(name)
				if !wordAfter(tail, k) {
					mm := skipHardWS(tail, k)
					if mm >= len(tail) || tail[mm] != '=' ||
						(mm+1 < len(tail) && tail[mm+1] == '=') {
						return true
					}
				}
			}
		}
	}
	return false
}

func isWordAt(b []byte, i int, s string) bool {
	return i+len(s) <= len(b) && string(b[i:i+len(s)]) == s
}

func asciiLower(s string) string {
	b := []byte(s)
	for i := range b {
		if b[i] >= 'A' && b[i] <= 'Z' {
			b[i] += 32
		}
	}
	return string(b)
}

var secretPats = func() []string {
	return []string{
		"api_key", "api-key", "apikey", "secret", "password", "passwd", "pwd",
		"token", "bearer", "access_key", "access-key", "private_key",
		"private-key", "client_secret", "client-secret", "auth_token",
		"auth-token", "jwt", "credential", "smtp_pass", "smtp-pass", "db_pass",
		"db-pass", "sk_live", "rk_live", "pk_live", "ghp_", "xoxb-", "akia",
	}
}()

var allocNameRe = allocNameMatcher{}

type allocNameMatcher struct{}

func (allocNameMatcher) MatchString(s string) bool {
	low := asciiLower(s)
	for _, suf := range [...]string{"alloc", "free", "strdup", "memdup"} {
		if len(low) > len(suf) && low[len(low)-len(suf):] == suf {
			return true
		}
	}
	return false
}

var brackets = strings.NewReplacer("[", "", "]", "")

type extractor struct {
	g *Graph

	fnByName                              map[string][]int32
	fnByNameF                             map[string][]int32
	fnByNameM                             map[string][]int32
	fnByNameS                             map[string][]bool
	incAdj                                map[int32][]int32
	pickMore                              []int
	ambigN, declPickN                     int32
	declFiles                             map[string][]int32
	incReach                              map[int32]map[int32]bool
	macroSID                              map[string]int32
	declared                              map[string]bool
	byBasename                            map[string]int32
	nSym                                  int32
	edgeN, macroN, externN, declN, unresN int32
	callsTotal                            int32

	pendSID  []int32
	pendFID  []int32
	pendMID  []int32
	pendName []string
	pendLine [][]int32
}

func newExtractor(g *Graph) *extractor {
	return &extractor{
		g:          g,
		fnByName:   map[string][]int32{},
		fnByNameF:  map[string][]int32{},
		fnByNameM:  map[string][]int32{},
		fnByNameS:  map[string][]bool{},
		macroSID:   map[string]int32{},
		declared:   map[string]bool{},
		byBasename: map[string]int32{},
	}
}

type fld struct {
	ordinal int32
	ftype   string
	fname   string
	ptr     int32
	alen    int32
	isFnptr int32
	depth   int32
	inUnion int32
	line    int32

	// size already known from a definition in this file: a nested or
	// earlier struct/union, or a typedef of one
	kn            bool
	ksz, kal, kex int32
}

func jtrunc(s string, n int) string {
	c := 0
	for i := range s {
		if c == n {
			return s[:i]
		}
		c++
	}
	return s
}

func maxi(a, b int) int {
	if a > b {
		return a
	}
	return b
}

func squeeze(s, cut string) string {
	s = strings.Trim(s, cut)

	var sb strings.Builder
	sb.Grow(len(s))
	sp := false
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
			c == '\f' || c == 0x1c || c == 0x1d || c == 0x1e {
			sp = true
			continue
		}
		if sp {
			sb.WriteByte(' ')
			sp = false
		}
		sb.WriteByte(c)
	}
	if sp {
		sb.WriteByte(' ')
	}
	return sb.String()
}

func typeSizeOf(ctype string, ptrDepth, arrayLen int32) (int32, int32, int32) {
	var base, align, exact int32
	if ptrDepth > 0 {
		base, align, exact = ptrSize, ptrSize, 1
	} else {
		t := ctype
		for _, w := range [...]string{"const", "volatile", "struct", "union",
			"enum", "_Atomic", "register"} {
			t = replaceWord(t, w, " ")
		}
		t = trimSpace(squeeze(t, ""))
		if v, ok := typeSizeTable[t]; ok {
			base = v
			if a, ok := typeAlignTable[t]; ok {
				align = a
			} else if base < 8 {
				align = base
			} else {
				align = 8
			}
			exact = 1
		}
	}
	if arrayLen > 0 && base != 0 {
		return base * arrayLen, align, exact
	}
	if arrayLen < 0 {
		return 0, align, 0
	}
	return base, align, exact
}

func replaceWord(s, w, rep string) string {
	var sb strings.Builder
	i := 0
	for i < len(s) {
		if isWordAt([]byte(s), i, w) && (i == 0 || !isIdentPart(s[i-1])) &&
			(i+len(w) >= len(s) || !isIdentPart(s[i+len(w)])) {
			sb.WriteString(rep)
			i += len(w)
			continue
		}
		sb.WriteByte(s[i])
		i++
	}
	return sb.String()
}

// size is one member's (bytes, alignment, exact).
func (f *fld) size() (int32, int32, int32) {
	sz, al, ex := typeSizeOf(f.ftype, f.ptr, f.alen)
	if f.kn && f.ptr == 0 {
		sz, al, ex = f.ksz, f.kal, f.kex
		if f.alen > 0 {
			sz *= f.alen
		} else if f.alen < 0 {
			sz, ex = 0, 0
		}
	}
	return sz, al, ex
}

// packedSize is the smallest size the struct's direct members can occupy:
// what a compiler produces when they are declared widest-alignment-first.
// Sorting by alignment is optimal here because every alignment is a power of
// two and every size is a multiple of its own alignment, so no hole can open
// between two members -- only tail padding remains. Returns 0 unless every
// member is exactly sized.
func packedSize(flds []fld) int32 {
	type sa struct{ sz, al int32 }
	var m []sa
	for i := range flds {
		if flds[i].depth != 0 || flds[i].inUnion != 0 {
			continue
		}
		sz, al, ex := flds[i].size()
		if ex == 0 || al <= 0 {
			return 0
		}
		m = append(m, sa{sz, al})
	}
	sort.SliceStable(m, func(i, j int) bool { return m[i].al > m[j].al })
	off, maxAl := int32(0), int32(1)
	for _, x := range m {
		if r := off % x.al; r != 0 {
			off += x.al - r
		}
		off += x.sz
		maxAl = max(maxAl, x.al)
	}
	if r := off % maxAl; r != 0 {
		off += maxAl - r
	}
	return off
}

func layoutStruct(flds []fld, isUnion bool) ([][5]int32, int32, int32, int32, int32) {
	off := int32(0)
	maxAlign := int32(1)
	allExact := int32(1)
	out := make([][5]int32, 0, len(flds))
	for _, f := range flds {
		if f.depth != 0 || f.inUnion != 0 {
			out = append(out, [5]int32{f.ordinal, -1, 0, 0, 0})
			continue
		}
		sz, al, ex := f.size()
		if ex == 0 {
			allExact = 0
		}
		if al == 0 {
			al = 1
		}
		if al > maxAlign {
			maxAlign = al
		}
		var pad int32
		if !isUnion {
			pad = (int32(-off)) % al
			if pad < 0 {
				pad += al
			}
		}
		thisOff := int32(0)
		if !isUnion {
			thisOff = off + pad
		}
		out = append(out, [5]int32{f.ordinal, thisOff, sz, pad, ex})
		if isUnion {
			if sz > off {
				off = sz
			}
		} else {
			off += pad + sz
		}
	}
	tail := int32(0)
	if maxAlign > 0 {
		tail = (int32(-off)) % maxAlign
		if tail < 0 {
			tail += maxAlign
		}
	}
	return out, off + tail, tail, allExact, maxAlign
}

func lineText(b []byte, nl []int32, n int) []byte {
	start := int32(0)
	if n > 1 {
		if n-2 >= len(nl) {
			return nil
		}
		start = nl[n-2]
	}
	end := int32(len(b))
	if n-1 < len(nl) {
		end = nl[n-1]
	}
	if end < start {
		return nil
	}
	return b[start:end]
}

func leadingComment(b []byte, nl []int32, lineStart int) (bool, int) {
	n := 0
	i := lineStart - 2
	for i >= 0 {
		s := trimSpaceBytes(lineText(b, nl, i+1))
		if len(s) == 0 && n == 0 {
			i--
			continue
		}
		if bytes.HasPrefix(s, []byte("/*")) || bytes.HasPrefix(s, []byte("*")) ||
			bytes.HasPrefix(s, []byte("*/")) || bytes.HasPrefix(s, []byte("//")) {
			n++
			i--
			continue
		}
		break
	}
	return n > 0, n
}

func eventFamily(fn string) string {
	switch {
	case strings.HasPrefix(fn, "epoll_"):
		return "epoll"
	case strings.HasPrefix(fn, "io_uring_"):
		return "io_uring"
	}
	return "kqueue"
}

func nsIntersects(used, want map[string]bool) bool {
	for k := range want {
		if used[k] {
			return true
		}
	}
	return false
}

func countGuard(g string, b []byte) int {
	switch g {
	case "ret_neg":
		return gRetNeg(b)
	case "domain":
		return gDomain(b)
	case "uchar":
		return gUchar(b)
	case "errno_zero":
		return gErrnoZero(b)
	case "endptr":
		return gEndptr(b)
	case "va_end":
		return gVaEnd(b)
	case "map_failed":
		return gMapFailed(b)
	case "monotonic":
		return gMonotonic(b)
	case "stacksz_array":
		return gStackszArray(b)
	case "ptr_ovf_check":
		return gPtrOvf(b)
	case "calloc_sizeof_first":
		return gCallocTransposed(b)
	case "va_arg_array":
		return gVaArgArray(b)
	}
	return 0
}

var typedSizeofWords = []string{"struct", "union", "enum"}

func firstIdent(s string) (string, bool) {
	for i := 0; i < len(s); i++ {
		if isIdentStart(s[i]) {
			e := i + 1
			for e < len(s) && isIdentPart(s[e]) {
				e++
			}
			return s[i:e], true
		}
	}
	return "", false
}

func lastPathTail(dst string) string {
	parts := strings.Split(dst, "")
	_ = parts
	segs := strings.FieldsFunc(dst, func(r rune) bool { return false })
	_ = segs
	last := ""
	start := 0
	for i := 0; i <= len(dst); i++ {
		if i == len(dst) || isPathTailSep(dst[i]) {
			last = dst[start:i]
			start = i + 1
		}
	}
	t := trimSpace(last)
	if t == "" {
		return dst
	}
	return t
}

func isPathTailSep(c byte) bool {
	return c == '.' || c == '>' || c == '[' || c == ']'
}

var magicOKCache sync.Map

func (g *Graph) tSymbols() [][]fval {
	out := make([][]fval, 0, len(g.Symbols))
	for i := range g.Symbols {
		out = append(out, g.symbolRow(g.Symbols[i]))
	}
	return out
}

func (g *Graph) symbolRow(s *Symbol) []fval {
	r := g.rv(s)
	return []fval{
		fI32(s.ID),
		fI32(s.FileID),
		fI32(s.ModuleID),
		fNull(),
		fStr(s.Name()),
		fStr(s.QualName()),
		fStr(s.Kind()),
		fI32(s.LineStart),
		fI32(0),
		fI32(0),
		fOptStr(s.Signature(), s.HasSignature),
		fOptStr(s.ReturnType(), s.HasReturnType),
		fI32(s.NParams),
		fI32(0),
		fI32(0),
		fI32(0),
		fI32(0),
		fI32(s.IsPublic),
		fI32(s.IsStatic),
		fI32(0),
		fI32(0),
		fI32(0),
		fI32(0),
		fI32(0),
		fI32(s.IsTest),
		fI32(0),
		fI32(s.IsEntrypoint),
		fI32(s.IsGenerated),
		fI32(s.Sloc),
		fI32(s.NCommentLines),
		fI32(s.HasDoc),
		fI32(s.Cyclomatic),
		fI32(s.Cognitive),
		fI32(s.MaxNesting),
		fI32(s.NTokens),
		fI32(s.NOperators),
		fI32(s.NOperands),
		fI32(s.NDistinctOperators),
		fI32(s.NDistinctOperands),
		fI32(s.NLoops),
		fI32(s.NBranches),
		fI32(s.NReturns),
		fI32(s.NSwitch),
		fI32(s.NCases),
		fI32(0),
		fI32(0),
		fI32(0),
		fI32(0),
		fI32(0),
		fI32(0),
		fI32(s.NLabels),
		fI32(s.NGotos),
		fI32(s.MaxLoopDepth),
		fI32(s.CallInLoop),
		fI32(s.AllocInLoop),
		fI32(s.IOInLoop),
		fI32(0),
		fI32(s.LockInLoop),
		fI32(0),
		fI32(0),
		fI32(0),
		fI32(s.BranchInLoop),
		fI32(s.NLocals),
		fI32(s.NCmp),
		fI32(s.NShift),
		fI32(s.NArith),
		fI32(0),
		fI32(s.NFloatLit),
		fI32(s.NMagic),
		fI32(s.NNullCheck),
		fI32(0),
		fI32(0),
		fI32(s.NCalls),
		fI32(s.NDynamicCalls),
		fI32(s.NUnresolvedCalls),
		fI32(s.FanIn),
		fI32(s.FanOut),
		fI32(s.NCallsites),
		fI32(s.IsRecursive),
		fI32(s.RiskScore),
		fI32(s.NMemory),
		fI32(s.NAlloc),
		fI32(s.NIO),
		fI32(s.NStdio),
		fI32(s.NExec),
		fI32(s.NLibm),
		fI32(s.NInteger),
		fI32(s.NConcurrency),
		fI32(s.NReentrancy),
		fI32(s.IsInline),
		fI32(s.IsVariadic),
		fI32(s.NPtrLocals),
		fI32(s.NDeref),
		fI32(s.NCast),
		fI32(s.NSizeof),
		fI32(s.NIntrinsic),
		fI32(s.NAtomic),
		fI32(s.NRestrict),
		fI32(s.NLikely),
		fI32(s.NBuiltin),
		fI32(r.SwitchInLoop),
		fI32(r.LibmInLoop),
		fI32(r.DivInLoop),
		fI32(r.StrlenInLoop),
		fI32(r.RetNull),
		fI32(r.RetNeg),
		fI32(r.RetZero),
		fI32(r.RetVal),
		fI32(r.RetVoid),
		fI32(r.NFnptrCalls),
		fI32(r.NMacroCalls),
		fI32(r.NExternalCalls),
		fI32(r.NFree),
		fI32(r.NConstCast),
		fI32(r.NToctou),
		fI32(r.NLockAcquire),
		fI32(r.NLockRelease),
		fI32(r.NNarrowCast),
		fI32(r.NSignCmp),
		fI32(r.NVariadicFmt),
		fI32(r.NMemcpy),
		fI32(r.NAllocsite),
		fI32(r.NGlobalWrite),
		fI32(r.NErrno),
		fI32(r.NWeakRandom),
		fI32(r.NShiftVar),
		fI32(r.NReallocSelf),
		fI32(r.NVla),
		fI32(r.NGetenv),
		fI32(r.NAssertSide),
		fI32(r.NFreeThenUse),
		fI32(r.NEpoll),
		fI32(r.NUring),
		fI32(r.NKqueue),
		fI32(r.NEventWait),
		fI32(r.NEtReg),
		fI32(r.NOneshotReg),
		fI32(r.NWriteReady),
		fI32(r.NErrFlag),
		fI32(r.NRearm),
		fI32(r.NDereg),
		fI32(r.NEagain),
		fI32(r.NEintr),
		fI32(r.NUringRes),
		fI32(r.NUringRing),
		fI32(r.NUringBarrier),
		fI32(r.NUringSqpoll),
		fI32(r.NNonblockSet),
		fI32(r.NEventCreate),
		fI32(r.NEventDestroy),
		fI32(r.NUringSqe),
		fI32(r.NUringSeen),
		fI32(r.NUringUdata),
		fI32(r.NUringLink),
		fI32(r.NUringStreamOps),
		fI32(r.NUringTeardown),
		fI32(r.NEvTimeoutIndefinite),
		fI32(r.NEvTimeoutZero),
		fI32(r.NEvBatchOne),
		fI32(r.NKqTimer),
		fI32(r.NKqTimerZeroData),
		fI32(r.NKqReceipt),
		fI32(r.NRetNegCheck),
		fI32(r.NDomainGuard),
		fI32(r.NUcharCast),
		fI32(r.NErrnoZero),
		fI32(r.NEndptr),
		fI32(r.NVaEnd),
		fI32(r.NMapFailed),
		fI32(r.NMonotonicClock),
		fI32(r.NStackszArray),
		fI32(r.NPtrOvfCheck),
		fI32(r.NCallocTransposed),
		fI32(r.NVaArgArr),
	}
}

func (g *Graph) tFTS() {

	rows := make([]string, len(g.Symbols))
	for i := range rows {
		rows[i] = `R \N \N \N`
	}
	sort.Strings(rows)
	fmt.Fprintf(g.dumpW, "T sym_fts 3 %d\n", len(rows))
	for _, r := range rows {
		g.dumpW.WriteString(r)
		g.dumpW.WriteByte('\n')
	}
	g.dumpW.WriteString("E sym_fts\n")
	g.dumpW.WriteString("T sym_fts_config 2 1\n")
	g.dumpW.WriteString(`R s:version i:4` + "\n")
	g.dumpW.WriteString("E sym_fts_config\n")

	lines := make([]string, 0, len(g.Symbols))
	for i := range g.Symbols {
		s := g.Symbols[i]
		blob := append(svarint(tokenCount(s.Name())), svarint(tokenCount(s.QualName()))...)
		blob = append(blob, svarint(tokenCount(s.Signature()))...)
		lines = append(lines, "R i:"+itoa(int(s.ID))+" s:"+escapeText(reprBytes(blob)))
	}
	sort.Strings(lines)
	fmt.Fprintf(g.dumpW, "T sym_fts_docsize 2 %d\n", len(lines))
	for _, l := range lines {
		g.dumpW.WriteString(l)
		g.dumpW.WriteByte('\n')
	}
	g.dumpW.WriteString("E sym_fts_docsize\n")
}

func tokenCount(s string) int {
	n := 0
	in := false
	for _, r := range s {
		if unicode.IsLetter(r) || unicode.IsDigit(r) {
			_ = foldRune(r)
			in = true
			continue
		}
		if in {
			n++
			in = false
		}
	}
	if in {
		n++
	}
	return n
}

func foldRune(r rune) rune {
	if r < 128 {
		if r >= 'A' && r <= 'Z' {
			return r + 32
		}
		return r
	}
	if d, ok := diacritics[r]; ok {
		return d
	}
	if r >= 0x0300 && r <= 0x036F {
		return 0
	}
	l := unicode.ToLower(r)
	return l
}

var diacritics = map[rune]rune{
	'À': 'A', 'Á': 'A', 'Â': 'A', 'Ã': 'A', 'Ä': 'A', 'Å': 'A', 'Æ': 'A',
	'Ç': 'C', 'È': 'E', 'É': 'E', 'Ê': 'E', 'Ë': 'E', 'Ì': 'I', 'Í': 'I',
	'Î': 'I', 'Ï': 'I', 'Ð': 'D', 'Ñ': 'N', 'Ò': 'O', 'Ó': 'O', 'Ô': 'O',
	'Õ': 'O', 'Ö': 'O', 'Ø': 'O', 'Ù': 'U', 'Ú': 'U', 'Û': 'U', 'Ü': 'U',
	'Ý': 'Y', 'Þ': 'P', 'ß': 's', 'à': 'a', 'á': 'a', 'â': 'a', 'ã': 'a',
	'ä': 'a', 'å': 'a', 'æ': 'a', 'ç': 'c', 'è': 'e', 'é': 'e', 'ê': 'e',
	'ë': 'e', 'ì': 'i', 'í': 'i', 'î': 'i', 'ï': 'i', 'ð': 'd', 'ñ': 'n',
	'ò': 'o', 'ó': 'o', 'ô': 'o', 'õ': 'o', 'ö': 'o', 'ø': 'o', 'ù': 'u',
	'ú': 'u', 'û': 'u', 'ü': 'u', 'ý': 'y', 'þ': 'p', 'ÿ': 'y',
}

func svarint(v int) []byte {
	var tmp [9]byte
	n := 0
	u := uint64(v)
	for {
		tmp[n] = byte(u & 0x7f)
		n++
		u >>= 7
		if u == 0 {
			break
		}
	}
	for i := 0; i < n-1; i++ {
		tmp[i] |= 0x80
	}
	out := make([]byte, n)
	for i := 0; i < n; i++ {
		out[i] = tmp[n-1-i]
	}
	return out
}

func reprBytes(b []byte) string {
	quote := byte('\'')
	hasSingle, hasDouble := false, false
	for _, c := range b {
		if c == '\'' {
			hasSingle = true
		}
		if c == '"' {
			hasDouble = true
		}
	}
	if hasSingle && !hasDouble {
		quote = '"'
	}
	var sb strings.Builder
	sb.WriteByte('b')
	sb.WriteByte(quote)
	for i, c := range b {
		if c >= 0x20 && c < 0x7F {
			if c == quote && i+1 < len(b) && b[i+1] == quote {
				sb.WriteByte('\\')
				sb.WriteByte(quote)
				continue
			}
			sb.WriteByte(c)
			continue
		}
		switch c {
		case '\t':
			sb.WriteString(`\t`)
		case '\n':
			sb.WriteString(`\n`)
		case '\r':
			sb.WriteString(`\r`)
		case '\\':
			sb.WriteString(`\\`)
		default:
			fmt.Fprintf(&sb, `\x%02x`, c)
		}
	}
	sb.WriteByte(quote)
	return sb.String()
}

var _ = bufio.NewWriter

func build(g *Graph, root string, opts runOpts, quiet bool) int {

	// No GC target is set here. This used to raise it to 200, which paid when
	// a run made three times the garbage it does now; measured on PostgreSQL
	// after the allocation work, the default is exactly as fast and peaks
	// 380 MB lower -- and overriding it also overrode the user's GOGC.
	abs, _ := filepath.Abs(root)
	g.fileByRel = map[string]int32{}
	g.byName = map[string][]int32{}
	g.Meta = map[string]string{}

	if !quiet {
		fmt.Fprintln(os.Stderr, "  parser: C23 lexer + hide-set preprocessor + recursive-descent parser (stdlib only)")
	}
	tDisc := time.Now()
	parseIDs := discover(abs, g, opts)
	for i := range g.Files {
		g.fileByRel[g.Files[i].Path()] = g.Files[i].ID
	}

	g.FilesParsed = int32(len(parseIDs))
	if !quiet {
		fmt.Fprintf(os.Stderr, "  %d c files discovered in %.1fs\n", len(parseIDs),
			time.Since(tDisc).Seconds())
	}

	ppSrcMu.Lock()
	ppSrcCache = map[string][]byte{}
	treePaths := make([]string, 0, len(g.Files))
	for i := range g.Files {
		full := filepath.Join(abs, filepath.FromSlash(g.Files[i].Path()))
		treePaths = append(treePaths, full)
		if data := source[i]; len(data) > 0 {
			ppSrcCache[full] = data
		}
	}
	ppSrcMu.Unlock()
	buildTreeIdx(treePaths)
	ppTreeRoot = abs
	for i := range source {
		source[i] = nil
	}

	e := newExtractor(g)
	t1 := time.Now()
	step := max(len(parseIDs)/4, 1)
	outs := e.runC23(abs, g, parseIDs, quiet, step)
	lexCache.Clear()
	lexCacheToks.Store(0)
	dropTokBufs()
	e.reserve(g, outs)
	for i, o := range outs {
		if o == nil {
			continue
		}
		e.merge(g, o)
		outs[i] = nil
		if !quiet && (i+1)%step == 0 {
			fmt.Fprintf(os.Stderr, "  ... %d/%d files\n", i+1, len(parseIDs))
		}
	}
	if !quiet {
		fmt.Fprintf(os.Stderr, "  %d symbols parsed in %.1fs\n", len(g.Symbols),
			time.Since(t1).Seconds())
	}

	nres := e.resolveImportTargets(abs)
	t2 := time.Now()
	e.resolveCalls()
	if !quiet {
		fmt.Fprintf(os.Stderr, "  call graph built in %.1fs\n", time.Since(t2).Seconds())
	}

	collapseHazards(g)

	nimp := 0
	for i := range g.Imports {
		if g.Imports[i].HasTargetID {
			nimp++
		}
	}
	g.Meta["imports_resolved"] = itoa(nimp) + " of " + itoa(len(g.Imports)) +
		" import rows point at a file in this tree (" + itoa(nres) +
		" resolved by path here)"
	parseManifests(abs, g)

	t3 := time.Now()
	e.materialize()
	if !quiet {
		fmt.Fprintf(os.Stderr, "  aggregates materialized in %.1fs\n", time.Since(t3).Seconds())
	}
	t4 := time.Now()
	e.postBuild()
	if !quiet {
		fmt.Fprintf(os.Stderr, "  indexed in %.1fs\n", time.Since(t4).Seconds())
	}

	g.Meta["schema_version"] = "2"
	g.Meta["lang"] = "c"
	g.Meta["target"] = target
	g.Meta["root"] = abs
	g.Meta["parse_mode"] = "c23-ast"
	g.Meta["parser"] = "C23 lexer + hide-set preprocessor + recursive-descent parser (stdlib only)"
	g.Meta["files_parsed"] = itoa(len(parseIDs))
	g.Meta["files_failed"] = itoa(int(g.FilesFailed))
	g.Meta["grammar_note"] = grammarNote
	g.Meta["layout_model"] = "LP64: pointer 8/8, long 8, int 4; sizes reported only where exact=1"
	g.Meta["files_skipped"] = "big=" + itoa(int(g.FilesSkippedBig)) +
		" special=" + itoa(int(g.FilesSkippedSpecial)) +
		" escaping_symlink=" + itoa(int(g.FilesSkippedEscape)) +
		" denied=" + itoa(int(g.FilesSkippedDenied)) +
		" walk_errors=" + itoa(int(g.WalkErrors))
	buildIndex(g)
	return len(parseIDs)
}

func collapseHazards(g *Graph) {
	idx := make(map[[2]any]int, len(g.Hazards))
	out := g.Hazards[:0]
	for _, h := range g.Hazards {
		k := [2]any{h.SymbolID, h.Pattern()}
		if j, ok := idx[k]; ok {
			out[j].N += h.N
			continue
		}
		idx[k] = len(out)
		out = append(out, h)
	}
	g.Hazards = out
}

const grammarNote = "no compiler frontend and no third-party package: C23 " +
	"(ISO/IEC 9899:2024) is analysed by a real preprocessor (hide-set macro " +
	"expansion, conditionals, include graph) and a recursive-descent parser " +
	"producing a pointer-free flat AST. Race-surface and guard counters keep " +
	"their text heuristics, fed exact AST body spans."

type edgeKey struct{ c, d int32 }

// includes reports whether file `from` reaches file `to` through #include,
// directly or through a chain. Both arms of an #if are followed: the include
// table records every directive, taken or not.
func (e *extractor) includes(from, to int32) bool {
	if e.incAdj == nil {
		e.incAdj = map[int32][]int32{}
		for i := range e.g.Imports {
			im := &e.g.Imports[i]
			if im.HasTargetID && im.TargetID != im.FileID {
				e.incAdj[im.FileID] = append(e.incAdj[im.FileID], im.TargetID)
			}
		}
		e.incReach = map[int32]map[int32]bool{}
	}
	r, ok := e.incReach[from]
	if !ok {
		r = map[int32]bool{}
		stack := append([]int32(nil), e.incAdj[from]...)
		for len(stack) > 0 {
			f := stack[len(stack)-1]
			stack = stack[:len(stack)-1]
			if r[f] {
				continue
			}
			r[f] = true
			stack = append(stack, e.incAdj[f]...)
		}
		e.incReach[from] = r
	}
	return r[to]
}

// pickCallee chooses which definition of name a call in file fid reaches, by
// the rules the linker uses, and returns -1 when those rules do not decide:
//
//  1. a definition in the calling file
//  2. a static definition in a file the caller #includes (a header's
//     `static inline`, or a .c pulled in as a backend)
//  3. the one definition with external linkage -- or, of several, the one in
//     the caller's own module, or the one written against the prototype the
//     caller sees (pickByDecl)
//
// A static function in some OTHER translation unit is never a candidate: the
// caller cannot name it. Taking the first definition regardless, as this used
// to, gave one file's `static parseOptions` every call made to its namesakes
// and left the namesakes looking dead.
func (e *extractor) pickCallee(name string, fid, mid int32) int {
	files := e.fnByNameF[name]
	for x, f := range files {
		if f == fid {
			return x
		}
	}
	stat := e.fnByNameS[name]
	mods := e.fnByNameM[name]
	ext, extN, extMod, extModN := -1, 0, -1, 0
	inc := -1
	for x, f := range files {
		if stat[x] {
			if e.includes(fid, f) {
				// several included files defining it are the #if-selected
				// backends of one interface (ae_epoll.c / ae_kqueue.c): the
				// call reaches whichever was compiled in, so it has an edge
				// to each rather than to the alphabetically first
				if inc < 0 {
					inc = x
				} else {
					e.pickMore = append(e.pickMore, x)
				}
			}
			continue
		}
		if extN == 0 {
			ext = x
		}
		extN++
		if mods[x] == mid {
			if extModN == 0 {
				extMod = x
			}
			extModN++
		}
	}
	switch {
	case inc >= 0:
		return inc
	case extN == 1:
		return ext
	case extModN == 1:
		return extMod
	}
	if extN > 1 {
		if x := e.pickByDecl(name, fid, files, stat); x >= 0 {
			e.declPickN++
			return x
		}
		e.ambigN++
	}
	return -1
}

// pickByDecl settles a name with several external definitions the way the
// compiler ties a call to one: through the prototype. The caller sees the
// prototype in some header; a definition written against that same header is
// the one the call was compiled for. PostgreSQL defines pfree twice, for the
// backend under utils/palloc.h and for frontends under common/fe_memutils.h,
// and no caller includes both.
//
// It answers only when exactly one definition shares a prototype with the
// caller. Two #if-selected implementations behind one header (cryptohash.c,
// cryptohash_openssl.c) both do, and stay unresolved -- which is the truth.
func (e *extractor) pickByDecl(name string, fid int32, files []int32, stat []bool) int {
	if e.declFiles == nil {
		e.declFiles = map[string][]int32{}
		for i := range e.g.Decls {
			d := &e.g.Decls[i]
			nm := d.Name()
			if fs := e.declFiles[nm]; len(fs) == 0 || fs[len(fs)-1] != d.FileID {
				e.declFiles[nm] = append(fs, d.FileID)
			}
		}
	}
	pick := -1
	for _, d := range e.declFiles[name] {
		if d != fid && !e.includes(fid, d) {
			continue
		}
		for x, f := range files {
			if stat[x] || (f != d && !e.includes(f, d)) {
				continue
			}
			if pick >= 0 && pick != x {
				return -1
			}
			pick = x
		}
	}
	return pick
}

func (e *extractor) resolveCalls() {
	g := e.g
	edges := map[edgeKey]*Edge{}
	order := []edgeKey{}
	callSites := map[[3]int32]bool{}
	csOrder := [][3]int32{}
	unres := map[[2]any]*Unresolved{}
	unresOrder := [][2]any{}
	macroUses := map[int32]int32{}
	ext := map[int32]int32{}
	decl := map[int32]int32{}
	mac := map[int32]int32{}
	var allocHz []hazardW

	for i, name := range e.pendName {
		caller := e.pendSID[i]
		fid := e.pendFID[i]
		mid := e.pendMID[i]
		lines := e.pendLine[i]
		cnt := int32(len(lines))
		line := lines[0]
		ti := -1
		cands, ok := e.fnByName[name]
		if ok {
			e.pickMore = e.pickMore[:0]
			ti = e.pickCallee(name, fid, mid)
			if ti < 0 {
				e.pickMore = e.pickMore[:0]
			}
		}
		for pick := -1; ti >= 0 && pick < len(e.pickMore); pick++ {
			tx := ti
			if pick >= 0 {
				tx = e.pickMore[pick]
			}
			target := cands[tx]
			tf := e.fnByNameF[name][tx]
			tm := e.fnByNameM[name][tx]
			for _, site := range lines {
				k := edgeKey{caller, target}
				ed, ok := edges[k]
				if !ok {
					ed = &Edge{CallerID: caller, CalleeID: target, NCalls: 1,
						SameFile: b2i(tf == fid), SameModule: b2i(tm == mid),
						IsSelf: b2i(caller == target)}
					edges[k] = ed
					order = append(order, k)
				} else {
					ed.NCalls++
				}
				ck := [3]int32{caller, target, site}
				if !callSites[ck] {
					callSites[ck] = true
					csOrder = append(csOrder, ck)
				}
			}
		}
		if ti >= 0 {
			e.edgeN += cnt
			if allocNameRe.MatchString(name) {
				allocHz = append(allocHz, hazardW{SymbolID: caller,
					Pattern: jtrunc(name, 80), Category: "alloc", N: cnt,
					FirstLine: line})
			}
			continue
		}
		if msid, ok := e.macroSID[name]; ok {
			mac[caller] += cnt
			macroUses[msid] += cnt
			e.macroN += cnt
			continue
		}
		// Defined in this tree, but linkage did not say which definition: that
		// is a call the graph could not follow, not a call that left the tree.
		if ok {
			nm := jtrunc(name, 160)
			k := [2]any{caller, nm}
			if u, seen := unres[k]; seen {
				u.N += cnt
			} else {
				u = &Unresolved{}
				cgZeroRow(unsafe.Pointer(u), unsafe.Sizeof(*u))
				u.CallerID = caller
				u.name = cgPut(nm)
				u.N = cnt
				u.FirstLine = line
				unres[k] = u
				unresOrder = append(unresOrder, k)
			}
			e.unresN += cnt
			continue
		}
		if e.isExternal(name) {
			ext[caller] += cnt
			e.externN += cnt
			continue
		}
		if e.declared[name] {
			ext[caller] += cnt
			decl[caller] += cnt
			e.externN += cnt
			e.declN += cnt
			continue
		}
		nm := jtrunc(name, 160)
		k := [2]any{caller, nm}
		u, ok := unres[k]
		if !ok {
			u = &Unresolved{}
			cgZeroRow(unsafe.Pointer(u), unsafe.Sizeof(*u))
			u.CallerID = caller
			u.name = cgPut(nm)
			u.N = cnt
			u.FirstLine = line
			unres[k] = u
			unresOrder = append(unresOrder, k)
		} else {
			u.N += cnt
		}
		e.unresN += cnt
	}

	for _, h := range allocHz {
		g.Hazards = append(g.Hazards, Hazard{})
		hz := &g.Hazards[len(g.Hazards)-1]
		cgZeroRow(unsafe.Pointer(hz), unsafe.Sizeof(*hz))
		hz.SymbolID = h.SymbolID
		hz.pattern = cgPut(h.Pattern)
		hz.category = cgPut(h.Category)
		hz.N = h.N
		hz.FirstLine = h.FirstLine
	}
	for i := range g.Edges {
		_ = i
	}
	for _, k := range order {
		g.Edges = append(g.Edges, *edges[k])
	}
	for _, k := range csOrder {
		g.Callsites = append(g.Callsites, Callsite{k[0], k[1], k[2]})
	}
	for _, k := range unresOrder {
		g.Unres = append(g.Unres, *unres[k])
	}
	for _, sid := range sortedIDs(ext) {
		g.rw(int(sid - 1)).NExternalCalls += ext[sid]
	}
	for _, sid := range sortedIDs(mac) {
		g.rw(int(sid - 1)).NMacroCalls += mac[sid]
	}

	for i := range g.Macros {
		if n, ok := macroUses[g.Macros[i].SymbolID]; ok {
			g.Macros[i].NUses = n
		}
	}
	total := e.edgeN + e.macroN + e.externN + e.unresN
	pct := int32(0)
	if total > 0 {
		pct = 100 * (total - e.unresN) / total
	}
	den := max(total, 1)
	g.Meta["calls_resolved"] = itoa(int(e.edgeN)) + " in-tree (" +
		itoa(int(e.declPickN)) + " callers matched to one of several definitions by a shared prototype) / " +
		itoa(int(e.macroN)) + " via macro / " + itoa(int(e.externN)) +
		" external (" + itoa(int(e.declN)) +
		" of them declared-here-defined-elsewhere) / " +
		itoa(int(e.unresN)) + " unresolved (" + itoa(int(e.ambigN)) +
		" names defined more than once with external linkage) -- " + itoa(int(pct)) + "% of " +
		itoa(int(den)) + " call sites accounted for"
}

func (e *extractor) isExternal(name string) bool {
	if libcKnown[name] {
		return true
	}
	for _, p := range builtinPrefixes {
		if strings.HasPrefix(name, p) {
			return true
		}
	}
	return intrinsicName(name)
}

func (e *extractor) resolveImportTargets(root string) int {
	g := e.g
	byPath := make(map[string]int32, len(g.Files)*2)
	for i := range g.Files {
		p := filepath.ToSlash(g.Files[i].Path())
		if _, ok := byPath[p]; !ok {
			byPath[p] = g.Files[i].ID
		}
		stem := p
		if j := strings.LastIndexByte(p, '.'); j > 0 {
			stem = p[:j]
		}
		if _, ok := byPath[stem]; !ok {
			byPath[stem] = g.Files[i].ID
		}
	}
	look := func(cand string) int32 {
		cand = strings.Trim(cand, "/")
		if cand == "" {
			return 0
		}
		if v, ok := byPath[cand]; ok {
			return v
		}
		for _, idx := range importIndexes {
			if v, ok := byPath[cand+"/"+idx]; ok {
				return v
			}
		}
		return 0
	}
	n := 0
	for i := range g.Imports {
		im := &g.Imports[i]
		if im.HasTargetID {
			continue
		}
		target := im.Target()
		if target == "" {
			continue
		}
		here := pathDir(g.Files[im.FileID-1].Path())
		var hit int32
		t := target
		if strings.HasPrefix(t, ".") {
			nUp := len(t) - len(strings.TrimLeft(t, "."))
			rest := t
			if !strings.Contains(t, "/") {
				rest = strings.ReplaceAll(t[nUp:], ".", "/")
			} else {
				rest = strings.TrimLeft(t, "./")
			}
			base := here
			for k := 0; k < maxi(0, nUp-1); k++ {
				base = pathDir(base)
				if base == "." {
					break
				}
			}
			if base != "" {
				hit = look(base + "/" + rest)
			} else {
				hit = look(rest)
			}
		} else {
			hit = look(strings.ReplaceAll(t, ".", "/"))
			if hit == 0 {
				hit = look(here + "/" + t)
			}
		}
		if hit != 0 && hit != im.FileID {
			im.TargetID, im.HasTargetID = hit, true
			n++
		}
	}
	return n
}

var importIndexes = []string{"__init__.py", "index.ts", "index.tsx",
	"index.js", "index.mjs", "mod.rs", "lib.rs"}

func parseManifests(root string, g *Graph) {
	bases := []string{root}
	ents, _ := os.ReadDir(root)
	names := make([]string, 0, len(ents))
	for _, e := range ents {
		names = append(names, e.Name())
	}
	sort.Strings(names)
	if len(names) > 64 {
		names = names[:64]
	}
	for _, d := range names {
		p := filepath.Join(root, d)
		if st, err := os.Stat(p); err == nil && st.IsDir() {
			bases = append(bases, p)
		}
	}
	seen := 0
	for _, base := range bases {
		for _, nm := range []string{"Makefile", "makefile", "GNUmakefile"} {
			p := filepath.Join(base, nm)
			st, err := os.Stat(p)
			if err != nil || !st.Mode().IsRegular() {
				continue
			}
			seen++
			rel, _ := filepath.Rel(root, p)
			rel = filepath.ToSlash(rel)
			data, err := os.ReadFile(p)
			if err != nil {
				break
			}
			cont, start := "", 0
			for lnNo0, ln := range splitLines(string(data)) {
				lnNo := lnNo0 + 1
				ln = strings.TrimRight(ln, "\r")
				if cont != "" {
					ln = cont + " " + strings.TrimLeft(ln, " \t")
					cont = ""
				}
				if strings.HasSuffix(ln, "\\") {
					cont = ln[:len(ln)-1]
					if start == 0 {
						start = lnNo
					}
					continue
				}
				if r := matchMakeRule(ln); r != "" {
					pre := ln[strings.Index(ln, ":")+1:]
					g.MkRules = append(g.MkRules, MakefileRule{})
					mr := &g.MkRules[len(g.MkRules)-1]
					cgZeroRow(unsafe.Pointer(mr), unsafe.Sizeof(*mr))
					mr.path = cgPut(rel)
					mr.rule = cgPut(r)
					mr.Line = int32(cmpOr(start, lnNo))
					mr.NObjs = int32(countMakeObj(pre))
					mr.NSrcs = int32(countMakeSrc(pre))
					mr.UsesAr = b2i(strings.Contains(pre, ".a"))
					start = 0
					continue
				}
				if v, val := matchMakeVar(ln); v != "" {
					objs := countMakeObj(val)
					srcs := countMakeSrc(val)
					if objs > 0 || srcs > 0 {
						g.MkRules = append(g.MkRules, MakefileRule{})
						mr := &g.MkRules[len(g.MkRules)-1]
						cgZeroRow(unsafe.Pointer(mr), unsafe.Sizeof(*mr))
						mr.path = cgPut(rel)
						mr.rule = cgPut(v + " =")
						mr.Line = int32(cmpOr(start, lnNo))
						mr.NObjs = int32(objs)
						mr.NSrcs = int32(srcs)
						mr.UsesAr = b2i(strings.Contains(val, ".a"))
					}
				}
				start = 0
			}
			break
		}
	}
	g.MakefilesRead = int32(seen)
	g.Meta["makefiles_read"] = itoa(seen)
}

func cmpOr(a, b int) int {
	if a != 0 {
		return a
	}
	return b
}

func matchMakeRule(ln string) string {
	bb := []byte(ln)
	k := 0
	for k < len(bb) && isMakeNameByte(bb[k]) {
		k++
	}
	if k == 0 {
		return ""
	}
	m := k
	for m < len(bb) && (bb[m] == ' ' || bb[m] == '\t') {
		m++
	}
	if m >= len(bb) || bb[m] != ':' {
		return ""
	}
	if m+1 >= len(bb) || bb[m+1] == '=' {
		return ""
	}
	return ln[:k]
}

func isMakeNameByte(c byte) bool {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		(c >= '0' && c <= '9') || c == '_' || c == '.' || c == '/' ||
		c == '$' || c == '(' || c == ')' || c == '%' || c == '-'
}

func matchMakeVar(ln string) (string, string) {
	if ln == "" || !isIdentStart(ln[0]) {
		return "", ""
	}
	k := identEndStr(ln, 0)
	m := k
	for m < len(ln) && (ln[m] == ' ' || ln[m] == '\t') {
		m++
	}
	if m < len(ln) && (ln[m] == ':' || ln[m] == '+' || ln[m] == '?') {
		m++
	}
	if m >= len(ln) || ln[m] != '=' {
		return "", ""
	}
	m++
	for m < len(ln) && (ln[m] == ' ' || ln[m] == '\t') {
		m++
	}
	return ln[:k], ln[m:]
}

func countMakeObj(s string) int { return countMakeExt(s, 'o') }

func countMakeSrc(s string) int {
	n, _ := countMakeExt2(s, 'c')
	return n
}

func makeObjClass(c byte) bool {
	return isWordByte(c) || c == '/' || c == '$' || c == '(' || c == ')' ||
		c == '{' || c == '}' || c == '.' || c == '-'
}

func makeSrcClass(c byte) bool {
	return isWordByte(c) || c == '/' || c == '.' || c == '-'
}

func countMakeExt(s string, ext byte) int {
	n, _ := countMakeExt2(s, ext)
	return n
}

func countMakeExt2(s string, ext byte) (int, bool) {
	bb := []byte(s)
	inClass := makeObjClass
	if ext == 'c' {
		inClass = makeSrcClass
	}
	n := 0
	i := 0
	for i < len(bb) {
		if !inClass(bb[i]) {
			i++
			continue
		}
		greedy := i
		for greedy < len(bb) && inClass(bb[greedy]) {
			greedy++
		}
		found := -1
		for j := greedy - 1; j > i; j-- {
			if bb[j] != '.' || j+1 >= len(bb) || bb[j+1] != ext {
				continue
			}
			if j+2 < len(bb) && isWordByte(bb[j+2]) {
				continue
			}
			found = j
			break
		}
		if found < 0 {
			i = greedy
			continue
		}
		n++
		i = found + 2
	}
	return n, true
}

func (e *extractor) materialize() {
	g := e.g

	byCat := map[int32]map[string]int32{}
	for _, h := range g.Hazards {
		m := byCat[h.SymbolID]
		if m == nil {
			m = map[string]int32{}
			byCat[h.SymbolID] = m
		}
		m[h.Category()] += h.N
	}

	fanIn := map[int32]int32{}
	fanOut := map[int32]int32{}
	nsites := map[int32]int32{}
	selfCall := map[int32]bool{}
	for _, ed := range g.Edges {
		if ed.IsSelf == 0 {
			fanOut[ed.CallerID]++
			fanIn[ed.CalleeID]++
		} else {
			selfCall[ed.CallerID] = true
		}
	}
	for _, c := range g.Callsites {
		nsites[c.CalleeID]++
	}
	unresN := map[int32]int32{}
	for _, u := range g.Unres {
		unresN[u.CallerID] += u.N
	}
	hazN := map[int32]int32{}
	for _, h := range g.Hazards {
		hazN[h.SymbolID] += h.N
	}
	uniq := map[int32]int32{}
	for _, ed := range g.Edges {
		uniq[ed.CallerID]++
	}

	freeN := map[int32]int32{}
	allocN := map[int32]int32{}
	for _, h := range g.Hazards {
		if h.Category() != "alloc" {
			continue
		}
		if h.Pattern() == "free" || strings.HasSuffix(asciiLower(h.Pattern()), "free") {
			freeN[h.SymbolID] += h.N
		} else {
			allocN[h.SymbolID] += h.N
		}
	}
	for i := range g.Symbols {
		s := g.Symbols[i]
		s.FanOut = fanOut[s.ID]
		s.FanIn = fanIn[s.ID]
		s.NCallsites = nsites[s.ID]
		if selfCall[s.ID] {
			s.IsRecursive = 1
		}
		s.NUnresolvedCalls = unresN[s.ID]
		if m := byCat[s.ID]; m != nil {
			s.NMemory = m["memory"]
			s.NAlloc = m["alloc"]
			s.NIO = m["io"]
			s.NStdio = m["stdio"]
			s.NExec = m["exec"]
			s.NLibm = m["libm"]
			s.NInteger = m["integer"]
			s.NConcurrency = m["concurrency"]
			s.NReentrancy = m["reentrancy"]
		}
		// rw gives the symbol a rare row if it has none; writing a zero
		// through it gave one to every typedef and struct in the tree
		if n := freeN[s.ID]; n != g.rv(s).NFree {
			g.rw(i).NFree = n
		}
		s.NAlloc = allocN[s.ID]
	}

	for i := range g.Symbols {
		s := g.Symbols[i]
		r := s.Cyclomatic*2 + s.Cognitive + s.MaxNesting*5 +
			s.NMemory*10 + s.NIO*8 + s.NExec*15 + s.NInteger*1 +
			s.NAlloc*2 + s.NConcurrency*3
		if s.IsRecursive == 1 {
			r += 25
		}
		if s.NAlloc > 0 && g.rv(s).NFree == 0 {
			r += 10
		}
		s.RiskScore = r
	}

	type fileAgg struct {
		n, fn, ty, cyclo, maxc, risk int32
	}
	fagg := map[int32]*fileAgg{}
	for i := range g.Symbols {
		s := g.Symbols[i]
		a := fagg[s.FileID]
		if a == nil {
			a = &fileAgg{}
			fagg[s.FileID] = a
		}
		a.n++
		switch s.Kind() {
		case "function", "method", "constructor", "closure":
			a.fn++
		case "class", "struct", "interface", "trait", "enum", "union",
			"record", "protocol", "type", "impl":
			a.ty++
		}
		a.cyclo += s.Cyclomatic
		if s.Cyclomatic > a.maxc {
			a.maxc = s.Cyclomatic
		}
		a.risk += s.RiskScore
	}
	impN := map[int32]int32{}
	for _, im := range g.Imports {
		impN[im.FileID]++
	}
	for i := range g.Files {
		f := &g.Files[i]
		if a := fagg[f.ID]; a != nil {
			f.NSymbols, f.NFunctions, f.NTypes = a.n, a.fn, a.ty
			f.TotalCyclo, f.MaxCyclo, f.TotalRisk = a.cyclo, a.maxc, a.risk
		}
		f.NImports = impN[f.ID]
	}

	type modAgg struct{ n, p, files, sloc int32 }
	magg := map[int32]*modAgg{}
	for i := range g.Symbols {
		s := g.Symbols[i]
		if s.ModuleID == 0 {
			continue
		}
		m := magg[s.ModuleID]
		if m == nil {
			m = &modAgg{}
			magg[s.ModuleID] = m
		}
		m.n++
		m.p += s.IsPublic
	}
	for i := range g.Files {
		f := &g.Files[i]
		if f.ModuleID == 0 {
			continue
		}
		m := magg[f.ModuleID]
		if m == nil {
			m = &modAgg{}
			magg[f.ModuleID] = m
		}
		m.files++
		m.sloc += f.Sloc
	}

	mo, mi := map[int32]map[int32]bool{}, map[int32]map[int32]bool{}
	for _, ed := range g.Edges {
		if ed.CallerID < 1 || ed.CalleeID < 1 {
			continue
		}
		a := g.Symbols[ed.CallerID-1]
		b := g.Symbols[ed.CalleeID-1]
		if a.ModuleID == 0 || b.ModuleID == 0 || a.ModuleID == b.ModuleID {
			continue
		}
		if mo[a.ModuleID] == nil {
			mo[a.ModuleID] = map[int32]bool{}
		}
		mo[a.ModuleID][b.ModuleID] = true
		if mi[b.ModuleID] == nil {
			mi[b.ModuleID] = map[int32]bool{}
		}
		mi[b.ModuleID][a.ModuleID] = true
	}
	for i := range g.Modules {
		m := &g.Modules[i]
		if a := magg[m.ID]; a != nil {
			m.NSymbols, m.NPublic, m.NFiles, m.Sloc = a.n, a.p, a.files, a.sloc
		}
		m.FanOut = int32(len(mo[m.ID]))
		m.FanIn = int32(len(mi[m.ID]))
		if m.FanIn+m.FanOut != 0 {
			m.Instability = float64(m.FanOut) / float64(m.FanIn+m.FanOut)
		}
	}
}

func (e *extractor) reserve(g *Graph, outs []*fileOut) {
	var nSym, nPar, nFld, nLoc, nLit, nMk, nAt, nImp, nHaz, nEn int32
	var nLay, nSS, nDec, nAdd, nSec, nAlc, nMem, nMac, nGlb, nCfg int32
	var nLock, nEv, nAPI, nRare int32
	for _, o := range outs {
		if o == nil {
			continue
		}
		nSym += o.symN
		nRare += o.rareN
		nPar += int32(len(o.params))
		nFld += int32(len(o.fields))
		nLoc += int32(len(o.locals))
		nLit += int32(len(o.literals))
		nMk += int32(len(o.markers))
		nAt += int32(len(o.attrs))
		nImp += int32(len(o.imports))
		nHaz += int32(len(o.hazards))
		nEn += int32(len(o.enums))
		nLay += int32(len(o.layout))
		nSS += int32(len(o.ssize))
		nDec += int32(len(o.decls))
		nAdd += int32(len(o.addrs))
		nSec += int32(len(o.secrets))
		nAlc += int32(len(o.allocs))
		nMem += int32(len(o.memops))
		nMac += int32(len(o.macros))
		nGlb += int32(len(o.globals))
		nCfg += int32(len(o.cfgs))
		nLock += int32(len(o.locks))
		nEv += int32(len(o.evops))
		nAPI += int32(len(o.apiuses))
	}
	g.Symbols = make([]*Symbol, 0, nSym)
	g.symStore = make([]Symbol, 0, nSym)
	g.rare = make([]symRare, 0, nRare)
	g.Params = make([]Param, 0, nPar)
	g.Fields = make([]Field, 0, nFld)
	g.Locals = make([]Local, 0, nLoc)
	g.Literals = make([]Literal, 0, nLit)
	g.Markers = make([]Marker, 0, nMk)
	g.Attrs = make([]Attribute, 0, nAt)
	g.Imports = make([]Import, 0, nImp)
	g.Hazards = make([]Hazard, 0, nHaz)
	g.EnumMem = make([]EnumMember, 0, nEn)
	g.Layout = make([]LayoutRow, 0, nLay)
	g.SSize = make([]StructSize, 0, nSS)
	g.Decls = make([]Declaration, 0, nDec)
	g.Addr = make([]AddrTaken, 0, nAdd)
	g.Secrets = make([]SecretCandidate, 0, nSec)
	g.Allocs = make([]AllocSite, 0, nAlc)
	g.Memops = make([]Memop, 0, nMem)
	g.Macros = make([]Macro, 0, nMac)
	g.Globals = make([]Global, 0, nGlb)
	g.Cfgs = make([]ConfigBlock, 0, nCfg)
	g.Locks = make([]LockRow, 0, nLock)
	g.EvOps = make([]EventOp, 0, nEv)
	g.APIUses = make([]APIUse, 0, nAPI)
}

func (e *extractor) merge(g *Graph, o *fileOut) {
	g.ParseMsgs = append(g.ParseMsgs, o.perr...)
	base := int32(len(g.Symbols))
	syms := o.symA.at(o.symLo, o.symN)
	for i := range syms {
		w := &syms[i]
		g.symStore = append(g.symStore, Symbol{})
		d := &g.symStore[len(g.symStore)-1]
		cgZeroRow(unsafe.Pointer(d), unsafe.Sizeof(*d))
		d.ID = w.ID + base
		d.FileID = w.FileID
		d.ModuleID = w.ModuleID
		d.name = cgPut(w.Name)
		d.qualName = cgPut(w.QualName)
		d.kind = cgPut(w.Kind)
		d.LineStart = w.LineStart
		d.signature = cgPut(w.Signature)
		d.returnType = cgPut(w.ReturnType)
		d.HasSignature = w.HasSignature
		d.HasReturnType = w.HasReturnType
		d.NParams = w.NParams
		d.IsPublic = w.IsPublic
		d.IsStatic = w.IsStatic
		d.IsAbstract = w.IsAbstract
		d.IsOverride = w.IsOverride
		d.IsTest = w.IsTest
		d.IsEntrypoint = w.IsEntrypoint
		d.IsGenerated = w.IsGenerated
		d.Sloc = w.Sloc
		d.NCommentLines = w.NCommentLines
		d.HasDoc = w.HasDoc
		d.Cyclomatic = w.Cyclomatic
		d.Cognitive = w.Cognitive
		d.MaxNesting = w.MaxNesting
		d.NTokens = w.NTokens
		d.NOperators = w.NOperators
		d.NOperands = w.NOperands
		d.NDistinctOperators = w.NDistinctOperators
		d.NDistinctOperands = w.NDistinctOperands
		d.NLoops = w.NLoops
		d.NBranches = w.NBranches
		d.NReturns = w.NReturns
		d.NSwitch = w.NSwitch
		d.NCases = w.NCases
		d.NLabels = w.NLabels
		d.NGotos = w.NGotos
		d.MaxLoopDepth = w.MaxLoopDepth
		d.CallInLoop = w.CallInLoop
		d.AllocInLoop = w.AllocInLoop
		d.IOInLoop = w.IOInLoop
		d.LockInLoop = w.LockInLoop
		d.BranchInLoop = w.BranchInLoop
		d.NLocals = w.NLocals
		d.NCmp = w.NCmp
		d.NArith = w.NArith
		d.NShift = w.NShift
		d.NFloatLit = w.NFloatLit
		d.NMagic = w.NMagic
		d.NNullCheck = w.NNullCheck
		d.NCalls = w.NCalls
		d.NDynamicCalls = w.NDynamicCalls
		d.NUnresolvedCalls = w.NUnresolvedCalls
		d.FanIn = w.FanIn
		d.FanOut = w.FanOut
		d.NCallsites = w.NCallsites
		d.IsRecursive = w.IsRecursive
		d.RiskScore = w.RiskScore
		d.NMemory = w.NMemory
		d.NAlloc = w.NAlloc
		d.NIO = w.NIO
		d.NStdio = w.NStdio
		d.NExec = w.NExec
		d.NLibm = w.NLibm
		d.NInteger = w.NInteger
		d.NConcurrency = w.NConcurrency
		d.NReentrancy = w.NReentrancy
		d.IsInline = w.IsInline
		d.IsVariadic = w.IsVariadic
		d.NPtrLocals = w.NPtrLocals
		d.NDeref = w.NDeref
		d.NCast = w.NCast
		d.NSizeof = w.NSizeof
		d.NIntrinsic = w.NIntrinsic
		d.NAtomic = w.NAtomic
		d.NRestrict = w.NRestrict
		d.NLikely = w.NLikely
		d.NBuiltin = w.NBuiltin
		if old := w.rare; old >= 0 {
			g.rare = append(g.rare, o.symA.rare[old])
			d.rare = int32(len(g.rare) - 1)
		} else {
			d.rare = -1
		}
		g.Symbols = append(g.Symbols, d)
	}

	off := func(ids []int32) {
		for i := range ids {
			ids[i] += base
		}
	}
	for i := range o.params {
		o.params[i].SymbolID += base
	}
	for i := range o.fields {
		o.fields[i].SymbolID += base
	}
	for i := range o.locals {
		o.locals[i].SymbolID += base
	}
	for i := range o.literals {
		o.literals[i].ID = int32(len(g.Literals) + i + 1)
		o.literals[i].SymbolID += base
	}
	for i := range o.markers {
		o.markers[i].ID = int32(len(g.Markers) + i + 1)
	}
	for i := range o.attrs {
		o.attrs[i].ID = int32(len(g.Attrs) + i + 1)
		o.attrs[i].SymbolID += base
	}
	for i := range o.hazards {
		o.hazards[i].SymbolID += base
	}
	for i := range o.enums {
		o.enums[i].SymbolID += base
	}
	for i := range o.layout {
		o.layout[i].SymbolID += base
	}
	for i := range o.ssize {
		o.ssize[i].SymbolID += base
	}
	for i := range o.decls {
		o.decls[i].ID = int32(len(g.Decls) + i + 1)
	}
	for i := range o.addrs {
		o.addrs[i].ID = int32(len(g.Addr) + i + 1)
		if o.addrs[i].HasSym {
			o.addrs[i].SymbolID += base
		}
	}
	for i := range o.secrets {
		o.secrets[i].ID = int32(len(g.Secrets) + i + 1)
		o.secrets[i].SymbolID += base
	}
	for i := range o.allocs {
		o.allocs[i].ID = int32(len(g.Allocs) + i + 1)
		o.allocs[i].SymbolID += base
	}
	for i := range o.memops {
		o.memops[i].ID = int32(len(g.Memops) + i + 1)
		o.memops[i].SymbolID += base
	}
	for i := range o.macros {
		o.macros[i].SymbolID += base
	}
	for i := range o.locks {
		o.locks[i].ID = int32(len(g.Locks) + i + 1)
		o.locks[i].SymbolID += base
	}
	for i := range o.evops {
		o.evops[i].ID = int32(len(g.EvOps) + i + 1)
		o.evops[i].SymbolID += base
	}
	for i := range o.apiuses {
		o.apiuses[i].ID = int32(len(g.APIUses) + i + 1)
		o.apiuses[i].SymbolID += base
	}
	off(o.pendSID)
	e.pendSID = append(e.pendSID, o.pendSID...)
	e.pendFID = append(e.pendFID, o.pendFID...)
	e.pendMID = append(e.pendMID, o.pendMID...)
	e.pendName = append(e.pendName, o.pendName...)
	e.pendLine = append(e.pendLine, o.pendLine...)
	for i, m := range o.macros {
		if _, ok := e.macroSID[o.macroNames[i]]; !ok {
			e.macroSID[o.macroNames[i]] = m.SymbolID
		}
	}
	for _, d := range o.decls {
		e.declared[d.Name] = true
	}
	for _, fs := range o.funcs {
		e.fnByName[fs.name] = append(e.fnByName[fs.name], fs.sid+base)
		e.fnByNameF[fs.name] = append(e.fnByNameF[fs.name], fs.fid)
		e.fnByNameM[fs.name] = append(e.fnByNameM[fs.name], fs.mid)
		e.fnByNameS[fs.name] = append(e.fnByNameS[fs.name], fs.static)
	}

	for i := range o.params {
		w := &o.params[i]
		g.Params = append(g.Params, Param{})
		d := &g.Params[len(g.Params)-1]
		cgZeroRow(unsafe.Pointer(d), unsafe.Sizeof(*d))
		d.SymbolID = w.SymbolID
		d.Pos = w.Pos
		d.name = cgPut(w.Name)
		d.HasName = w.HasName
		d.typ = cgPut(w.Type)
		d.def = cgPut(w.DefaultValue)
		d.HasDefaultValue = w.HasDefaultValue
		d.IsOptional = w.IsOptional
		d.IsVariadic = w.IsVariadic
		d.IsRef = w.IsRef
		d.IsMutable = w.IsMutable
		d.IsNullable = w.IsNullable
		d.IsGeneric = w.IsGeneric
		d.IsUntyped = w.IsUntyped
		d.TypeDepth = w.TypeDepth
	}
	for i := range o.fields {
		w := &o.fields[i]
		g.Fields = append(g.Fields, Field{})
		d := &g.Fields[len(g.Fields)-1]
		cgZeroRow(unsafe.Pointer(d), unsafe.Sizeof(*d))
		d.SymbolID = w.SymbolID
		d.Ordinal = w.Ordinal
		d.name = cgPut(w.Name)
		d.typ = cgPut(w.Type)
		d.vis = cgPut(w.Visibility)
		d.Line = w.Line
		d.IsStatic = w.IsStatic
		d.IsConst = w.IsConst
		d.IsMutable = w.IsMutable
		d.IsNullable = w.IsNullable
		d.IsCollection = w.IsCollection
		d.IsUntyped = w.IsUntyped
		d.HasDefault = w.HasDefault
		d.TypeDepth = w.TypeDepth
	}
	for i := range o.locals {
		w := &o.locals[i]
		g.Locals = append(g.Locals, Local{})
		d := &g.Locals[len(g.Locals)-1]
		cgZeroRow(unsafe.Pointer(d), unsafe.Sizeof(*d))
		d.SymbolID = w.SymbolID
		d.Ordinal = w.Ordinal
		d.name = cgPut(w.Name)
		d.typ = cgPut(w.Type)
		d.Line = w.Line
		d.IsConst = w.IsConst
		d.IsMutable = w.IsMutable
		d.IsUntyped = w.IsUntyped
		d.HasInit = w.HasInit
		d.InLoop = w.InLoop
		d.ScopeDepth = w.ScopeDepth
	}
	for i := range o.literals {
		w := &o.literals[i]
		g.Literals = append(g.Literals, Literal{})
		d := &g.Literals[len(g.Literals)-1]
		cgZeroRow(unsafe.Pointer(d), unsafe.Sizeof(*d))
		d.ID = w.ID
		d.SymbolID = w.SymbolID
		d.HasSym = w.HasSym
		d.FileID = w.FileID
		d.kind = cgPut(w.Kind)
		d.value = cgPut(w.Value)
		d.Line = w.Line
		d.IsMagic = w.IsMagic
	}
	for i := range o.markers {
		w := &o.markers[i]
		g.Markers = append(g.Markers, Marker{})
		d := &g.Markers[len(g.Markers)-1]
		cgZeroRow(unsafe.Pointer(d), unsafe.Sizeof(*d))
		d.ID = w.ID
		d.FileID = w.FileID
		d.SymbolID = w.SymbolID
		d.HasSym = w.HasSym
		d.kind = cgPut(w.Kind)
		d.Line = w.Line
		d.text = cgPut(w.Text)
	}
	for i := range o.attrs {
		w := &o.attrs[i]
		g.Attrs = append(g.Attrs, Attribute{})
		d := &g.Attrs[len(g.Attrs)-1]
		cgZeroRow(unsafe.Pointer(d), unsafe.Sizeof(*d))
		d.ID = w.ID
		d.SymbolID = w.SymbolID
		d.HasSymbolID = w.HasSymbolID
		d.FileID = w.FileID
		d.name = cgPut(w.Name)
		d.args = cgPut(w.Args)
		d.HasArgs = w.HasArgs
		d.Line = w.Line
	}
	for i := range o.imports {
		w := &o.imports[i]
		g.Imports = append(g.Imports, Import{})
		d := &g.Imports[len(g.Imports)-1]
		cgZeroRow(unsafe.Pointer(d), unsafe.Sizeof(*d))
		d.ID = w.ID
		d.FileID = w.FileID
		d.target = cgPut(w.Target)
		d.TargetID = w.TargetID
		d.HasTargetID = w.HasTargetID
		d.alias = cgPut(w.Alias)
		d.HasAlias = w.HasAlias
		d.kind = cgPut(w.Kind)
		d.Line = w.Line
		d.IsExternal = w.IsExternal
		d.IsRelative = w.IsRelative
		d.IsWildcard = w.IsWildcard
		d.IsTypeOnly = w.IsTypeOnly
		d.IsDynamic = w.IsDynamic
		d.NNames = w.NNames
	}
	for i := range o.hazards {
		w := &o.hazards[i]
		g.Hazards = append(g.Hazards, Hazard{})
		d := &g.Hazards[len(g.Hazards)-1]
		cgZeroRow(unsafe.Pointer(d), unsafe.Sizeof(*d))
		d.SymbolID = w.SymbolID
		d.pattern = cgPut(w.Pattern)
		d.category = cgPut(w.Category)
		d.N = w.N
		d.FirstLine = w.FirstLine
	}
	for i := range o.enums {
		w := &o.enums[i]
		g.EnumMem = append(g.EnumMem, EnumMember{})
		d := &g.EnumMem[len(g.EnumMem)-1]
		cgZeroRow(unsafe.Pointer(d), unsafe.Sizeof(*d))
		d.SymbolID = w.SymbolID
		d.Ordinal = w.Ordinal
		d.name = cgPut(w.Name)
		d.value = cgPut(w.Value)
		d.HasValue = w.HasValue
		d.NFields = w.NFields
	}
	g.Layout = append(g.Layout, o.layout...)
	g.SSize = append(g.SSize, o.ssize...)
	for i := range o.decls {
		w := &o.decls[i]
		g.Decls = append(g.Decls, Declaration{})
		d := &g.Decls[len(g.Decls)-1]
		cgZeroRow(unsafe.Pointer(d), unsafe.Sizeof(*d))
		d.ID = w.ID
		d.FileID = w.FileID
		d.name = cgPut(w.Name)
		d.Line = w.Line
	}
	for i := range o.addrs {
		w := &o.addrs[i]
		g.Addr = append(g.Addr, AddrTaken{})
		d := &g.Addr[len(g.Addr)-1]
		cgZeroRow(unsafe.Pointer(d), unsafe.Sizeof(*d))
		d.ID = w.ID
		d.SymbolID = w.SymbolID
		d.HasSym = w.HasSym
		d.FileID = w.FileID
		d.name = cgPut(w.Name)
		d.Line = w.Line
		d.kind = cgPut(w.Kind)
	}
	for i := range o.secrets {
		w := &o.secrets[i]
		g.Secrets = append(g.Secrets, SecretCandidate{})
		d := &g.Secrets[len(g.Secrets)-1]
		cgZeroRow(unsafe.Pointer(d), unsafe.Sizeof(*d))
		d.ID = w.ID
		d.SymbolID = w.SymbolID
		d.HasSym = w.HasSym
		d.FileID = w.FileID
		d.value = cgPut(w.Value)
		d.Line = w.Line
	}
	for i := range o.allocs {
		w := &o.allocs[i]
		g.Allocs = append(g.Allocs, AllocSite{})
		d := &g.Allocs[len(g.Allocs)-1]
		cgZeroRow(unsafe.Pointer(d), unsafe.Sizeof(*d))
		d.ID = w.ID
		d.SymbolID = w.SymbolID
		d.FileID = w.FileID
		d.fn = cgPut(w.Fn)
		d.sizeExpr = cgPut(w.SizeExpr)
		d.Line = w.Line
	}
	for i := range o.memops {
		w := &o.memops[i]
		g.Memops = append(g.Memops, Memop{})
		d := &g.Memops[len(g.Memops)-1]
		cgZeroRow(unsafe.Pointer(d), unsafe.Sizeof(*d))
		d.ID = w.ID
		d.SymbolID = w.SymbolID
		d.FileID = w.FileID
		d.fn = cgPut(w.Fn)
		d.dst = cgPut(w.Dst)
		d.src = cgPut(w.Src)
		d.sizeArg = cgPut(w.SizeArg)
		d.sizeBuf = cgPut(w.SizeBuf)
		d.dstTail = cgPut(w.DstTail)
		d.Line = w.Line
	}
	for i := range o.macros {
		w := &o.macros[i]
		g.Macros = append(g.Macros, Macro{})
		d := &g.Macros[len(g.Macros)-1]
		cgZeroRow(unsafe.Pointer(d), unsafe.Sizeof(*d))
		d.SymbolID = w.SymbolID
		d.IsFunctionlike = w.IsFunctionlike
		d.NParams = w.NParams
		d.body = cgPut(w.Body)
		d.HasBody = w.HasBody
		d.BodyLen = w.BodyLen
		d.IsMultiline = w.IsMultiline
		d.NUses = w.NUses
	}
	for i := range o.globals {
		w := &o.globals[i]
		g.Globals = append(g.Globals, Global{})
		d := &g.Globals[len(g.Globals)-1]
		cgZeroRow(unsafe.Pointer(d), unsafe.Sizeof(*d))
		d.ID = w.ID
		d.FileID = w.FileID
		d.ModuleID = w.ModuleID
		d.name = cgPut(w.Name)
		d.typ = cgPut(w.Type)
		d.Line = w.Line
		d.IsStatic = w.IsStatic
		d.IsConst = w.IsConst
		d.IsVolatile = w.IsVolatile
		d.IsAtomic = w.IsAtomic
		d.IsArray = w.IsArray
		d.PtrDepth = w.PtrDepth
		d.HasInit = w.HasInit
	}
	for i := range o.cfgs {
		w := &o.cfgs[i]
		g.Cfgs = append(g.Cfgs, ConfigBlock{})
		d := &g.Cfgs[len(g.Cfgs)-1]
		cgZeroRow(unsafe.Pointer(d), unsafe.Sizeof(*d))
		d.ID = w.ID
		d.FileID = w.FileID
		d.directive = cgPut(w.Directive)
		d.expr = cgPut(w.Expr)
		d.Line = w.Line
		d.IsConfig = w.IsConfig
	}
	for i := range o.locks {
		w := &o.locks[i]
		g.Locks = append(g.Locks, LockRow{})
		d := &g.Locks[len(g.Locks)-1]
		cgZeroRow(unsafe.Pointer(d), unsafe.Sizeof(*d))
		d.ID = w.ID
		d.SymbolID = w.SymbolID
		d.FileID = w.FileID
		d.name = cgPut(w.Name)
		d.Line = w.Line
	}
	for i := range o.evops {
		w := &o.evops[i]
		g.EvOps = append(g.EvOps, EventOp{})
		d := &g.EvOps[len(g.EvOps)-1]
		cgZeroRow(unsafe.Pointer(d), unsafe.Sizeof(*d))
		d.ID = w.ID
		d.SymbolID = w.SymbolID
		d.FileID = w.FileID
		d.family = cgPut(w.Family)
		d.fn = cgPut(w.Fn)
		d.args = cgPut(w.Args)
		d.Line = w.Line
	}
	for i := range o.apiuses {
		w := &o.apiuses[i]
		g.APIUses = append(g.APIUses, APIUse{})
		d := &g.APIUses[len(g.APIUses)-1]
		cgZeroRow(unsafe.Pointer(d), unsafe.Sizeof(*d))
		d.ID = w.ID
		d.SymbolID = w.SymbolID
		d.FileID = w.FileID
		d.ns = cgPut(w.NS)
		d.fn = cgPut(w.Fn)
		d.Line = w.Line
	}
}

func (e *extractor) postBuild() {
	g := e.g
	fnNames := map[string]bool{}
	for i := range g.Symbols {
		if g.Symbols[i].Kind() == "function" {
			fnNames[g.Symbols[i].Name()] = true
		}
	}
	pruned := 0
	out := g.Addr[:0]
	for _, a := range g.Addr {
		if !a.HasSym && !fnNames[a.Name()] {
			pruned++
			continue
		}
		out = append(out, a)
	}
	g.Addr = out
	g.AddrPruned = int32(pruned)
	g.Meta["addr_taken_pruned"] = itoa(pruned) +
		" non-function file-scope references dropped"

	e.transitiveFan()
	e.includeCycles()
}

func (e *extractor) transitiveFan() {
	g := e.g
	n := int32(len(g.Symbols)) + 1
	type ek struct{ a, b int32 }
	edges := make([]ek, 0, len(g.Edges))
	has := make([]bool, n)
	for _, ed := range g.Edges {
		if ed.CallerID == ed.CalleeID {
			continue
		}
		edges = append(edges, ek{ed.CallerID, ed.CalleeID})
		has[ed.CallerID] = true
		has[ed.CalleeID] = true
	}
	if len(edges) == 0 {
		return
	}
	nodes := make([]int32, 0, n)
	for v := int32(1); v < n; v++ {
		if has[v] {
			nodes = append(nodes, v)
		}
	}
	start := make([]int32, n+1)
	for _, ed := range edges {
		start[ed.a+1]++
	}
	for v := int32(1); v < n; v++ {
		start[v+1] += start[v]
	}
	fill := make([]int32, n)
	copy(fill, start[:n])
	adj := make([]int32, len(edges))
	for _, ed := range edges {
		adj[fill[ed.a]] = ed.b
		fill[ed.a]++
	}
	index := make([]int32, n)
	low := make([]int32, n)
	onstk := make([]bool, n)
	compOf := make([]int32, n)
	for i := range index {
		index[i] = -1
		compOf[i] = -1
	}
	stk := make([]int32, 0, 128)
	comps := make([][]int32, 0, 64)
	var counter int32
	type frame struct {
		v int32
		w int32
	}
	for _, root := range nodes {
		if index[root] >= 0 {
			continue
		}
		index[root] = counter
		low[root] = counter
		counter++
		stk = append(stk, root)
		onstk[root] = true
		work := []frame{{root, 0}}
		for len(work) > 0 {
			top := &work[len(work)-1]
			v := top.v
			descended := false
			for top.w < start[v+1]-start[v] {
				w := adj[start[v]+top.w]
				top.w++
				if index[w] < 0 {
					index[w] = counter
					low[w] = counter
					counter++
					stk = append(stk, w)
					onstk[w] = true
					work = append(work, frame{w, 0})
					descended = true
					break
				}
				if onstk[w] && index[w] < low[v] {
					low[v] = index[w]
				}
			}
			if descended {
				continue
			}
			work = work[:len(work)-1]
			if len(work) > 0 {
				u := work[len(work)-1].v
				if low[v] < low[u] {
					low[u] = low[v]
				}
			}
			if low[v] == index[v] {
				comp := []int32{}
				for {
					w := stk[len(stk)-1]
					stk = stk[:len(stk)-1]
					onstk[w] = false
					comp = append(comp, w)
					if w == v {
						break
					}
				}
				comps = append(comps, comp)
			}
		}
	}
	for ci, c := range comps {
		for _, v := range c {
			compOf[v] = int32(ci)
		}
	}
	slot := make([]int32, n)
	for i, v := range nodes {
		slot[v] = int32(i)
	}
	nComp := len(comps)
	words := (len(nodes) + 63) / 64
	predE := make([][]int32, nComp)
	succE := make([][]int32, nComp)
	for _, ed := range edges {
		pc, cc := compOf[ed.a], compOf[ed.b]
		if pc == cc {
			continue
		}
		predE[cc] = append(predE[cc], pc)
		succE[pc] = append(succE[pc], cc)
	}
	preds := make([][]uint64, nComp)
	succs := make([][]uint64, nComp)
	setNode := func(bits []uint64, v int32) {
		s := slot[v]
		bits[s>>6] |= 1 << uint(s&63)
	}
	orInto := func(dst, src []uint64) {
		for i := range src {
			dst[i] |= src[i]
		}
	}
	for ci := nComp - 1; ci >= 0; ci-- {
		acc := make([]uint64, words)
		for _, p := range predE[ci] {
			orInto(acc, preds[p])
			for _, v := range comps[p] {
				setNode(acc, v)
			}
		}
		preds[ci] = acc
	}
	for ci := range nComp {
		acc := make([]uint64, words)
		for _, c2 := range succE[ci] {
			orInto(acc, succs[c2])
			for _, v := range comps[c2] {
				setNode(acc, v)
			}
		}
		succs[ci] = acc
	}
	for ci, comp := range comps {
		baseIn := int32(popcount(preds[ci]))
		baseOut := int32(popcount(succs[ci]))
		extra := int32(len(comp) - 1)
		if baseIn+baseOut+extra == 0 {
			continue
		}
		for _, v := range comp {
			e.g.Reach = append(e.g.Reach, Reach{SymbolID: v,
				NTransitive: baseIn + extra, NTransitiveOut: baseOut + extra})
		}
	}
}

func popcount(b []uint64) int {
	n := 0
	for _, w := range b {
		n += popcnt64(w)
	}
	return n
}

func popcnt64(x uint64) int {
	n := 0
	for x != 0 {
		x &= x - 1
		n++
	}
	return n
}

func (e *extractor) includeCycles() {
	g := e.g
	adj := map[int32]*pyset{}
	roots := []int32{}
	for i := range g.Imports {
		im := &g.Imports[i]
		if !im.HasTargetID || im.IsRelative != 1 || im.Kind() != "include" {
			continue
		}
		if g.Files[im.FileID-1].IsTest == 1 {
			continue
		}
		s := adj[im.FileID]
		if s == nil {
			s = newPyset()
			adj[im.FileID] = s
			roots = append(roots, im.FileID)
		}
		s.add(im.TargetID)
	}
	slices.Sort(roots)
	paths := make(map[int32]string, len(g.Files))
	for i := range g.Files {
		paths[g.Files[i].ID] = g.Files[i].Path()
	}
	seen := map[string]bool{}
	type row struct {
		a, b    string
		length  int
		members string
	}
	var rows []row
	for _, root := range roots {
		stack := [][2]any{}
		stack = append(stack, [2]any{root, []int32{root}})
		for len(stack) > 0 {
			top := stack[len(stack)-1]
			stack = stack[:len(stack)-1]
			node := top[0].(int32)
			path := top[1].([]int32)
			if s := adj[node]; s != nil {
				for _, nxt := range s.order() {
					if nxt == root && len(path) >= 2 {
						key := pathKey(path)
						if seen[key] {
							continue
						}
						seen[key] = true
						members := make([]string, 0, len(path))
						for _, f := range path {
							if p, ok := paths[f]; ok {
								members = append(members, p)
							} else {
								members = append(members, "?")
							}
						}
						rows = append(rows, row{members[0], members[len(members)-1],
							len(path), strings.Join(members, " -> ")})
					} else if !containsInt32(path, nxt) && len(path) < 8 {
						stack = append(stack, [2]any{nxt,
							append(append([]int32{}, path...), nxt)})
					}
				}
			}
			if len(rows) > 5000 {
				break
			}
		}
	}
	for _, r := range rows {
		g.Cycles = append(g.Cycles, IncludeCycle{})
		cy := &g.Cycles[len(g.Cycles)-1]
		cgZeroRow(unsafe.Pointer(cy), unsafe.Sizeof(*cy))
		cy.aPath = cgPut(r.a)
		cy.bPath = cgPut(r.b)
		cy.Length = int32(r.length)
		cy.members = cgPut(r.members)
	}
}

func pathKey(path []int32) string {
	cp := append([]int32{}, path...)
	slices.Sort(cp)
	return itoaArr(cp)
}

func itoaArr(v []int32) string {
	var sb strings.Builder
	for i, x := range v {
		if i > 0 {
			sb.WriteByte(',')
		}
		sb.WriteString(strconv.Itoa(int(x)))
	}
	return sb.String()
}

func containsInt32(s []int32, v int32) bool {
	return slices.Contains(s, v)
}

type pyset struct {
	table []int32
	mask  int
	fill  int
	used  int
}

func newPyset() *pyset {
	return &pyset{table: make([]int32, 8), mask: 7}
}

func psetCap(n int) int {
	t := 8
	for n > t/2 {
		t *= 4
	}
	return t
}

func (s *pyset) add(v int32) {
	if want := psetCap(s.used + 1); want > len(s.table) {
		s.resize(want, v)
		return
	}
	s.place(v)
	s.fill++
	s.used++
}

func (s *pyset) place(v int32) {
	mask := len(s.table) - 1
	i := int(uint32(v) & uint32(mask))
	for s.table[i] != 0 {
		i = (i + 1) & mask
	}
	s.table[i] = v
}

func (s *pyset) resize(newSize int, v int32) {
	nt := make([]int32, newSize)
	old := s.table
	s.table = nt
	s.place(v)
	for _, x := range old {
		if x != 0 {
			s.place(x)
		}
	}
	s.fill = s.used + 1
	s.used++
}

func (s *pyset) order() []int32 {
	out := make([]int32, 0, s.used)
	for _, v := range s.table {
		if v != 0 {
			out = append(out, v)
		}
	}
	return out
}

func buildIndex(g *Graph) {
	g.byFile = make(map[int32][]int32, len(g.Files))
	g.byName = make(map[string][]int32, len(g.Symbols))
	for i := range g.Symbols {
		s := g.Symbols[i]
		g.byFile[s.FileID] = append(g.byFile[s.FileID], s.ID)
		g.byName[s.Name()] = append(g.byName[s.Name()], s.ID)
	}
	g.fileByBase = make(map[string]int32, len(g.Files))
	for i := range g.Files {
		b := pathBase(g.Files[i].Path())
		if _, ok := g.fileByBase[b]; !ok {
			g.fileByBase[b] = g.Files[i].ID
		}
	}
}

func qQ1(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "io", "mem", "int_", "alloc", "cyclo", "fan_in", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.NIO <= 0 || (s.NMemory <= 0 && s.NInteger <= 0) {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.NIO), fI32(s.NMemory),
			fI32(s.NInteger), fI32(s.NAlloc), fI32(s.Cyclomatic), fI32(s.FanIn),
			fStr(g.at(s))))
	}
	orderBy(rows, dSum(2, 3), dI(1))
	return cols, limit(rows, p)
}

func qQ2(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "frame_bytes", "levels_per_mb", "unsized", "cyclo",
		"nest", "fan_in", "sloc", "at"}
	frame := g.shapeDense("frame_bytes")
	uns := g.shapeDense("frame_unsized")
	var rows [][]fval
	for _, s := range g.syms() {
		if s.IsRecursive != 1 || s.Kind() != "function" {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		// 48 bytes of return address, saved registers and spill per level is
		// a floor for a non-leaf function on either LP64 ABI
		per := frame[s.ID] + 48
		rows = append(rows, mkrow(fStr(s.Name()), fI32(frame[s.ID]),
			fI32((1<<20)/per), fI32(uns[s.ID]), fI32(s.Cyclomatic),
			fI32(s.MaxNesting), fI32(s.FanIn), fI32(s.Sloc), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(4), cmpS(0))
	return cols, limit(rows, p)
}

func qQ3(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "allocs", "frees", "fan_in", "returns_", "gotos",
		"labels", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.NAlloc <= 0 || g.rv(s).NFree != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.NAlloc), fI32(g.rv(s).NFree),
			fI32(s.FanIn), fI32(s.NReturns), fI32(s.NGotos), fI32(s.NLabels),
			fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(3))
	return cols, limit(rows, p)
}

func qQ4(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"path", "libc", "project", "distinct_fns", "via"}
	type agg struct {
		libc, project int32
		pats          []string
	}
	byFile := map[int32]*agg{}
	var order []int32

	hs := g.hazardsByCatN("alloc")
	for _, hi := range hs {
		h := &g.Hazards[hi]
		sid := h.SymbolID
		if sid < 1 || int(sid) > len(g.Symbols) {
			continue
		}
		fid := g.Symbols[sid-1].FileID
		a := byFile[fid]
		if a == nil {
			a = &agg{}
			byFile[fid] = a
			order = append(order, fid)
		}
		if libcAllocNames[h.Pattern()] {
			a.libc += h.N
		} else {
			a.project += h.N
		}
		if !containsStr(a.pats, h.Pattern()) {
			a.pats = append(a.pats, h.Pattern())
		}
	}
	var rows [][]fval
	for _, fid := range order {
		a := byFile[fid]
		if a.libc <= 0 || a.project <= 0 {
			continue
		}
		if int(fid) > len(g.Files) {
			continue
		}
		f := &g.Files[fid-1]
		if !likeMatch(g.modName(f.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(f.Path()), fI32(a.libc), fI32(a.project),
			fInt(len(a.pats)), fStr(groupConcat(a.pats))))
	}
	orderBy(rows, dSum(1, 2))
	return cols, limit(rows, p)
}

func qQ5(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "allocs", "depth", "frees", "calls", "fan_in", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.AllocInLoop <= 0 || !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.AllocInLoop),
			fI32(s.MaxLoopDepth), fI32(g.rv(s).NFree), fI32(s.CallInLoop),
			fI32(s.FanIn), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(2))
	return cols, limit(rows, p)
}

func qQ6(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "allocs", "frees", "loops", "brs", "sloc", "fan_in", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.Kind() != "function" || s.NAlloc <= 0 || g.rv(s).NFree <= 0 ||
			s.NLoops <= 0 || s.Sloc > 120 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.NAlloc), fI32(g.rv(s).NFree),
			fI32(s.NLoops), fI32(s.NBranches), fI32(s.Sloc), fI32(s.FanIn),
			fStr(g.at(s))))
	}
	orderBy(rows, dWeighted([][3]int32{{3, 3}, {1, 4}, {2, 1}}), cmpI(5), cmpS(0))
	return cols, limit(rows, p)
}

func qQ7(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name_", "type_", "ptr", "arr", "stat", "init", "module",
		"at", "mod_conc_fns"}
	modConc := g.perMod()
	for i := range g.Symbols {
		if g.Symbols[i].NConcurrency > 0 {
			modConc.add(g.Symbols[i].ModuleID, 1)
		}
	}
	var rows [][]fval
	for i := range g.Globals {
		gl := &g.Globals[i]
		if gl.IsConst != 0 || gl.IsAtomic != 0 || gl.IsVolatile != 0 {
			continue
		}
		if gl.FileID < 1 || int(gl.FileID) > len(g.Files) {
			continue
		}
		f := &g.Files[gl.FileID-1]
		if f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(gl.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(gl.Name()), fStr(gl.Type()), fI32(gl.PtrDepth),
			fI32(gl.IsArray), fI32(gl.IsStatic), fI32(gl.HasInit),
			fStr(g.modName(gl.ModuleID)),
			fStr(f.Path()+":"+strconv.Itoa(int(gl.Line))),
			fI32(modConc.at(gl.ModuleID))))
	}
	orderBy(rows, dI(8), dI(2), cmpS(0))
	return cols, limit(rows, p)
}

func qQ8(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "sw", "depth", "calls", "cases", "sloc", "fan_in", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if g.rv(s).SwitchInLoop <= 0 || !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(g.rv(s).SwitchInLoop),
			fI32(s.MaxLoopDepth), fI32(s.CallInLoop), fI32(s.NCases),
			fI32(s.Sloc), fI32(s.FanIn), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(3))
	return cols, limit(rows, p)
}

func qQ9(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "strlens", "depth", "loops", "fan_in", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if g.rv(s).StrlenInLoop <= 0 || !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(g.rv(s).StrlenInLoop),
			fI32(s.MaxLoopDepth), fI32(s.NLoops), fI32(s.FanIn), fStr(g.at(s))))
	}
	orderBy(rows, dI(2), dI(1))
	return cols, limit(rows, p)
}

func qQ10(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "r_null", "r_neg", "r_0", "r_val", "r_void",
		"ret_type", "fan_in", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.Kind() != "function" {
			continue
		}
		shapes := 0
		for _, v := range [][2]int32{{g.rv(s).RetNull, 0}, {g.rv(s).RetNeg, 0}, {g.rv(s).RetZero, 0}} {
			if v[0] > 0 {
				shapes++
			}
		}
		if shapes < 2 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(g.rv(s).RetNull), fI32(g.rv(s).RetNeg),
			fI32(g.rv(s).RetZero), fI32(g.rv(s).RetVal), fI32(g.rv(s).RetVoid),
			fOptStr(s.ReturnType(), s.HasReturnType), fI32(s.FanIn), fStr(g.at(s))))
	}
	orderBy(rows, dI(7), cmpS(0))
	return cols, limit(rows, p)
}

func qQ11(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "kind", "sloc", "cyclo", "ext_calls", "at"}
	var rows [][]fval
	named := g.namedAsValue()
	for _, s := range g.syms() {
		if s.FanIn != 0 || s.IsPublic != 0 || s.IsTest != 0 ||
			s.IsEntrypoint != 0 || s.IsOverride != 0 || s.IsAbstract != 0 {
			continue
		}
		if named(s) {
			continue
		}
		switch s.Kind() {
		case "function", "method", "closure":
		default:
			continue
		}
		if s.Name() == "(anonymous)" || s.Name() == "<module>" {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 || f.IsGenerated != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fStr(s.Kind()), fI32(s.Sloc),
			fI32(s.Cyclomatic), fI32(g.rv(s).NExternalCalls), fStr(g.at(s))))
	}
	orderBy(rows, dI(2))
	return cols, limit(rows, p)
}

func qQ12(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "module", "n_patterns", "patterns", "reentrancy_calls",
		"concurrency_calls", "atomics", "fan_in", "cyclo", "at"}
	bySym := map[int32][]string{}
	for _, i := range g.hazCats("reentrancy") {
		h := &g.Hazards[i]
		if !containsStr(bySym[h.SymbolID], h.Pattern()) {
			bySym[h.SymbolID] = append(bySym[h.SymbolID], h.Pattern())
		}
	}
	var rows [][]fval
	for _, s := range g.syms() {
		pats := bySym[s.ID]
		if len(pats) == 0 || s.NConcurrency <= 0 {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fStr(g.modName(s.ModuleID)),
			fInt(len(pats)), fStr(groupConcat(pats)), fI32(s.NReentrancy),
			fI32(s.NConcurrency), fI32(s.NAtomic), fI32(s.FanIn),
			fI32(s.Cyclomatic), fStr(g.at(s))))
	}
	orderBy(rows, dI(5), dI(2), dI(7))
	return cols, limit(rows, p)
}

var convFns = []string{"atoi", "atol", "atoll", "atof", "strtol", "strtoul",
	"strtoll", "strtoull", "strtod", "strtof", "strtoimax", "strtoumax"}

func qQ13(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "module", "n_conversions", "conversions",
		"conversion_calls", "io_calls", "stdio_calls", "memory_calls",
		"returns_value", "fan_in", "at"}
	bySym := map[int32][]string{}
	total := map[int32]int32{}
	for pi, h := range g.hazardsByPattern(convFns) {
		_ = pi
		if !containsStr(bySym[h.SymbolID], h.Pattern()) {
			bySym[h.SymbolID] = append(bySym[h.SymbolID], h.Pattern())
		}
		total[h.SymbolID] += h.N
	}
	var rows [][]fval
	for _, s := range g.syms() {
		if len(bySym[s.ID]) == 0 || (s.NIO <= 0 && s.NStdio <= 0) {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fStr(g.modName(s.ModuleID)),
			fInt(len(bySym[s.ID])), fStr(groupConcat(bySym[s.ID])),
			fI32(total[s.ID]), fI32(s.NIO), fI32(s.NStdio), fI32(s.NMemory),
			fI32(g.rv(s).RetVal), fI32(s.FanIn), fStr(g.at(s))))
	}
	orderBy(rows, dI(4), dI(5), dI(9))
	return cols, limit(rows, p)
}

var strcpyFns = []string{"sprintf", "strcpy", "strcat", "gets", "vsprintf",
	"scanf", "sscanf", "fscanf", "vfscanf", "vsscanf"}
var printfFns = []string{"printf", "fprintf", "sprintf", "snprintf", "vprintf",
	"vfprintf", "vsprintf", "vsnprintf", "syslog"}
var sigUnsafe = []string{"malloc", "calloc", "realloc", "free", "printf",
	"fprintf", "sprintf", "snprintf", "syslog", "fopen", "fclose", "fread",
	"fwrite", "strdup"}

func hazardsBy(g *Graph, cat string, pats []string) (map[int32][]string, map[int32]int32) {
	list := map[int32][]string{}
	total := map[int32]int32{}
	for _, i := range g.hazRows(pats) {
		h := &g.Hazards[i]
		if cat != "" && h.Category() != cat {
			continue
		}
		if !containsStr(list[h.SymbolID], h.Pattern()) {
			list[h.SymbolID] = append(list[h.SymbolID], h.Pattern())
		}
		total[h.SymbolID] += h.N
	}
	return list, total
}

func qQ14(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "n_patterns", "patterns", "dangerous_calls",
		"memory_calls", "io_calls", "fan_in", "cyclo", "at"}
	list, total := hazardsBy(g, "", strcpyFns)
	var rows [][]fval
	for _, s := range g.syms() {
		if len(list[s.ID]) == 0 {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fInt(len(list[s.ID])),
			fStr(groupConcat(list[s.ID])), fI32(total[s.ID]), fI32(s.NMemory),
			fI32(s.NIO), fI32(s.FanIn), fI32(s.Cyclomatic), fStr(g.at(s))))
	}
	orderBy(rows, dI(3), dI(6))
	return cols, limit(rows, p)
}

func qQ15(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "n_patterns", "patterns", "format_calls", "io_calls",
		"stdio_calls", "fan_in", "cyclo", "at"}
	list, total := hazardsBy(g, "", printfFns)
	for k := range list {
		sort.Strings(list[k])
	}
	var rows [][]fval
	for _, s := range g.syms() {
		if len(list[s.ID]) == 0 {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fInt(len(list[s.ID])),
			fStr(groupConcat(list[s.ID])), fI32(total[s.ID]), fI32(s.NIO),
			fI32(s.NStdio), fI32(s.FanIn), fI32(s.Cyclomatic), fStr(g.at(s))))
	}
	orderBy(rows, dI(3), dI(6))
	return cols, limit(rows, p)
}

func qQ16(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "allocs", "frees", "imbalance", "memory_ops",
		"fan_in", "cyclo", "returns", "at"}
	var rows [][]fval
	for _, s := range g.symsName() {
		if s.NAlloc <= 0 || g.rv(s).NFree != 0 {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.NAlloc), fI32(g.rv(s).NFree),
			fI32(s.NAlloc-g.rv(s).NFree), fI32(s.NMemory), fI32(s.FanIn),
			fI32(s.Cyclomatic), fI32(s.NReturns), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(5))
	return cols, limit(rows, p)
}

func qQ17(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "allocs", "frees", "excess_frees", "return_paths",
		"cyclo", "fan_in", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if g.rv(s).NFree <= s.NAlloc || g.rv(s).NFree <= 1 {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.NAlloc), fI32(g.rv(s).NFree),
			fI32(g.rv(s).NFree-s.NAlloc), fI32(s.NReturns), fI32(s.Cyclomatic),
			fI32(s.FanIn), fStr(g.at(s))))
	}
	orderBy(rows, dI(3), dI(6))
	return cols, limit(rows, p)
}

func qQ18(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "memory_ops", "null_checks", "allocs", "cyclo",
		"fan_in", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.NMemory <= 5 || s.NNullCheck != 0 {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.NMemory),
			fI32(s.NNullCheck), fI32(s.NAlloc), fI32(s.Cyclomatic),
			fI32(s.FanIn), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(5))
	return cols, limit(rows, p)
}

func qQ19(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "arith_ops", "integer_ops", "comparisons", "loops",
		"cyclo", "fan_in", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.NArith <= 20 {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.NArith), fI32(s.NInteger),
			fI32(s.NCmp), fI32(s.NLoops), fI32(s.Cyclomatic), fI32(s.FanIn),
			fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(6))
	return cols, limit(rows, p)
}

// shapeCounts collects one `shape` hazard pattern per symbol.
// The result is shared between callers and must not be written to.
func (g *Graph) shapeCounts(pat string) map[int32]int32 {
	ix := g.hz()
	if m, ok := ix.shape[pat]; ok {
		return m
	}
	m := map[int32]int32{}
	for _, i := range ix.byPat[pat] {
		if h := &g.Hazards[i]; h.Category() == "shape" {
			m[h.SymbolID] += h.N
		}
	}
	ix.shape[pat] = m
	return m
}

// hazIndex is the hazards table by pattern and by category, each list in
// table order. Some thirty queries want a handful of patterns out of a
// hundred thousand rows; each used to compare every row's strings to find
// them, a millisecond a time on PostgreSQL.
type hazIndex struct {
	n     int
	byPat map[string][]int32
	byCat map[string][]int32
	shape map[string]map[int32]int32
	dense map[string][]int32

	ign, who map[string]int32
}

func (g *Graph) hz() *hazIndex {
	if g.hazIx != nil && g.hazIx.n == len(g.Hazards) {
		return g.hazIx
	}
	ix := &hazIndex{n: len(g.Hazards), byPat: map[string][]int32{},
		byCat: map[string][]int32{}, shape: map[string]map[int32]int32{},
		dense: map[string][]int32{}}
	for i := range g.Hazards {
		h := &g.Hazards[i]
		ix.byPat[h.Pattern()] = append(ix.byPat[h.Pattern()], int32(i))
		ix.byCat[h.Category()] = append(ix.byCat[h.Category()], int32(i))
	}
	g.hazIx = ix
	return ix
}

// shapeDense is shapeCounts as an array indexed by symbol id.
func (g *Graph) shapeDense(pat string) []int32 {
	ix := g.hz()
	if d, ok := ix.dense[pat]; ok {
		return d
	}
	d := make([]int32, len(g.Symbols)+1)
	for id, n := range g.shapeCounts(pat) {
		if uint32(id) < uint32(len(d)) {
			d[id] = n
		}
	}
	ix.dense[pat] = d
	return d
}

// edgeCSR is the call edges grouped by one end, as edge-table indices in
// table order: the edges of symbol id are idx[start[id]:start[id+1]].
type edgeCSR struct {
	start, idx []int32
}

func (c *edgeCSR) of(id int32) []int32 {
	if id < 0 || int(id)+1 >= len(c.start) {
		return nil
	}
	return c.idx[c.start[id]:c.start[id+1]]
}

func (g *Graph) edgeCSR(byCallee bool) *edgeCSR {
	slot := &g.edgeOut
	if byCallee {
		slot = &g.edgeIn
	}
	if *slot != nil && (*slot).n == len(g.Edges) {
		return &(*slot).edgeCSR
	}
	n := len(g.Symbols) + 1
	end := func(e *Edge) int32 {
		id := e.CallerID
		if byCallee {
			id = e.CalleeID
		}
		if id < 0 || int(id) >= n {
			return -1
		}
		return id
	}
	c := &edgeCSRc{n: len(g.Edges)}
	c.start = make([]int32, n+1)
	for i := range g.Edges {
		if id := end(&g.Edges[i]); id >= 0 {
			c.start[id+1]++
		}
	}
	for i := 1; i <= n; i++ {
		c.start[i] += c.start[i-1]
	}
	c.idx = make([]int32, c.start[n])
	fill := slices.Clone(c.start[:n])
	for i := range g.Edges {
		if id := end(&g.Edges[i]); id >= 0 {
			c.idx[fill[id]] = int32(i)
			fill[id]++
		}
	}
	*slot = c
	return &c.edgeCSR
}

type edgeCSRc struct {
	edgeCSR
	n int
}

// symIndex holds what several queries used to rebuild for themselves: tables
// keyed by a NAME (declarations, address-taken references) joined to the
// symbols carrying that name, as arrays indexed by symbol id. A query then
// reads a slot where it used to hash the symbol's name -- for every symbol.
type symIndex struct {
	nSym, nDecl, nAddr, nMacro int

	nameN  []int32 // symbols sharing this symbol's name
	fnDefs []int32 // function symbols sharing it
	rank   []int32 // position in syms()

	declN, declHdr []int32 // declaration rows with this name; those in a .h

	addrAny    []bool  // some address-taken row has this name
	addrInFile []bool  // ... in this symbol's own file
	bodyN      []int32 // rows inside a function body with this name
	bodyInFile []int32 // ... in this symbol's own file
	refs       map[string][]addrRef

	macro []*Macro
}

type addrRef struct {
	file int32
	kind string
}

func (g *Graph) six() *symIndex {
	if x := g.symIx; x != nil && x.nSym == len(g.Symbols) && x.nDecl == len(g.Decls) &&
		x.nAddr == len(g.Addr) && x.nMacro == len(g.Macros) {
		return x
	}
	n := len(g.Symbols) + 1
	x := &symIndex{nSym: len(g.Symbols), nDecl: len(g.Decls), nAddr: len(g.Addr),
		nMacro: len(g.Macros),
		nameN:  make([]int32, n), fnDefs: make([]int32, n), rank: make([]int32, n),
		declN: make([]int32, n), declHdr: make([]int32, n),
		addrAny: make([]bool, n), addrInFile: make([]bool, n),
		bodyN: make([]int32, n), bodyInFile: make([]int32, n),
		refs: map[string][]addrRef{}, macro: make([]*Macro, n)}
	if g.byName == nil {
		buildIndex(g)
	}
	for _, ids := range g.byName {
		fns := int32(0)
		for _, id := range ids {
			if g.Symbols[id-1].Kind() == "function" {
				fns++
			}
		}
		for _, id := range ids {
			x.nameN[id], x.fnDefs[id] = int32(len(ids)), fns
		}
	}
	for i, s := range g.syms() {
		x.rank[s.ID] = int32(i)
	}
	for i := range g.Decls {
		d := &g.Decls[i]
		hdr := int32(0)
		if f := g.fileOf2(d.FileID); f != nil && f.Ext() == ".h" {
			hdr = 1
		}
		for _, id := range g.byName[d.Name()] {
			x.declN[id]++
			x.declHdr[id] += hdr
		}
	}
	for i := range g.Addr {
		a := &g.Addr[i]
		nm := a.Name()
		for _, id := range g.byName[nm] {
			own := g.Symbols[id-1].FileID == a.FileID
			x.addrAny[id] = true
			if own {
				x.addrInFile[id] = true
			}
			if a.HasSym {
				x.bodyN[id]++
				if own {
					x.bodyInFile[id]++
				}
			}
		}
		if !a.HasSym {
			x.refs[nm] = append(x.refs[nm], addrRef{a.FileID, a.Kind()})
		}
	}
	for i := range g.Macros {
		if id := g.Macros[i].SymbolID; id >= 0 && int(id) < n {
			x.macro[id] = &g.Macros[i]
		}
	}
	g.symIx = x
	return x
}

// perID is a count per symbol, file or module id. It takes any id a table
// row may carry -- a row with no symbol has id 0, a damaged one anything --
// where a map took them silently and a bare slice would not.
type perID []int32

func (g *Graph) perSym() perID  { return make(perID, len(g.Symbols)+1) }
func (g *Graph) perFile() perID { return make(perID, len(g.Files)+1) }
func (g *Graph) perMod() perID  { return make(perID, len(g.Modules)+1) }

func (p perID) add(id, n int32) {
	if uint32(id) < uint32(len(p)) {
		p[id] += n
	}
}

func (p perID) at(id int32) int32 {
	if uint32(id) < uint32(len(p)) {
		return p[id]
	}
	return 0
}

// groupRows groups row numbers 0..n-1 by key(i), a value in [0, nk): the rows
// of key k are idx[start[k]:start[k+1]], in row order. Rows whose key is out
// of range are left out.
func groupRows(n, nk int, key func(i int) int32) (start, idx []int32) {
	start = make([]int32, nk+1)
	for i := 0; i < n; i++ {
		if k := key(i); k >= 0 && int(k) < nk {
			start[k+1]++
		}
	}
	for k := 1; k <= nk; k++ {
		start[k] += start[k-1]
	}
	idx = make([]int32, start[nk])
	fill := slices.Clone(start[:nk])
	for i := 0; i < n; i++ {
		if k := key(i); k >= 0 && int(k) < nk {
			idx[fill[k]] = int32(i)
			fill[k]++
		}
	}
	return start, idx
}

// discards is, per callee name, how many call sites drop its result and in
// how many callers. Built once; callers must not write to it.
func (ix *hazIndex) discards(g *Graph) (ign, who map[string]int32) {
	if ix.ign == nil {
		ix.ign, ix.who = map[string]int32{}, map[string]int32{}
		for _, i := range ix.byCat["discard"] {
			h := &g.Hazards[i]
			ix.ign[h.Pattern()] += h.N
			ix.who[h.Pattern()]++
		}
	}
	return ix.ign, ix.who
}

// hazRows is the rows whose pattern is one of pats, in table order.
func (g *Graph) hazRows(pats []string) []int32 {
	ix := g.hz()
	var out []int32
	for k, pat := range pats {
		if !slices.Contains(pats[:k], pat) {
			out = append(out, ix.byPat[pat]...)
		}
	}
	slices.Sort(out)
	return out
}

// hazCats is the rows in any of the categories, in table order.
func (g *Graph) hazCats(cats ...string) []int32 {
	ix := g.hz()
	if len(cats) == 1 {
		return ix.byCat[cats[0]]
	}
	var out []int32
	for _, c := range cats {
		out = append(out, ix.byCat[c]...)
	}
	slices.Sort(out)
	return out
}

func qQ20(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "unguarded_divs", "var_divs", "div_in_loop",
		"n_params", "cyclo", "fan_in", "at"}
	ung := g.shapeDense("div_unguarded")
	all := g.shapeDense("div_by_var")
	var rows [][]fval
	for _, s := range g.syms() {
		if ung[s.ID] <= 0 {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(ung[s.ID]), fI32(all[s.ID]),
			fI32(g.rv(s).DivInLoop), fI32(s.NParams), fI32(s.Cyclomatic),
			fI32(s.FanIn), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(6), cmpS(0))
	return cols, limit(rows, p)
}

func qQ21(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "switches", "cases", "cyclo", "fan_in", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.NSwitch <= 0 {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.NSwitch), fI32(s.NCases),
			fI32(s.Cyclomatic), fI32(s.FanIn), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(4))
	return cols, limit(rows, p)
}

func qQ22(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "concurrency_ops", "atomic_ops", "reentrancy_calls",
		"memory_ops", "fan_in", "cyclo", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.NConcurrency <= 0 {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.NConcurrency),
			fI32(s.NAtomic), fI32(s.NReentrancy), fI32(s.NMemory),
			fI32(s.FanIn), fI32(s.Cyclomatic), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(5))
	return cols, limit(rows, p)
}

func qQ23(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "n_unsafe_patterns", "unsafe_patterns", "io_calls",
		"memory_calls", "fan_in", "at"}
	list, _ := hazardsBy(g, "", sigUnsafe)
	var rows [][]fval
	for _, s := range g.syms() {
		if len(list[s.ID]) == 0 {
			continue
		}
		low := asciiLower(s.Name())
		if !strings.Contains(low, "sig") && !strings.Contains(low, "handler") &&
			!strings.Contains(low, "signal") {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fInt(len(list[s.ID])),
			fStr(groupConcat(list[s.ID])), fI32(s.NIO), fI32(s.NMemory),
			fI32(s.FanIn), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(5))
	return cols, limit(rows, p)
}

func qQ24(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "no_exit_loops", "forever_loops", "returns", "cyclo",
		"fan_in", "sloc", "at"}
	nox := g.shapeDense("loop_forever_noexit")
	all := g.shapeDense("loop_forever")
	var rows [][]fval
	for _, s := range g.syms() {
		if nox[s.ID] <= 0 || s.Kind() != "function" {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(nox[s.ID]), fI32(all[s.ID]),
			fI32(s.NReturns), fI32(s.Cyclomatic), fI32(s.FanIn), fI32(s.Sloc),
			fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(5), cmpS(0))
	return cols, limit(rows, p)
}

func qQ25(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "ignored_sites", "call_sites", "pct_ignored",
		"callers_ignoring", "returns", "at"}
	ign, who := g.hz().discards(g)
	// a name defined twice cannot say which definition was called. The
	// candidates are the few symbols named by a discard row, taken in scan
	// order; asking every symbol in the tree for its name's count was most of
	// this query.
	x := g.six()
	var cand []*Symbol
	for nm := range ign {
		for _, id := range g.byName[nm] {
			cand = append(cand, g.Symbols[id-1])
		}
	}
	slices.SortFunc(cand, func(a, b *Symbol) int { return cmp.Compare(x.rank[a.ID], x.rank[b.ID]) })
	var rows [][]fval
	for _, s := range cand {
		n := ign[s.Name()]
		if n <= 0 || s.Kind() != "function" || x.fnDefs[s.ID] != 1 {
			continue
		}
		rt := strings.TrimSpace(s.ReturnType())
		if rt == "" || rt == "void" || strings.HasSuffix(rt, " void") {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		sites := max(s.NCallsites, n)
		rows = append(rows, mkrow(fStr(s.Name()), fI32(n), fI32(sites),
			fI32(pct(int64(n), int64(sites))), fI32(who[s.Name()]),
			fStr(jtrunc(rt, 24)), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(3), cmpS(0))
	return cols, limit(rows, p)
}

func qQ26(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "uses", "params", "body_len", "multiline", "at"}
	bySym := g.six().macro
	var rows [][]fval
	for _, s := range g.syms() {
		m := bySym[s.ID]
		if m == nil || m.IsFunctionlike != 1 || m.NUses <= 3 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(m.NUses), fI32(m.NParams),
			fI32(m.BodyLen), fI32(m.IsMultiline), fStr(g.at(s))))
	}
	orderBy(rows, dI(1))
	return cols, limit(rows, p)
}

func qQ27(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "fnptr_calls", "dyn_calls", "direct_calls",
		"fan_in", "sloc", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if g.rv(s).NFnptrCalls+s.NDynamicCalls <= 0 || s.Kind() != "function" {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		direct := maxI(0, s.NCalls-g.rv(s).NFnptrCalls-s.NDynamicCalls)
		rows = append(rows, mkrow(fStr(s.Name()), fI32(g.rv(s).NFnptrCalls),
			fI32(s.NDynamicCalls), fI32(direct), fI32(s.FanIn), fI32(s.Sloc),
			fStr(g.at(s))))
	}
	orderBy(rows, dSum(1, 2), cmpS(0))
	return cols, limit(rows, p)
}

func qQ28(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"path", "user_headers", "sys_headers", "pct_user", "sloc"}
	type agg struct{ user, sys int32 }
	byFile := map[int32]*agg{}
	var order []int32
	for _, im := range g.Imports {
		if im.Kind() != "include" {
			continue
		}
		a := byFile[im.FileID]
		if a == nil {
			a = &agg{}
			byFile[im.FileID] = a
			order = append(order, im.FileID)
		}
		if im.IsRelative == 1 {
			a.user++
		} else {
			a.sys++
		}
	}
	var rows [][]fval
	for _, fid := range order {
		f := g.fileOf2(fid)
		if f == nil || !likeMatch(g.modName(f.ModuleID), p.mod) {
			continue
		}
		a := byFile[fid]
		rows = append(rows, mkrow(fStr(f.Path()), fI32(a.user), fI32(a.sys),
			fI32(pct(int64(a.user), int64(a.user+a.sys))), fI32(f.Sloc)))
	}
	orderBy(rows, cmpI(3), dSum(1, 2))
	return cols, limit(rows, p)
}

func qQ29(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"fn_a", "fn_b", "at_a", "at_b", "a_calls_b", "b_calls_a"}
	out := g.edgeCSR(false)
	var rows [][]fval
	for i := range g.Edges {
		a := &g.Edges[i]
		if a.CallerID >= a.CalleeID {
			continue
		}
		rev := int32(0)
		for _, j := range out.of(a.CalleeID) {
			if g.Edges[j].CalleeID == a.CallerID {
				rev = g.Edges[j].NCalls
			}
		}
		if rev == 0 {
			continue
		}
		sa := g.sym(a.CallerID)
		sb := g.sym(a.CalleeID)
		if sa == nil || sb == nil {
			continue
		}
		if !likeMatch(g.modName(sa.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(sa.Name()), fStr(sb.Name()), fStr(g.at(sa)),
			fStr(g.at(sb)), fI32(a.NCalls), fI32(rev)))
	}
	orderBy(rows, dSum(4, 5))
	return cols, limit(rows, p)
}

func qQ30(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"path", "name", "type", "line", "has_init", "is_volatile",
		"is_atomic", "ptr_depth"}
	var rows [][]fval
	for i := range g.Globals {
		gl := &g.Globals[i]
		if gl.IsStatic != 0 || gl.IsConst != 0 {
			continue
		}
		if !likeMatch(g.modName(gl.ModuleID), p.mod) {
			continue
		}
		f := g.fileOf2(gl.FileID)
		if f == nil {
			continue
		}
		rows = append(rows, mkrow(fStr(f.Path()), fStr(gl.Name()), fStr(gl.Type()),
			fI32(gl.Line), fI32(gl.HasInit), fI32(gl.IsVolatile),
			fI32(gl.IsAtomic), fI32(gl.PtrDepth)))
	}
	orderBy(rows, dI(6), dI(5), cmpS(0), cmpI(3))
	return cols, limit(rows, p)
}

func (g *Graph) fileOf2(id int32) *File {
	if id < 1 || int(id) > len(g.Files) {
		return nil
	}
	return &g.Files[id-1]
}

func qQ31(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"header", "sloc", "n_syms", "n_fns", "inbound_calls",
		"included_by"}
	nf := len(g.Files) + 1
	okF := func(id int32) bool { return id >= 0 && int(id) < nf }
	nFns := make([]int32, nf)
	nSyms := make([]int32, nf)
	for _, s := range g.Symbols {
		if !okF(s.FileID) {
			continue
		}
		nSyms[s.FileID]++
		if s.Kind() == "function" {
			nFns[s.FileID]++
		}
	}
	inbound := make([]bool, nf)
	for i := range g.Edges {
		if s := g.sym(g.Edges[i].CalleeID); s != nil && okF(s.FileID) {
			inbound[s.FileID] = true
		}
	}
	// distinct (header, includer) pairs
	var pairs []uint64
	for i := range g.Imports {
		im := &g.Imports[i]
		if im.Kind() == "include" && im.HasTargetID && okF(im.TargetID) {
			pairs = append(pairs, uint64(uint32(im.TargetID))<<32|uint64(uint32(im.FileID)))
		}
	}
	slices.Sort(pairs)
	includedBy := make([]int32, nf)
	for i, k := range pairs {
		if i == 0 || k != pairs[i-1] {
			includedBy[k>>32]++
		}
	}
	var rows [][]fval
	for i := range g.Files {
		f := &g.Files[i]
		if !okF(f.ID) || includedBy[f.ID] == 0 || f.NSymbols <= 0 || nFns[f.ID] <= 0 ||
			inbound[f.ID] {
			continue
		}
		if !likeMatch(g.modName(f.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(f.Path()), fI32(f.Sloc), fI32(nSyms[f.ID]),
			fI32(nFns[f.ID]), fI32(0), fI32(includedBy[f.ID])))
	}
	orderBy(rows, dI(5), dI(1))
	return cols, limit(rows, p)
}

func qQ32(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "transitive_callers", "fan_in", "n_calls", "sloc", "at"}
	var rows [][]fval
	for _, r := range g.Reach {
		s := g.sym(r.SymbolID)
		if s == nil || s.Kind() != "function" {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(r.NTransitive),
			fI32(s.FanIn), fI32(s.NCalls), fI32(s.Sloc), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), cmpS(0))
	return cols, limit(rows, p)
}

func qQ33(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "total_size", "tail_pad", "total_pad", "n_lines_64",
		"exact", "n_fields", "n_ptr_fields", "at"}
	nTop, nPtr := g.perSym(), g.perSym()
	for i := range g.Layout {
		l := &g.Layout[i]
		if l.Depth == 0 {
			nTop.add(l.SymbolID, 1)
		}
		if l.PtrDepth > 0 {
			nPtr.add(l.SymbolID, 1)
		}
	}
	var rows [][]fval
	for _, ss := range g.SSize {
		if ss.Exact != 1 || ss.TotalSize < 64 {
			continue
		}
		s := g.sym(ss.SymbolID)
		if s == nil {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(ss.TotalSize),
			fI32(ss.TailPad), fI32(ss.TotalPad), fI32(ss.NLines64),
			fI32(ss.Exact), fI32(nTop.at(ss.SymbolID)), fI32(nPtr.at(ss.SymbolID)),
			fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(3))
	return cols, limit(rows, p)
}

func qQ34(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"path", "extern_globals", "extern_calls", "total_calls",
		"pct_external"}
	nf := len(g.Files) + 1
	okF := func(id int32) bool { return id >= 0 && int(id) < nf }
	eg := make([]int32, nf)
	for i := range g.Globals {
		if gl := &g.Globals[i]; gl.IsStatic == 0 && gl.IsConst == 0 && okF(gl.FileID) {
			eg[gl.FileID]++
		}
	}
	ec := make([]int32, nf)
	tot := make([]int32, nf)
	nSym := make([]int32, nf)
	for _, s := range g.Symbols {
		if !okF(s.FileID) {
			continue
		}
		nSym[s.FileID]++
		if s.Kind() == "function" {
			ec[s.FileID] += g.rv(s).NExternalCalls
			tot[s.FileID] += s.NCalls
		}
	}
	var rows [][]fval
	for i := range g.Files {
		f := &g.Files[i]
		if !okF(f.ID) || nSym[f.ID] == 0 || (ec[f.ID] == 0 && eg[f.ID] == 0) {
			continue
		}
		if !likeMatch(g.modName(f.ModuleID), p.mod) {
			continue
		}
		var pv fval = fNull()
		if tot[f.ID] != 0 {
			pv = fI32(pct(int64(ec[f.ID]), int64(tot[f.ID])))
		}
		rows = append(rows, mkrow(fStr(f.Path()), fI32(eg[f.ID]), fI32(ec[f.ID]),
			fI32(tot[f.ID]), pv))
	}
	orderBy(rows, dI(2))
	return cols, limit(rows, p)
}

func qQ35(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "n_sigs", "n_files", "where_defined"}
	// the scan is in name order, so the definitions of one name are adjacent
	// and a group is a run, not a map entry
	var rows [][]fval
	var (
		cur   string
		open  bool
		sigs  []string
		files []int32
		paths []string
	)
	flush := func() {
		if open && len(sigs) > 1 {
			rows = append(rows, mkrow(fStr(cur), fInt(len(sigs)), fInt(len(files)),
				fStr(groupConcat(paths))))
		}
	}
	for _, s := range g.symsName() {
		if s.Kind() != "function" || !s.HasSignature || s.Signature() == "" {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsGenerated != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		if nm := s.Name(); !open || nm != cur {
			flush()
			cur, open = nm, true
			sigs, files, paths = sigs[:0], files[:0], paths[:0]
		}
		if sig := s.Signature(); !slices.Contains(sigs, sig) {
			sigs = append(sigs, sig)
		}
		if !slices.Contains(files, f.ID) {
			files = append(files, f.ID)
			paths = append(paths, f.Path())
		}
	}
	flush()
	orderBy(rows, dI(2), dI(1), cmpS(0))
	return cols, limit(rows, p)
}

func qQ36(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "path", "line_start", "fan_in", "n_caller_files"}
	// only "none, one, or more than one calling file" is ever asked
	n1 := len(g.Symbols) + 1
	first := make([]int32, n1)
	nFiles := make([]uint8, n1)
	for i := range g.Edges {
		e := &g.Edges[i]
		cs := g.sym(e.CallerID)
		if cs == nil || e.CalleeID < 0 || int(e.CalleeID) >= n1 {
			continue
		}
		switch id := e.CalleeID; {
		case nFiles[id] == 0:
			first[id], nFiles[id] = cs.FileID, 1
		case first[id] != cs.FileID:
			nFiles[id] = 2
		}
	}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.Kind() != "function" || s.IsStatic != 0 {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsGenerated != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		n := int32(nFiles[s.ID])
		if s.FanIn <= 0 || n > 1 {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fStr(f.Path()),
			fI32(s.LineStart), fI32(s.FanIn), fI32(n)))
	}
	orderBy(rows, dI(3), cmpS(0))
	return cols, limit(rows, p)
}

var riskyApis = []string{"system", "popen", "execve", "execl", "execlp",
	"execvp", "execv", "fork", "posix_spawn", "posix_spawnp", "vfork", "wordexp",
	"dlopen", "dlmopen", "mktemp", "tmpnam", "tempnam", "access"}

func qQ37(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"path", "caller", "api", "category", "sites", "first_line",
		"fan_in"}
	var rows [][]fval
	for _, i := range g.hazRows(riskyApis) {
		h := &g.Hazards[i]
		s := g.sym(h.SymbolID)
		if s == nil {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsGenerated != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(f.Path()), fStr(s.Name()), fStr(h.Pattern()),
			fStr(h.Category()), fI32(h.N), fI32(h.FirstLine), fI32(s.FanIn)))
	}
	orderBy(rows, dI(6), dI(4))
	return cols, limit(rows, p)
}

func qQ38(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "candidate", "line", "at"}
	var rows [][]fval
	for _, sc := range g.Secrets {
		s := g.sym(sc.SymbolID)
		if s == nil {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 || f.IsGenerated != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		v := sc.Value()
		if strings.HasPrefix(v, "/") || strings.Contains(v, "|") ||
			strings.Contains(v, "%") {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fStr(v), fI32(sc.Line),
			fStr(f.Path()+":"+strconv.Itoa(int(sc.Line)))))
	}
	orderBy(rows, dLenStr(1))
	return cols, limit(rows, p)
}

func qQ39(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"header", "partner", "length", "members"}
	var rows [][]fval
	byPath := map[string]int32{}
	for i := range g.Files {
		byPath[g.Files[i].Path()] = g.Files[i].ID
	}
	for _, c := range g.Cycles {
		fid, ok := byPath[c.APath()]
		if !ok {
			rows = append(rows, mkrow(fStr(c.APath()), fStr(c.BPath()),
				fI32(c.Length), fStr(c.Members())))
			continue
		}
		f := &g.Files[fid-1]
		if !likeMatch(g.modName(f.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(c.APath()), fStr(c.BPath()), fI32(c.Length),
			fStr(c.Members())))
	}
	orderBy(rows, cmpI(2), cmpS(0))
	return cols, limit(rows, p)
}

func qQ40(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "const_casts", "casts_total", "fan_in", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if g.rv(s).NConstCast <= 0 {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(g.rv(s).NConstCast),
			fI32(s.NCast), fI32(s.FanIn), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(3))
	return cols, limit(rows, p)
}

func qQ41(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "path", "line_start", "sloc", "addr_taken"}
	x := g.six()
	var rows [][]fval
	for _, s := range g.syms() {
		if s.Kind() != "function" || s.FanIn != 0 || s.IsTest != 0 {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		n := x.bodyInFile[s.ID]
		if s.IsStatic == 0 {
			n = x.bodyN[s.ID]
		}
		if n == 0 {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fStr(f.Path()), fI32(s.LineStart),
			fI32(s.Sloc), fI32(n)))
	}
	orderBy(rows, dI(4), dI(3), cmpS(0))
	return cols, limit(rows, p)
}

func qQ42(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "path", "line", "defined_count"}
	var rows [][]fval
	for _, d := range g.Decls {
		cnt := int32(0)
		for _, sid := range g.byName[d.Name()] {
			if sy := g.sym(sid); sy.Kind() == "function" &&
				(sy.IsStatic == 0 || sy.FileID == d.FileID) {
				cnt++
			}
		}
		if cnt != 0 {
			continue
		}
		f := g.fileOf2(d.FileID)
		if f == nil || f.IsGenerated != 0 {
			continue
		}
		if !likeMatch(g.modName(f.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(d.Name()), fStr(f.Path()), fI32(d.Line),
			fI32(0)))
	}
	orderBy(rows, cmpS(0), cmpS(1))
	return cols, limit(rows, p)
}

func qQ43(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "pairs", "io_calls", "fan_in", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if g.rv(s).NToctou <= 0 {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(g.rv(s).NToctou), fI32(s.NIO),
			fI32(s.FanIn), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(3))
	return cols, limit(rows, p)
}

func qQ44(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "acquires", "releases", "imbalance", "return_paths",
		"gotos", "fan_in", "cyclo", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.Kind() != "function" || g.rv(s).NLockAcquire <= g.rv(s).NLockRelease {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(g.rv(s).NLockAcquire),
			fI32(g.rv(s).NLockRelease), fI32(g.rv(s).NLockAcquire-g.rv(s).NLockRelease),
			fI32(s.NReturns), fI32(s.NGotos), fI32(s.FanIn),
			fI32(s.Cyclomatic), fStr(g.at(s))))
	}
	orderBy(rows, dI(3), dI(6), cmpS(0))
	return cols, limit(rows, p)
}

func qQ45(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "acquires", "io_calls", "mem_calls", "cyclo",
		"fan_in", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.Kind() != "function" || g.rv(s).NLockAcquire <= 0 || s.NIO <= 0 {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(g.rv(s).NLockAcquire), fI32(s.NIO),
			fI32(s.NMemory), fI32(s.Cyclomatic), fI32(s.FanIn), fStr(g.at(s))))
	}
	orderBy(rows, dI(2), dI(1), cmpS(0))
	return cols, limit(rows, p)
}

func qQ46(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"op", "dst", "src", "size_arg", "size_buf", "caller",
		"fan_in", "at"}
	want := []string{"memcpy", "memmove", "strncpy", "strncat", "snprintf"}
	var rows [][]fval
	for _, m := range g.Memops {
		if !containsStr(want, m.Fn()) || m.SizeBuf() == "" || m.SizeBuf() == m.DstTail() {
			continue
		}
		if strings.Contains(m.Src(), m.SizeBuf()) {
			continue
		}
		if strings.Contains(m.SizeArg(), "*") || strings.Contains(m.SizeArg(), "+") {
			continue
		}
		s := g.sym(m.SymbolID)
		if s == nil {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(m.Fn()), fStr(m.Dst()), fStr(m.Src()),
			fStr(m.SizeArg()), fStr(m.SizeBuf()), fStr(s.Name()), fI32(s.FanIn),
			fStr(f.Path()+":"+strconv.Itoa(int(m.Line)))))
	}
	orderBy(rows, dI(6), cmpLine(7), cmpS(0))
	return cols, limit(rows, p)
}

func qQ47(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"fn", "size_expr", "caller", "fan_in", "alloc_sites", "at"}
	var rows [][]fval
	for _, a := range g.Allocs {
		if strings.Contains(a.SizeExpr(), "sizeof") || a.SizeExpr() == "" {
			continue
		}
		s := g.sym(a.SymbolID)
		if s == nil {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(a.Fn()), fStr(a.SizeExpr()), fStr(s.Name()),
			fI32(s.FanIn), fI32(g.rv(s).NAllocsite),
			fStr(f.Path()+":"+strconv.Itoa(int(a.Line)))))
	}
	orderBy(rows, dI(3), cmpLine(5), cmpS(0))
	return cols, limit(rows, p)
}

func qQ48(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "writes", "module", "mutable_globals_here", "fan_in",
		"cyclo", "rank_in_module", "at"}
	mutable := map[int32]int32{}
	for _, gl := range g.Globals {
		if gl.IsConst == 0 {
			mutable[gl.FileID]++
		}
	}
	type w struct {
		id, mod int32
		writes  int32
	}

	var ws []w
	for _, s := range g.syms() {
		if s.Kind() != "function" || g.rv(s).NGlobalWrite <= 0 {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		ws = append(ws, w{s.ID, s.ModuleID, g.rv(s).NGlobalWrite})
	}
	type wrow struct {
		id, mod, writes, fan int32
		name                 string
	}
	wr := make([]wrow, 0, len(ws))
	for _, x := range ws {
		fan, nm := int32(0), ""
		if sy := g.sym(x.id); sy != nil {
			fan, nm = sy.FanIn, sy.Name()
		}
		wr = append(wr, wrow{x.id, x.mod, x.writes, fan, nm})
	}
	sort.SliceStable(wr, func(a, b int) bool {
		if wr[a].writes != wr[b].writes {
			return wr[a].writes > wr[b].writes
		}
		if wr[a].name != wr[b].name {
			return wr[a].name > wr[b].name
		}
		return wr[a].id < wr[b].id
	})

	rank := map[int32]int32{}
	for mi := range g.Modules {
		part := make([]wrow, 0, 8)
		for _, x := range wr {
			if x.mod == g.Modules[mi].ID {
				part = append(part, x)
			}
		}
		sort.SliceStable(part, func(a, b int) bool {
			if part[a].writes != part[b].writes {
				return part[a].writes > part[b].writes
			}
			return part[a].fan > part[b].fan
		})
		n := int32(0)
		var pw, pf int32
		first := true
		for _, x := range part {
			if first || x.writes != pw || x.fan != pf {
				n++
				pw, pf, first = x.writes, x.fan, false
			}
			rank[x.id] = n
		}
	}
	var rows [][]fval
	for _, x := range wr {
		s := g.sym(x.id)
		if s == nil {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(x.writes),
			fStr(g.modName(s.ModuleID)), fI32(mutable[f.ID]), fI32(s.FanIn),
			fI32(s.Cyclomatic), fI32(rank[x.id]), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(4))
	return cols, limit(rows, p)
}

type qw struct {
	id, mod, writes, fan int32
}

func qQ49(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "narrow_casts", "arith_ops", "shifts", "io_calls",
		"fan_in", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.Kind() != "function" || g.rv(s).NNarrowCast <= 0 {
			continue
		}
		if s.NArith <= 0 && s.NShift <= 0 && s.NIO <= 0 {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(g.rv(s).NNarrowCast),
			fI32(s.NArith), fI32(s.NShift), fI32(s.NIO), fI32(s.FanIn),
			fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(5), cmpS(0))
	return cols, limit(rows, p)
}

func qQ50(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "sign_cmps", "total_cmps", "n_params", "fan_in", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.Kind() != "function" || g.rv(s).NSignCmp <= 0 {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(g.rv(s).NSignCmp), fI32(s.NCmp),
			fI32(s.NParams), fI32(s.FanIn), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(4), cmpS(0))
	return cols, limit(rows, p)
}

func qQ51(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "fmt_calls", "stdio_calls", "fan_in", "via", "at"}
	want := []string{"vsnprintf", "vsprintf", "vfprintf", "vprintf", "syslog"}
	bySym := map[int32][]string{}
	for _, i := range g.hazRows(want) {
		h := &g.Hazards[i]
		if !containsStr(bySym[h.SymbolID], h.Pattern()) {
			bySym[h.SymbolID] = append(bySym[h.SymbolID], h.Pattern())
		}
	}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.Kind() != "function" || s.IsVariadic != 1 || g.rv(s).NVariadicFmt <= 0 {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		var via fval = fNull()
		if l := bySym[s.ID]; len(l) > 0 {
			via = fStr(groupConcat(l))
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(g.rv(s).NVariadicFmt),
			fI32(s.NStdio), fI32(s.FanIn), via, fStr(g.at(s))))
	}
	orderBy(rows, dI(3), dI(1), cmpS(0))
	return cols, limit(rows, p)
}

func qQ52(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "table_refs", "plain_refs", "tables_in", "sloc",
		"cyclo", "at"}
	x := g.six()
	vetoed := func(s *Symbol) bool {
		if s.IsStatic == 0 {
			return x.bodyN[s.ID] > 0
		}
		return x.bodyInFile[s.ID] > 0
	}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.Kind() != "function" || s.FanIn != 0 || vetoed(s) {
			continue
		}
		rs := x.refs[s.Name()]
		n := int32(0)
		plain := int32(0)
		var names []string
		for _, r := range rs {
			if s.IsStatic == 0 || r.file == s.FileID {
				n++
				if r.kind == "ref" {
					plain++
				}
				if f := g.fileOf2(r.file); f != nil {
					names = append(names, f.Basename())
				}
			}
		}
		if n == 0 {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(n), fI32(plain),
			fStr(groupConcat(distinctStrings(names))), fI32(s.Sloc),
			fI32(s.Cyclomatic), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(4), cmpS(0))
	return cols, limit(rows, p)
}

func qQ53(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "allocs", "frees", "return_paths", "branches",
		"gotos", "cyclo", "fan_in", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.Kind() != "function" || g.rv(s).NAllocsite <= 0 || g.rv(s).NFree <= 0 ||
			s.NReturns < 3 || s.NReturns <= g.rv(s).NAllocsite {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(g.rv(s).NAllocsite),
			fI32(g.rv(s).NFree), fI32(s.NReturns), fI32(s.NBranches), fI32(s.NGotos),
			fI32(s.Cyclomatic), fI32(s.FanIn), fStr(g.at(s))))
	}
	orderBy(rows, dI(3), dI(1), cmpS(0))
	return cols, limit(rows, p)
}

func qQ54(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "self_realloc", "allocs", "frees", "returns_",
		"fan_in", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if g.rv(s).NReallocSelf <= 0 || !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(g.rv(s).NReallocSelf),
			fI32(s.NAlloc), fI32(g.rv(s).NFree), fI32(s.NReturns), fI32(s.FanIn),
			fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(5))
	return cols, limit(rows, p)
}

func qQ55(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "fn", "size", "allocs", "fan_in", "at"}
	var rows [][]fval
	for _, a := range g.Allocs {
		if !strings.Contains(a.SizeExpr(), "strlen") ||
			strings.Contains(a.SizeExpr(), "+") {
			continue
		}
		s := g.sym(a.SymbolID)
		if s == nil || !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fStr(a.Fn()), fStr(a.SizeExpr()),
			fI32(s.NAlloc), fI32(s.FanIn),
			fStr(f.Path()+":"+strconv.Itoa(int(a.Line)))))
	}
	orderBy(rows, dI(4), cmpS(0))
	return cols, limit(rows, p)
}

func qQ56(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "opens", "closes", "returns_", "gotos", "cyclo",
		"fan_in", "at"}
	openFns := []string{"open", "openat", "creat", "fopen", "fdopen", "opendir",
		"socket", "accept", "accept4", "mmap", "popen", "tmpfile"}
	closeFns := []string{"close", "fclose", "closedir", "munmap", "pclose", "shutdown"}
	o := map[int32]int32{}
	c := map[int32]int32{}
	for _, i := range g.hazCats("io", "stdio") {
		h := &g.Hazards[i]
		if containsStr(openFns, h.Pattern()) {
			o[h.SymbolID] += h.N
		}
		if containsStr(closeFns, h.Pattern()) {
			c[h.SymbolID] += h.N
		}
	}
	var rows [][]fval
	sids := make([]int32, 0, len(o))
	for sid := range o {
		sids = append(sids, sid)
	}
	slices.Sort(sids)
	for _, sid := range sids {
		if o[sid] <= c[sid] {
			continue
		}
		s := g.sym(sid)
		if s == nil || !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(o[sid]), fI32(c[sid]),
			fI32(s.NReturns), fI32(s.NGotos), fI32(s.Cyclomatic),
			fI32(s.FanIn), fStr(g.at(s))))
	}
	orderBy(rows, dWeighted([][3]int32{{1, 1}, {-1, 2}}), dI(6))
	return cols, limit(rows, p)
}

func qQ57(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "allocs", "null_checks", "returns_", "cyclo",
		"fan_in", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.Kind() != "function" || s.NAlloc <= 0 || s.NNullCheck != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.NAlloc),
			fI32(s.NNullCheck), fI32(s.NReturns), fI32(s.Cyclomatic),
			fI32(s.FanIn), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(5), cmpS(0))
	return cols, limit(rows, p)
}

func qQ58(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "vlas", "io", "mem", "int_", "nest", "fan_in",
		"rec", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if g.rv(s).NVla <= 0 || (s.NIO <= 0 && s.NMemory <= 0 && s.IsRecursive != 1) {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(g.rv(s).NVla), fI32(s.NIO),
			fI32(s.NMemory), fI32(s.NInteger), fI32(s.MaxNesting),
			fI32(s.FanIn), fI32(s.IsRecursive), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(2), dI(6))
	return cols, limit(rows, p)
}

func qQ59(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "uses", "frees", "sloc", "returns_", "fan_in", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if g.rv(s).NFreeThenUse <= 0 || !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(g.rv(s).NFreeThenUse),
			fI32(g.rv(s).NFree), fI32(s.Sloc), fI32(s.NReturns), fI32(s.FanIn),
			fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(5))
	return cols, limit(rows, p)
}

func qQ60(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "weak_rand", "io", "exec", "fan_in", "exported", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if g.rv(s).NWeakRandom <= 0 || !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(g.rv(s).NWeakRandom),
			fI32(s.NIO), fI32(s.NExec), fI32(s.FanIn), fI32(s.IsPublic),
			fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(4))
	return cols, limit(rows, p)
}

func qQ61(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "var_shifts", "casts", "io", "int_", "cyclo",
		"fan_in", "at"}
	var rows [][]fval
	for _, s := range g.symsName() {
		if g.rv(s).NShiftVar <= 0 || !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(g.rv(s).NShiftVar), fI32(s.NCast),
			fI32(s.NIO), fI32(s.NInteger), fI32(s.Cyclomatic), fI32(s.FanIn),
			fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(3), dI(6))
	return cols, limit(rows, p)
}

func qQ62(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "errno_reads", "fallible_calls", "returns_", "cyclo",
		"fan_in", "at"}
	fallible := map[int32]int32{}
	for _, i := range g.hazCats("io", "stdio", "alloc", "memory") {
		fallible[g.Hazards[i].SymbolID] += g.Hazards[i].N
	}
	var rows [][]fval
	for _, s := range g.symsName() {
		if s.Kind() != "function" || g.rv(s).NErrno <= 0 {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(g.rv(s).NErrno),
			fI32(fallible[s.ID]), fI32(s.NReturns), fI32(s.Cyclomatic),
			fI32(s.FanIn), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(2))
	return cols, limit(rows, p)
}

func qQ63(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "env_reads", "mem", "exec", "io", "fan_in", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if g.rv(s).NGetenv <= 0 || !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(g.rv(s).NGetenv), fI32(s.NMemory),
			fI32(s.NExec), fI32(s.NIO), fI32(s.FanIn), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(3), dI(5))
	return cols, limit(rows, p)
}

func qQ64(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "bad_asserts", "sloc", "calls", "fan_in",
		"in_test_file", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if g.rv(s).NAssertSide <= 0 {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(g.rv(s).NAssertSide),
			fI32(s.Sloc), fI32(s.NCalls), fI32(s.FanIn), fI32(f.IsTest),
			fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(4))
	return cols, limit(rows, p)
}

func qQ65(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"lock_a", "lock_b", "a_first_fns", "b_first_fns", "a_first",
		"b_first"}
	type pair struct{ lo, hi string }
	byPair := map[pair]*lockPair{}
	per := map[int32][]int32{}
	for i, l := range g.Locks {
		per[l.SymbolID] = append(per[l.SymbolID], int32(i))
	}
	sids := make([]int32, 0, len(per))
	for sid := range per {
		sids = append(sids, sid)
	}
	slices.Sort(sids)
	for _, sid := range sids {
		idxs := per[sid]
		for _, a := range idxs {
			for _, b := range idxs {
				if g.Locks[a].Name() == g.Locks[b].Name() ||
					g.Locks[a].Line >= g.Locks[b].Line {
					continue
				}
				lo, hi := g.Locks[a].Name(), g.Locks[b].Name()
				dirFirst := 0
				if hi < lo {
					lo, hi = hi, lo
					dirFirst = 1
				}
				k := pair{lo, hi}
				lp := byPair[k]
				if lp == nil {
					lp = newLockPair()
					byPair[k] = lp
				}
				nm := ""
				if s := g.sym(sid); s != nil {
					nm = s.Name()
				}
				if dirFirst == 0 {
					lp.aFns[sid] = true
					lp.aNames = appendDistinct(lp.aNames, nm)
				} else {
					lp.bFns[sid] = true
					lp.bNames = appendDistinct(lp.bNames, nm)
				}
			}
		}
	}
	keys := make([]pair, 0, len(byPair))
	for k := range byPair {
		keys = append(keys, k)
	}
	sort.Slice(keys, func(i, j int) bool {
		if keys[i].lo != keys[j].lo {
			return keys[i].lo < keys[j].lo
		}
		return keys[i].hi < keys[j].hi
	})
	var rows [][]fval
	for _, k := range keys {
		lp := byPair[k]
		af, bf := int32(len(lp.aFns)), int32(len(lp.bFns))
		if af == 0 || bf == 0 {
			continue
		}
		modOK := false
		for sid := range lp.aFns {
			if s := g.sym(sid); s != nil && likeMatch(g.modName(s.ModuleID), p.mod) {
				modOK = true
			}
		}
		for sid := range lp.bFns {
			if s := g.sym(sid); s != nil && likeMatch(g.modName(s.ModuleID), p.mod) {
				modOK = true
			}
		}
		if !modOK {
			continue
		}
		rows = append(rows, mkrow(fStr(k.lo), fStr(k.hi), fI32(af), fI32(bf),
			fStr(groupConcat(lp.aNames)), fStr(groupConcat(lp.bNames))))
	}
	orderBy(rows, dSum(2, 3), dI(2))
	return cols, limit(rows, p)
}

type lockPair struct {
	aFns, bFns     map[int32]bool
	aNames, bNames []string
}

func newLockPair() *lockPair {
	return &lockPair{aFns: map[int32]bool{}, bFns: map[int32]bool{}}
}

func appendDistinct(l []string, s string) []string {
	if !containsStr(l, s) {
		return append(l, s)
	}
	return l
}

func qQ66(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"struct_name", "sharing_fields", "cache_line", "first_off",
		"last_off", "size", "pad", "conc_fns_in_module", "at"}
	modc := g.perMod()
	for i := range g.Symbols {
		if g.Symbols[i].NConcurrency > 0 {
			modc.add(g.Symbols[i].ModuleID, 1)
		}
	}
	type grp struct {
		n, lo, hi int32
	}
	byKey := map[int64]*grp{}
	for _, l := range g.Layout {
		if l.InUnion != 0 || l.Depth != 0 || l.ByteOff < 0 || l.Exact != 1 {
			continue
		}
		k := int64(l.SymbolID)<<32 | int64(l.ByteOff/64)
		e := byKey[k]
		if e == nil {
			e = &grp{lo: l.ByteOff, hi: l.ByteOff}
			byKey[k] = e
		}
		e.n++
		if l.ByteOff < e.lo {
			e.lo = l.ByteOff
		}
		if l.ByteOff > e.hi {
			e.hi = l.ByteOff
		}
	}
	size, pad := g.perSym(), g.perSym()
	for i := range g.SSize {
		if ss := &g.SSize[i]; uint32(ss.SymbolID) < uint32(len(size)) {
			size[ss.SymbolID], pad[ss.SymbolID] = ss.TotalSize, ss.TotalPad
		}
	}
	var keys []int64
	for k, v := range byKey {
		if v.n >= 2 {
			keys = append(keys, k)
		}
	}
	slices.Sort(keys)
	var rows [][]fval
	for _, k := range keys {
		sid := int32(k >> 32)
		s := g.sym(sid)
		if s == nil || !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		e := byKey[k]
		rows = append(rows, mkrow(fStr(s.Name()), fI32(e.n), fI32(int32(k&0xffffffff)),
			fI32(e.lo), fI32(e.hi), fI32(size.at(sid)), fI32(pad.at(sid)),
			fI32(modc.at(s.ModuleID)), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(7), dI(5))
	return cols, limit(rows, p)
}

func qQ67(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "fn", "dst", "size", "fan_in", "at"}
	var rows [][]fval
	for _, m := range g.Memops {
		if m.Fn() != "strncpy" && m.Fn() != "strncat" {
			continue
		}
		if !strings.Contains(m.SizeArg(), "sizeof") {
			continue
		}
		s := g.sym(m.SymbolID)
		if s == nil || !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fStr(m.Fn()), fStr(m.Dst()),
			fStr(m.SizeArg()), fI32(s.FanIn),
			fStr(f.Path()+":"+strconv.Itoa(int(m.Line)))))
	}
	orderBy(rows, dI(4), cmpS(0))
	return cols, limit(rows, p)
}

func qQ68(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"struct_name", "size", "pad", "pct_pad", "copied_whole", "at"}
	copied := g.perSym()
	for i := range g.Memops {
		if m := &g.Memops[i]; m.Fn() == "memcpy" || m.Fn() == "memmove" || m.Fn() == "memset" {
			copied.add(m.SymbolID, 1)
		}
	}
	var rows [][]fval
	for _, ss := range g.SSize {
		if ss.TotalPad <= 0 || ss.Exact != 1 {
			continue
		}
		s := g.sym(ss.SymbolID)
		if s == nil || !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		cw := int32(0)
		if copied.at(ss.SymbolID) > 0 {
			cw = 1
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(ss.TotalSize),
			fI32(ss.TotalPad), fI32(pct(int64(ss.TotalPad), int64(ss.TotalSize))),
			fI32(cw), fStr(g.at(s))))
	}
	orderBy(rows, dI(2), dI(4), dI(1))
	return cols, limit(rows, p)
}

// namedAsValue returns a test for "this function's name is used as a value
// somewhere it is visible": passed as a callback, stored in a field, or listed
// in a file-scope table. A static function is only visible in its own file,
// unless that file is a header.
func (g *Graph) namedAsValue() func(*Symbol) bool {
	x := g.six()
	return func(s *Symbol) bool {
		if !x.addrAny[s.ID] {
			return false
		}
		if s.IsStatic == 0 || x.addrInFile[s.ID] {
			return true
		}
		f := g.fileOf(s.ID)
		return f != nil && strings.HasPrefix(f.Ext(), ".h")
	}
}

func qQ69(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "sloc", "cyclo", "calls", "only_file", "at"}
	named := g.namedAsValue()
	var rows [][]fval
	for _, s := range g.syms() {
		if s.Kind() != "function" || s.IsStatic != 1 || s.FanIn != 0 {
			continue
		}
		if named(s) || !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.Sloc), fI32(s.Cyclomatic),
			fI32(s.NCalls), fStr(f.Path()), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(2), cmpS(0))
	return cols, limit(rows, p)
}

func qQ70(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "fan_in", "sloc", "external", "at"}
	declN := g.six().declN
	var rows [][]fval
	for _, s := range g.syms() {
		if s.Kind() != "function" || s.IsStatic != 0 || s.IsInline != 0 {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.Ext() != ".c" || declN[s.ID] > 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.FanIn), fI32(s.Sloc),
			fI32(s.IsPublic), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(2), cmpS(0))
	return cols, limit(rows, p)
}

func qQ71(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"path", "sloc", "syms", "includes", "included_by"}
	inc, incBy, guarded := g.perFile(), g.perFile(), g.perFile()
	for i := range g.Imports {
		im := &g.Imports[i]
		inc.add(im.FileID, 1)
		if im.HasTargetID {
			incBy.add(im.TargetID, 1)
		}
	}
	for i := range g.Cfgs {
		if cb := &g.Cfgs[i]; cb.Line <= 12 && cb.Directive() == "ifndef" {
			guarded.add(cb.FileID, 1)
		}
	}
	var rows [][]fval
	for i := range g.Files {
		f := &g.Files[i]
		if f.Ext() != ".h" || guarded.at(f.ID) > 0 {
			continue
		}
		if !likeMatch(g.modName(f.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(f.Path()), fI32(f.Sloc), fI32(f.NSymbols),
			fI32(inc.at(f.ID)), fI32(incBy.at(f.ID))))
	}
	orderBy(rows, dI(4), dI(1))
	return cols, limit(rows, p)
}

func qQ72(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"from_module", "to_module", "hops_out", "hops_back",
		"out_edges", "back_edges"}
	// the distinct module-to-module edges. A table of nm x nm flags answers
	// "seen this pair?" for all but absurd module counts.
	medge := map[[2]int32]bool{}
	nm := len(g.Modules) + 1
	var flags []bool
	if nm <= 4096 {
		flags = make([]bool, nm*nm)
	}
	first := func(k [2]int32) bool {
		if flags != nil && uint32(k[0]) < uint32(nm) && uint32(k[1]) < uint32(nm) {
			i := int(k[0])*nm + int(k[1])
			was := flags[i]
			flags[i] = true
			return !was
		}
		was := medge[k]
		medge[k] = true
		return !was
	}
	deg := map[int32]int32{}
	var direct [][2]int32
	for i := range g.Edges {
		e := &g.Edges[i]
		a, b := g.sym(e.CallerID), g.sym(e.CalleeID)
		if a == nil || b == nil || a.ModuleID == 0 || b.ModuleID == 0 ||
			a.ModuleID == b.ModuleID {
			continue
		}
		if k := [2]int32{a.ModuleID, b.ModuleID}; first(k) {
			direct = append(direct, k)
			deg[a.ModuleID]++
		}
	}

	// successors by module: stepping every reached pair against the whole
	// edge list is pairs x edges, which is fine at 139 modules and not at
	// the few thousand a kernel-sized tree has
	succ := map[int32][]int32{}
	for _, e := range direct {
		succ[e[0]] = append(succ[e[0]], e[1])
	}

	best := map[[2]int32]int32{}
	for _, k := range direct {
		best[k] = 1
	}

	cur := map[[2]int32]int32{}
	maps.Copy(cur, best)
	for depth := int32(2); depth <= 4; depth++ {
		next := map[[2]int32]int32{}
		for k, d := range cur {
			if d != depth-1 {
				continue
			}
			for _, to := range succ[k[1]] {
				nk := [2]int32{k[0], to}
				if _, ok := best[nk]; !ok {
					next[nk] = depth
				}
			}
		}
		maps.Copy(best, next)
		cur = next
		if len(next) == 0 {
			break
		}
	}
	type pair struct{ a, b int32 }
	seen := map[pair]bool{}
	var keys []pair
	for k := range best {
		if k[0] < k[1] {
			pk := pair{k[0], k[1]}
			if !seen[pk] {
				seen[pk] = true
				keys = append(keys, pk)
			}
		}
	}
	sort.Slice(keys, func(i, j int) bool {
		if keys[i].a != keys[j].a {
			return keys[i].a < keys[j].a
		}
		return keys[i].b < keys[j].b
	})
	var rows [][]fval
	for _, k := range keys {
		out, ok1 := best[[2]int32{k.a, k.b}]
		back, ok2 := best[[2]int32{k.b, k.a}]
		if !ok1 || !ok2 {
			continue
		}
		if !likeMatch(g.modName(k.a), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(g.modName(k.a)), fStr(g.modName(k.b)),
			fI32(out), fI32(back), fI32(deg[k.a]), fI32(deg[k.b])))
	}
	orderBy(rows, dSum(2, 3), dI(4))
	return cols, limit(rows, p)
}

func qQ73(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "dominant_prefix", "prefix_users", "fan_in", "sloc", "at"}
	// A file has a handful of distinct prefixes, so each file keeps a short
	// list of them rather than a map: no hashing, nothing allocated per
	// function.
	type pc struct {
		pr string
		n  int32
	}
	prefixOf := func(s *Symbol) string {
		nm := s.Name()
		if u := strings.IndexByte(nm, '_'); u >= 1 {
			return nm[:u+1]
		}
		return ""
	}
	nf := len(g.Files) + 1
	counts := make([][]pc, nf)
syms:
	for _, s := range g.Symbols {
		if s.Kind() != "function" || s.FileID < 0 || int(s.FileID) >= nf {
			continue
		}
		pr := prefixOf(s)
		if pr == "" {
			continue
		}
		l := counts[s.FileID]
		for i := range l {
			if l[i].pr == pr {
				l[i].n++
				continue syms
			}
		}
		counts[s.FileID] = append(l, pc{pr, 1})
	}
	dom := make([]pc, nf)
	for fid, l := range counts {
		for _, c := range l {
			if c.n > dom[fid].n || (c.n == dom[fid].n && c.pr > dom[fid].pr) {
				dom[fid] = c
			}
		}
	}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.Kind() != "function" || s.FileID < 0 || int(s.FileID) >= nf {
			continue
		}
		pr := prefixOf(s)
		d := dom[s.FileID]
		if pr == "" || pr == d.pr || d.n < 3 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fStr(d.pr), fI32(d.n),
			fI32(s.FanIn), fI32(s.Sloc), fStr(g.at(s))))
	}
	orderBy(rows, dI(2), dI(3))
	return cols, limit(rows, p)
}

func qQ74(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "fan_in", "cyclo", "sloc", "module", "at"}
	untested := make([]bool, len(g.Symbols)+1)
	for i := range g.Edges {
		e := &g.Edges[i]
		cs := g.sym(e.CallerID)
		if cs == nil || e.CalleeID < 0 || int(e.CalleeID) >= len(untested) {
			continue
		}
		if f := g.fileOf(cs.ID); (f != nil && f.IsTest == 1) || cs.IsTest == 1 {
			untested[e.CalleeID] = true
		}
	}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.Kind() != "function" || s.FanIn < 3 {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 || s.IsTest != 0 || untested[s.ID] {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		m := g.modName(s.ModuleID)
		if m == "" {
			m = "(none)"
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.FanIn),
			fI32(s.Cyclomatic), fI32(s.Sloc), fStr(m), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(2), cmpS(0))
	return cols, limit(rows, p)
}

func qQ75(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "io_calls", "depth", "loops", "calls", "fan_in", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.IOInLoop <= 0 || !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.IOInLoop),
			fI32(s.MaxLoopDepth), fI32(s.NLoops), fI32(s.CallInLoop),
			fI32(s.FanIn), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(2))
	return cols, limit(rows, p)
}

func qQ76(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "lock_ops", "depth", "acquires", "releases",
		"fan_in", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.LockInLoop <= 0 || !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.LockInLoop),
			fI32(s.MaxLoopDepth), fI32(g.rv(s).NLockAcquire), fI32(g.rv(s).NLockRelease),
			fI32(s.FanIn), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(2))
	return cols, limit(rows, p)
}

func qQ77(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "branches", "depth", "switches", "calls", "sloc",
		"fan_in", "at"}
	var rows [][]fval
	for _, s := range g.symsName() {
		if s.BranchInLoop <= 0 || s.MaxLoopDepth < 1 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.BranchInLoop),
			fI32(s.MaxLoopDepth), fI32(g.rv(s).SwitchInLoop), fI32(s.CallInLoop),
			fI32(s.Sloc), fI32(s.FanIn), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(2))
	return cols, limit(rows, p)
}

func simpleQ(cols []string, keep func(s *Symbol) bool, g *Graph, p params,
	fill func(s *Symbol) []fval, keys ...ordKey) ([]string, [][]fval) {
	var rows [][]fval
	for _, s := range g.syms() {
		if !keep(s) {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, fill(s))
	}
	orderBy(rows, then(keys...), cmpS(0))
	return cols, limit(rows, p)
}

func qQ78(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "et_regs", "epoll_calls", "kqueue_calls",
		"eagain_checks", "loops", "cyclo", "fan_in", "at"}
	return simpleQ(cols, func(s *Symbol) bool { return g.rv(s).NEtReg > 0 && g.rv(s).NEagain == 0 },
		g, p, func(s *Symbol) []fval {
			return mkrow(fStr(s.Name()), fI32(g.rv(s).NEtReg), fI32(g.rv(s).NEpoll),
				fI32(g.rv(s).NKqueue), fI32(g.rv(s).NEagain), fI32(s.NLoops),
				fI32(s.Cyclomatic), fI32(s.FanIn), fStr(g.at(s)))
		}, cmpI(1), cmpI(6))
}

func qQ79(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "waits", "epoll_calls", "uring_calls",
		"kqueue_calls", "eintr_checks", "loops", "cyclo", "fan_in", "at"}
	return simpleQ(cols, func(s *Symbol) bool { return g.rv(s).NEventWait > 0 && g.rv(s).NEintr == 0 },
		g, p, func(s *Symbol) []fval {
			return mkrow(fStr(s.Name()), fI32(g.rv(s).NEventWait), fI32(g.rv(s).NEpoll),
				fI32(g.rv(s).NUring), fI32(g.rv(s).NKqueue), fI32(g.rv(s).NEintr), fI32(s.NLoops),
				fI32(s.Cyclomatic), fI32(s.FanIn), fStr(g.at(s)))
		}, cmpI(1), cmpI(7))
}

func qQ80(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "write_ready", "epoll_calls", "kqueue_calls",
		"deregistrations", "loops", "fan_in", "at"}
	return simpleQ(cols, func(s *Symbol) bool {
		return g.rv(s).NWriteReady > 0 && g.rv(s).NDereg == 0
	}, g, p, func(s *Symbol) []fval {
		return mkrow(fStr(s.Name()), fI32(g.rv(s).NWriteReady), fI32(g.rv(s).NEpoll),
			fI32(g.rv(s).NKqueue), fI32(g.rv(s).NDereg), fI32(s.NLoops), fI32(s.FanIn),
			fStr(g.at(s)))
	}, dI(1), dI(6))
}

func qQ81(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "epoll_calls", "kqueue_calls", "err_flags", "waits",
		"cyclo", "fan_in", "at"}
	return simpleQ(cols, func(s *Symbol) bool {
		return (g.rv(s).NEpoll > 0 || g.rv(s).NKqueue > 0) && g.rv(s).NErrFlag == 0
	}, g, p, func(s *Symbol) []fval {
		return mkrow(fStr(s.Name()), fI32(g.rv(s).NEpoll), fI32(g.rv(s).NKqueue),
			fI32(g.rv(s).NErrFlag), fI32(g.rv(s).NEventWait), fI32(s.Cyclomatic),
			fI32(s.FanIn), fStr(g.at(s)))
	}, dSum(1, 2),
		dI(6))
}

func qQ82(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "oneshot_regs", "rearms", "epoll_calls",
		"kqueue_calls", "cyclo", "fan_in", "at"}
	return simpleQ(cols, func(s *Symbol) bool {
		return g.rv(s).NOneshotReg > 0 && g.rv(s).NRearm == 0
	}, g, p, func(s *Symbol) []fval {
		return mkrow(fStr(s.Name()), fI32(g.rv(s).NOneshotReg), fI32(g.rv(s).NRearm),
			fI32(g.rv(s).NEpoll), fI32(g.rv(s).NKqueue), fI32(s.Cyclomatic), fI32(s.FanIn),
			fStr(g.at(s)))
	}, cmpI(1), cmpI(6))
}

func qQ83(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "uring_calls", "res_checks", "waits", "errno_reads",
		"cyclo", "fan_in", "at"}
	return simpleQ(cols, func(s *Symbol) bool { return g.rv(s).NUring > 0 && g.rv(s).NUringRes == 0 },
		g, p, func(s *Symbol) []fval {
			return mkrow(fStr(s.Name()), fI32(g.rv(s).NUring), fI32(g.rv(s).NUringRes),
				fI32(g.rv(s).NEventWait), fI32(g.rv(s).NErrno), fI32(s.Cyclomatic),
				fI32(s.FanIn), fStr(g.at(s)))
		}, cmpI(1), cmpI(6))
}

func qQ84(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "uring_calls", "errno_reads", "res_checks", "cyclo",
		"fan_in", "at"}
	return simpleQ(cols, func(s *Symbol) bool { return g.rv(s).NUring > 0 && g.rv(s).NErrno > 0 },
		g, p, func(s *Symbol) []fval {
			return mkrow(fStr(s.Name()), fI32(g.rv(s).NUring), fI32(g.rv(s).NErrno),
				fI32(g.rv(s).NUringRes), fI32(s.Cyclomatic), fI32(s.FanIn),
				fStr(g.at(s)))
		}, cmpI(1), cmpI(2))
}

func qQ85(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "uring_calls", "sqpoll_setups", "frees", "allocs",
		"cyclo", "at"}
	return simpleQ(cols, func(s *Symbol) bool {
		return g.rv(s).NUring > 0 && g.rv(s).NUringSqpoll > 0 && g.rv(s).NFree > 0
	}, g, p, func(s *Symbol) []fval {
		return mkrow(fStr(s.Name()), fI32(g.rv(s).NUring), fI32(g.rv(s).NUringSqpoll),
			fI32(g.rv(s).NFree), fI32(s.NAlloc), fI32(s.Cyclomatic), fStr(g.at(s)))
	}, cmpI(2), cmpI(3))
}

func qQ86(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "ring_touches", "uring_calls", "barriers", "cyclo",
		"fan_in", "at"}
	return simpleQ(cols, func(s *Symbol) bool {
		return g.rv(s).NUringRing > 0 && g.rv(s).NUringBarrier == 0
	}, g, p, func(s *Symbol) []fval {
		return mkrow(fStr(s.Name()), fI32(g.rv(s).NUringRing), fI32(g.rv(s).NUring),
			fI32(g.rv(s).NUringBarrier), fI32(s.Cyclomatic), fI32(s.FanIn),
			fStr(g.at(s)))
	}, cmpI(1), cmpI(5))
}

func qQ87(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "waits", "allocs", "allocs_in_loop", "loop_depth",
		"calls_in_loop", "cyclo", "fan_in", "at"}
	return simpleQ(cols, func(s *Symbol) bool { return g.rv(s).NEventWait > 0 && s.NAlloc > 0 },
		g, p, func(s *Symbol) []fval {
			return mkrow(fStr(s.Name()), fI32(g.rv(s).NEventWait), fI32(s.NAlloc),
				fI32(s.AllocInLoop), fI32(s.MaxLoopDepth), fI32(s.CallInLoop),
				fI32(s.Cyclomatic), fI32(s.FanIn), fStr(g.at(s)))
		}, cmpI(2), cmpI(3))
}

func qQ88(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "indefinite", "zero_tmo", "waits", "epoll_calls",
		"kqueue_calls", "loops", "cyclo", "at"}
	return simpleQ(cols, func(s *Symbol) bool {
		return g.rv(s).NEvTimeoutIndefinite > 0 || g.rv(s).NEvTimeoutZero > 0
	}, g, p, func(s *Symbol) []fval {
		return mkrow(fStr(s.Name()), fI32(g.rv(s).NEvTimeoutIndefinite),
			fI32(g.rv(s).NEvTimeoutZero), fI32(g.rv(s).NEventWait), fI32(g.rv(s).NEpoll),
			fI32(g.rv(s).NKqueue), fI32(s.NLoops), fI32(s.Cyclomatic), fStr(g.at(s)))
	}, dSum(1, 2), cmpI(7))
}

func qQ89(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "et_regs", "nonblock", "eagain_checks",
		"epoll_calls", "kqueue_calls", "cyclo", "fan_in", "at"}
	return simpleQ(cols, func(s *Symbol) bool { return g.rv(s).NEtReg > 0 && g.rv(s).NNonblockSet == 0 },
		g, p, func(s *Symbol) []fval {
			return mkrow(fStr(s.Name()), fI32(g.rv(s).NEtReg), fI32(g.rv(s).NNonblockSet),
				fI32(g.rv(s).NEagain), fI32(g.rv(s).NEpoll), fI32(g.rv(s).NKqueue),
				fI32(s.Cyclomatic), fI32(s.FanIn), fStr(g.at(s)))
		}, cmpI(1), cmpI(6))
}

func qQ90(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "creates", "destroys", "epoll_calls", "uring_calls",
		"kqueue_calls", "returns_", "cyclo", "fan_in", "at"}
	return simpleQ(cols, func(s *Symbol) bool {
		return g.rv(s).NEventCreate > 0 && g.rv(s).NEventDestroy == 0
	}, g, p, func(s *Symbol) []fval {
		return mkrow(fStr(s.Name()), fI32(g.rv(s).NEventCreate), fI32(g.rv(s).NEventDestroy),
			fI32(g.rv(s).NEpoll), fI32(g.rv(s).NUring), fI32(g.rv(s).NKqueue), fI32(s.NReturns),
			fI32(s.Cyclomatic), fI32(s.FanIn), fStr(g.at(s)))
	}, dI(1), dI(8))
}

func qQ91(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "sqe_gets", "null_checks", "uring_calls", "cyclo",
		"fan_in", "at"}
	return simpleQ(cols, func(s *Symbol) bool { return g.rv(s).NUringSqe > 0 && s.NNullCheck == 0 },
		g, p, func(s *Symbol) []fval {
			return mkrow(fStr(s.Name()), fI32(g.rv(s).NUringSqe), fI32(s.NNullCheck),
				fI32(g.rv(s).NUring), fI32(s.Cyclomatic), fI32(s.FanIn), fStr(g.at(s)))
		}, cmpI(1), cmpI(5))
}

func qQ92(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "cqe_seen", "waits", "uring_calls", "sqe_gets",
		"cyclo", "fan_in", "at"}
	return simpleQ(cols, func(s *Symbol) bool {
		return g.rv(s).NUring > 0 && g.rv(s).NEventWait > 0 && g.rv(s).NUringSeen == 0
	}, g, p, func(s *Symbol) []fval {
		return mkrow(fStr(s.Name()), fI32(g.rv(s).NUringSeen), fI32(g.rv(s).NEventWait),
			fI32(g.rv(s).NUring), fI32(g.rv(s).NUringSqe), fI32(s.Cyclomatic),
			fI32(s.FanIn), fStr(g.at(s)))
	}, cmpI(2), cmpI(3))
}

func qQ93(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "uncorrelated", "sqe_gets", "user_data_sets",
		"waits", "cyclo", "fan_in", "at"}
	return simpleQ(cols, func(s *Symbol) bool {
		return g.rv(s).NUringSqe >= 2 && g.rv(s).NUringSqe > g.rv(s).NUringUdata
	}, g, p, func(s *Symbol) []fval {
		return mkrow(fStr(s.Name()), fI32(g.rv(s).NUringSqe-g.rv(s).NUringUdata),
			fI32(g.rv(s).NUringSqe), fI32(g.rv(s).NUringUdata), fI32(g.rv(s).NEventWait),
			fI32(s.Cyclomatic), fI32(s.FanIn), fStr(g.at(s)))
	}, cmpI(1), cmpI(2))
}

func qQ94(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "stream_ops", "linked", "sqe_gets", "loop_depth",
		"cyclo", "fan_in", "at"}
	return simpleQ(cols, func(s *Symbol) bool {
		return g.rv(s).NUringStreamOps > 0 && g.rv(s).NUringLink == 0
	}, g, p, func(s *Symbol) []fval {
		return mkrow(fStr(s.Name()), fI32(g.rv(s).NUringStreamOps), fI32(g.rv(s).NUringLink),
			fI32(g.rv(s).NUringSqe), fI32(s.MaxLoopDepth), fI32(s.Cyclomatic),
			fI32(s.FanIn), fStr(g.at(s)))
	}, cmpI(1), cmpI(6))
}

func qQ95(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "ring_inits", "exits", "uring_calls", "any_release",
		"cyclo", "fan_in", "at"}
	return simpleQ(cols, func(s *Symbol) bool {
		return g.rv(s).NUring > 0 && g.rv(s).NEventCreate > 0 && g.rv(s).NUringTeardown == 0
	}, g, p, func(s *Symbol) []fval {
		return mkrow(fStr(s.Name()), fI32(g.rv(s).NEventCreate), fI32(g.rv(s).NUringTeardown),
			fI32(g.rv(s).NUring), fI32(g.rv(s).NEventDestroy), fI32(s.Cyclomatic),
			fI32(s.FanIn), fStr(g.at(s)))
	}, cmpI(1), cmpI(2))
}

func qQ96(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "timers", "zero_data", "kqueue_calls", "cyclo",
		"fan_in", "at"}
	return simpleQ(cols, func(s *Symbol) bool { return g.rv(s).NKqTimerZeroData > 0 },
		g, p, func(s *Symbol) []fval {
			return mkrow(fStr(s.Name()), fI32(g.rv(s).NKqTimer), fI32(g.rv(s).NKqTimerZeroData),
				fI32(g.rv(s).NKqueue), fI32(s.Cyclomatic), fI32(s.FanIn), fStr(g.at(s)))
		}, cmpI(2), cmpI(5))
}

func qQ97(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "receipt_batches", "err_flags", "kqueue_calls",
		"cyclo", "fan_in", "at"}
	return simpleQ(cols, func(s *Symbol) bool { return g.rv(s).NKqReceipt > 0 && g.rv(s).NErrFlag == 0 },
		g, p, func(s *Symbol) []fval {
			return mkrow(fStr(s.Name()), fI32(g.rv(s).NKqReceipt), fI32(g.rv(s).NErrFlag),
				fI32(g.rv(s).NKqueue), fI32(s.Cyclomatic), fI32(s.FanIn), fStr(g.at(s)))
		}, cmpI(1), cmpI(5))
}

func qQ98(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "waits", "io_in_loop", "lock_in_loop", "calls_in_loop",
		"loop_depth", "cyclo", "fan_in", "at"}
	return simpleQ(cols, func(s *Symbol) bool {
		return g.rv(s).NEventWait > 0 && (s.IOInLoop > 0 || s.LockInLoop > 0)
	}, g, p, func(s *Symbol) []fval {
		return mkrow(fStr(s.Name()), fI32(g.rv(s).NEventWait), fI32(s.IOInLoop),
			fI32(s.LockInLoop), fI32(s.CallInLoop), fI32(s.MaxLoopDepth),
			fI32(s.Cyclomatic), fI32(s.FanIn), fStr(g.at(s)))
	}, dSum(2, 3), cmpI(7))
}

func (g *Graph) apiUseIndex() map[int32][]int32 {
	if g.apiByID == nil {
		idx := make(map[int32][]int32, len(g.Symbols)/8+1)
		for i := range g.APIUses {
			id := g.APIUses[i].SymbolID
			idx[id] = append(idx[id], int32(i))
		}
		g.apiByID = idx
	}
	return g.apiByID
}

func hasAPIUse(g *Graph, sid int32, pred func(a *APIUse) bool) bool {
	if g.apiByID == nil {
		g.apiUseIndex()
	}
	for _, i := range g.apiByID[sid] {
		if pred(&g.APIUses[i]) {
			return true
		}
	}
	return false
}

func fnIs(names ...string) func(a *APIUse) bool {
	return func(a *APIUse) bool { return containsStr(names, a.Fn()) }
}

func apiQ(cols []string, fns []string, keep func(s *Symbol) bool, g *Graph, p params,
	fill func(s *Symbol) []fval, keys ...ordKey) ([]string, [][]fval) {
	// mark the few symbols that call one of fns, then scan: asking each of
	// the tree's symbols for its API uses was a map lookup apiece
	if g.apiByFn == nil {
		g.apiByFn = map[string][]int32{}
		for i := range g.APIUses {
			u := &g.APIUses[i]
			g.apiByFn[u.Fn()] = append(g.apiByFn[u.Fn()], u.SymbolID)
		}
	}
	if len(g.symMark) != len(g.Symbols)+1 {
		g.symMark = make([]bool, len(g.Symbols)+1)
	}
	mark := g.symMark
	var marked []int32
	for _, fn := range fns {
		for _, id := range g.apiByFn[fn] {
			if id > 0 && int(id) < len(mark) && !mark[id] {
				mark[id] = true
				marked = append(marked, id)
			}
		}
	}
	defer func() {
		for _, id := range marked {
			mark[id] = false
		}
	}()
	var rows [][]fval
	for _, s := range g.syms() {
		if !mark[s.ID] || !keep(s) {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, fill(s))
	}
	orderBy(rows, then(keys...), cmpS(0))
	return cols, limit(rows, p)
}

func qQ99(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "sloc", "cyclo", "fan_in", "at"}
	return apiQ(cols, []string{"snprintf", "vsnprintf"},
		func(s *Symbol) bool { return g.rv(s).NRetNegCheck == 0 }, g, p,
		func(s *Symbol) []fval {
			return mkrow(fStr(s.Name()), fI32(s.Sloc), fI32(s.Cyclomatic),
				fI32(s.FanIn), fStr(g.at(s)))
		}, dI(1), dI(3))
}

func qQ100(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "sloc", "cyclo", "fan_in", "at"}
	return apiQ(cols, []string{"fread", "fwrite", "fgets", "fscanf", "vfscanf",
		"fprintf", "fputs", "fputc"},
		func(s *Symbol) bool { return g.rv(s).NRetNegCheck == 0 && s.NNullCheck == 0 },
		g, p, func(s *Symbol) []fval {
			return mkrow(fStr(s.Name()), fI32(s.Sloc), fI32(s.Cyclomatic),
				fI32(s.FanIn), fStr(g.at(s)))
		}, dI(1), dI(3))
}

func qQ101(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "errno_reads", "endptr_checks", "cyclo", "fan_in", "at"}
	return apiQ(cols, []string{"strtol", "strtoll", "strtoul", "strtoull",
		"strtod", "strtof", "strtold", "strtoimax", "strtoumax"},
		func(s *Symbol) bool { return !(g.rv(s).NErrnoZero > 0 && g.rv(s).NEndptr > 0) },
		g, p, func(s *Symbol) []fval {
			return mkrow(fStr(s.Name()), fI32(g.rv(s).NErrno), fI32(g.rv(s).NEndptr),
				fI32(s.Cyclomatic), fI32(s.FanIn), fStr(g.at(s)))
		}, dI(3), dI(4))
}

func qQ102(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "cyclo", "params", "fan_in", "at"}
	return apiQ(cols, []string{"isalnum", "isalpha", "isblank", "iscntrl",
		"isdigit", "isgraph", "islower", "isprint", "ispunct", "isspace",
		"isupper", "isxdigit", "tolower", "toupper"},
		func(s *Symbol) bool { return g.rv(s).NUcharCast == 0 }, g, p,
		func(s *Symbol) []fval {
			return mkrow(fStr(s.Name()), fI32(s.Cyclomatic), fI32(s.NParams),
				fI32(s.FanIn), fStr(g.at(s)))
		}, dI(3), dI(1))
}

func qQ103(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "loops", "cyclo", "fan_in", "at"}
	return apiQ(cols, []string{"nanosleep", "usleep", "sleep"},
		func(s *Symbol) bool { return g.rv(s).NEintr == 0 }, g, p,
		func(s *Symbol) []fval {
			return mkrow(fStr(s.Name()), fI32(s.NLoops), fI32(s.Cyclomatic),
				fI32(s.FanIn), fStr(g.at(s)))
		}, dI(3), dI(2))
}

func qQ104(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "depth", "io_in_loop", "cyclo", "fan_in", "at"}
	return apiQ(cols, []string{"send", "sendto", "sendmsg", "write", "writev",
		"pwritev"}, func(s *Symbol) bool { return g.rv(s).NRetNegCheck == 0 }, g, p,
		func(s *Symbol) []fval {
			return mkrow(fStr(s.Name()), fI32(s.MaxLoopDepth), fI32(s.IOInLoop),
				fI32(s.Cyclomatic), fI32(s.FanIn), fStr(g.at(s)))
		}, dI(4), dI(1))
}

func qQ105(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "returns_", "cyclo", "fan_in", "at"}
	return apiQ(cols, []string{"mmap"},
		func(s *Symbol) bool { return !hasAPIUse(g, s.ID, fnIs("munmap")) }, g, p,
		func(s *Symbol) []fval {
			return mkrow(fStr(s.Name()), fI32(s.NReturns), fI32(s.Cyclomatic),
				fI32(s.FanIn), fStr(g.at(s)))
		}, dI(3), dI(2))
}

func qQ106(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "cyclo", "returns_", "fan_in", "at"}
	return apiQ(cols, []string{"waitpid", "wait", "waitid", "wait3", "wait4"},
		func(s *Symbol) bool { return g.rv(s).NRetNegCheck == 0 }, g, p,
		func(s *Symbol) []fval {
			return mkrow(fStr(s.Name()), fI32(s.Cyclomatic), fI32(s.NReturns),
				fI32(s.FanIn), fStr(g.at(s)))
		}, cmpI(3), cmpI(1))
}

func qQ107(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "cyclo", "fan_in", "at"}
	return apiQ(cols, []string{"scanf", "fscanf", "sscanf", "vscanf", "vfscanf",
		"vsscanf"}, func(s *Symbol) bool { return g.rv(s).NRetNegCheck == 0 }, g, p,
		func(s *Symbol) []fval {
			return mkrow(fStr(s.Name()), fI32(s.Cyclomatic), fI32(s.FanIn),
				fStr(g.at(s)))
		}, dI(2), dI(1))
}

func qQ108(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "depth", "cyclo", "fan_in", "at"}
	return apiQ(cols, []string{"sqrt", "log", "log10", "log2", "log1p", "logb",
		"asin", "acos", "atanh", "acosh", "tgamma", "lgamma", "pow"},
		func(s *Symbol) bool { return g.rv(s).NDomainGuard == 0 }, g, p,
		func(s *Symbol) []fval {
			return mkrow(fStr(s.Name()), fI32(s.MaxLoopDepth),
				fI32(s.Cyclomatic), fI32(s.FanIn), fStr(g.at(s)))
		}, dI(3), dI(1))
}

func qQ109(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "cyclo", "conc_in_fn", "fan_in", "at"}
	return apiQ(cols, []string{"localtime", "gmtime", "ctime", "asctime"},
		func(s *Symbol) bool { return true }, g, p,
		func(s *Symbol) []fval {
			return mkrow(fStr(s.Name()), fI32(s.Cyclomatic), fI32(s.NConcurrency),
				fI32(s.FanIn), fStr(g.at(s)))
		}, cmpI(3), cmpI(1))
}

func qQ110(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "cyclo", "fan_in", "at"}
	return apiQ(cols, []string{"signal"}, func(s *Symbol) bool {
		return !hasAPIUse(g, s.ID, fnIs("sigaction"))
	}, g, p, func(s *Symbol) []fval {
		return mkrow(fStr(s.Name()), fI32(s.Cyclomatic), fI32(s.FanIn), fStr(g.at(s)))
	}, dI(2), dI(1))
}

func qQ111(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "cyclo", "fan_in", "at"}
	return apiQ(cols, []string{"mmap"},
		func(s *Symbol) bool { return g.rv(s).NMapFailed == 0 && s.NNullCheck == 0 },
		g, p, func(s *Symbol) []fval {
			return mkrow(fStr(s.Name()), fI32(s.Cyclomatic), fI32(s.FanIn),
				fStr(g.at(s)))
		}, dI(2), dI(1))
}

func qQ112(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "depth", "cyclo", "fan_in", "at"}
	return apiQ(cols, []string{"accept", "accept4"},
		func(s *Symbol) bool { return g.rv(s).NNonblockSet == 0 }, g, p,
		func(s *Symbol) []fval {
			return mkrow(fStr(s.Name()), fI32(s.MaxLoopDepth),
				fI32(s.Cyclomatic), fI32(s.FanIn), fStr(g.at(s)))
		}, cmpI(3), cmpI(1))
}

func qQ113(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "cyclo", "returns_", "fan_in", "at"}
	return apiQ(cols, []string{"va_start"},
		func(s *Symbol) bool { return g.rv(s).NVaEnd == 0 }, g, p,
		func(s *Symbol) []fval {
			return mkrow(fStr(s.Name()), fI32(s.Cyclomatic), fI32(s.NReturns),
				fI32(s.FanIn), fStr(g.at(s)))
		}, cmpI(3), cmpI(1))
}

func qQ114(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "sized_arrays", "cyclo", "fan_in", "at"}
	return apiQ(cols, []string{}, func(s *Symbol) bool { return g.rv(s).NStackszArray > 0 },
		g, p, func(s *Symbol) []fval {
			return mkrow(fStr(s.Name()), fI32(g.rv(s).NStackszArray),
				fI32(s.Cyclomatic), fI32(s.FanIn), fStr(g.at(s)))
		}, cmpI(1), cmpI(3))
}

func qQ115(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "broken_checks", "cyclo", "fan_in", "at"}
	return apiQ(cols, []string{"memcpy", "memmove", "memset", "malloc", "realloc",
		"strlen", "strcpy"}, func(s *Symbol) bool { return g.rv(s).NPtrOvfCheck > 0 },
		g, p, func(s *Symbol) []fval {
			return mkrow(fStr(s.Name()), fI32(g.rv(s).NPtrOvfCheck), fI32(s.Cyclomatic),
				fI32(s.FanIn), fStr(g.at(s)))
		}, cmpI(1), cmpI(3))
}

func qQ116(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "transposed", "cyclo", "fan_in", "at"}
	return apiQ(cols, []string{"calloc"},
		func(s *Symbol) bool { return g.rv(s).NCallocTransposed > 0 }, g, p,
		func(s *Symbol) []fval {
			return mkrow(fStr(s.Name()), fI32(g.rv(s).NCallocTransposed),
				fI32(s.Cyclomatic), fI32(s.FanIn), fStr(g.at(s)))
		}, cmpI(1), cmpI(3))
}

func qQ117(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "array_va_args", "cyclo", "fan_in", "at"}
	return apiQ(cols, []string{"va_arg", "va_start"},
		func(s *Symbol) bool { return g.rv(s).NVaArgArr > 0 }, g, p,
		func(s *Symbol) []fval {
			return mkrow(fStr(s.Name()), fI32(g.rv(s).NVaArgArr), fI32(s.Cyclomatic),
				fI32(s.FanIn), fStr(g.at(s)))
		}, cmpI(1), cmpI(3))
}

func qQ118(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "monotonic_uses", "cyclo", "depth", "fan_in", "at"}
	return apiQ(cols, []string{"gettimeofday", "time", "ftime", "clock"},
		func(s *Symbol) bool {
			return g.rv(s).NMonotonicClock == 0 &&
				!hasAPIUse(g, s.ID, fnIs("clock_gettime"))
		}, g, p, func(s *Symbol) []fval {
			return mkrow(fStr(s.Name()), fI32(g.rv(s).NMonotonicClock),
				fI32(s.Cyclomatic), fI32(s.MaxLoopDepth), fI32(s.FanIn),
				fStr(g.at(s)))
		}, dI(4), dI(2))
}

func qQ119(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "size_one_waits", "waits", "epoll_calls",
		"kqueue_calls", "cyclo", "fan_in", "at"}
	return simpleQ(cols, func(s *Symbol) bool { return g.rv(s).NEvBatchOne > 0 },
		g, p, func(s *Symbol) []fval {
			return mkrow(fStr(s.Name()), fI32(g.rv(s).NEvBatchOne), fI32(g.rv(s).NEventWait),
				fI32(g.rv(s).NEpoll), fI32(g.rv(s).NKqueue), fI32(s.Cyclomatic),
				fI32(s.FanIn), fStr(g.at(s)))
		}, cmpI(1), cmpI(6))
}

var _ = strconv.Itoa

type params struct {
	mod string
	lim int
}

func likeMatch(s, pat string) bool {
	if pat == "%" {
		return true
	}

	si, pi := 0, 0
	star, mark := -1, 0
	for si < len(s) {
		switch {
		case pi < len(pat) && (pat[pi] == '_' || pat[pi] == s[si]):
			si++
			pi++
		case pi < len(pat) && pat[pi] == '%':
			star = pi
			pi++
			mark = si
		case star >= 0:
			pi = star + 1
			mark++
			si = mark
		default:
			return false
		}
	}
	for pi < len(pat) && pat[pi] == '%' {
		pi++
	}
	return pi == len(pat)
}

func (g *Graph) modName(id int32) string {
	if id <= 0 || int(id) > len(g.Modules) {
		return ""
	}
	return g.Modules[id-1].Name()
}

func (g *Graph) fileOf(symID int32) *File {
	if symID < 1 || int(symID) > len(g.Symbols) {
		return nil
	}
	fid := g.Symbols[symID-1].FileID
	if fid < 1 || int(fid) > len(g.Files) {
		return nil
	}
	return &g.Files[fid-1]
}

// at is "path:line" for a symbol. Nearly every row of every query ends with
// one, so each is built once.
func (g *Graph) at(s *Symbol) string {
	if len(g.atCache) != len(g.Symbols)+1 {
		g.atCache = make([]string, len(g.Symbols)+1)
	}
	own := s.ID >= 1 && int(s.ID) <= len(g.Symbols) && g.Symbols[s.ID-1] == s
	if own && g.atCache[s.ID] != "" {
		return g.atCache[s.ID]
	}
	f := g.fileOf(s.ID)
	if f == nil {
		return ""
	}
	v := f.Path() + ":" + strconv.Itoa(int(s.LineStart))
	if own {
		g.atCache[s.ID] = v
	}
	return v
}

func pct(a, b int64) int32 {
	if b == 0 {
		return 0
	}
	return int32(float64(a) * 100.0 / float64(b))
}

func maxI(a, b int32) int32 {
	if a > b {
		return a
	}
	return b
}

// mkrow makes one result row. The values are copied into a block shared by
// many rows, so a query that returns twenty thousand rows makes a few dozen
// allocations for them, not twenty thousand. Queries run one at a time; the
// blocks are never reused, only dropped with the rows that point into them.
func mkrow(v ...fval) []fval {
	if cap(rowBlock)-len(rowBlock) < len(v) {
		rowBlock = make([]fval, 0, max(1024, len(v)))
	}
	n := len(rowBlock)
	rowBlock = append(rowBlock, v...)
	return rowBlock[n:len(rowBlock):len(rowBlock)]
}

var rowBlock []fval

func limit(rows [][]fval, p params) [][]fval {
	if p.lim >= 0 && len(rows) > p.lim {
		return rows[:p.lim]
	}
	return rows
}

type rowSorter struct {
	rows [][]fval
	less func(i, j int) bool
}

func (r rowSorter) Len() int      { return len(r.rows) }
func (r rowSorter) Swap(i, j int) { r.rows[i], r.rows[j] = r.rows[j], r.rows[i] }
func (r rowSorter) Less(i, j int) bool {
	return r.less(i, j)
}

// ordKey is one sort key: a comparison, and where the key is an integer the
// integer itself, so a sort can compare leading keys without a call.
type ordKey struct {
	cmp   func(a, b []fval) int
	lead  func(r []fval) int64
	loose bool     // equal leads do not mean equal keys (a string's prefix)
	parts []ordKey // the keys a then() was made from
}

// strLead is the first eight bytes of s as an integer that orders the way
// the bytes do, so most string comparisons never reach the string.
func strLead(s string) int64 {
	var v uint64
	for i := 0; i < 8; i++ {
		v <<= 8
		if i < len(s) {
			v |= uint64(s[i])
		}
	}
	return int64(v ^ 1<<63)
}

func cmpI(col int) ordKey {
	return ordKey{cmp: func(a, b []fval) int { return cmpI32(a[col].i, b[col].i) },
		lead: func(r []fval) int64 { return r[col].i }}
}

func cmpI32(a, b int64) int {
	switch {
	case a < b:
		return -1
	case a > b:
		return 1
	}
	return 0
}

func lineOf(at string) int64 {
	n, _ := strconv.Atoi(at[strings.LastIndexByte(at, ':')+1:])
	return int64(n)
}

func cmpLine(col int) ordKey {
	return ordKey{cmp: func(a, b []fval) int { return cmpI32(lineOf(a[col].s), lineOf(b[col].s)) },
		lead: func(r []fval) int64 { return lineOf(r[col].s) }}
}

func cmpS(col int) ordKey {
	return ordKey{cmp: func(a, b []fval) int { return strings.Compare(a[col].s, b[col].s) },
		lead: func(r []fval) int64 { return strLead(r[col].s) }, loose: true}
}

func then(cs ...ordKey) ordKey {
	if len(cs) == 1 {
		return cs[0]
	}
	k := ordKey{cmp: func(a, b []fval) int {
		for i := range cs {
			if v := cs[i].cmp(a, b); v != 0 {
				return v
			}
		}
		return 0
	}}
	if len(cs) > 0 {
		k.lead = cs[0].lead
	}
	for _, c := range cs {
		if c.parts != nil {
			k.parts = append(k.parts, c.parts...)
		} else {
			k.parts = append(k.parts, c)
		}
	}
	return k
}

func orderBy(rows [][]fval, keys ...ordKey) {
	if len(rows) < 2 {
		return
	}
	k := then(keys...)
	cmp := k.cmp
	if len(rows) < 64 {
		slices.SortStableFunc(rows, cmp)
		return
	}
	// Nearly every key is an integer column, so the rows are not compared at
	// all: the leading keys are read out once, packed into one or two words
	// per row, and the row numbers are radix-sorted by those words. Radix
	// sort is stable, which is the order SortStableFunc gave. Only rows whose
	// words tie are then compared, and only on the keys the words leave out
	// (a string's tail, or a key with no integer form). Comparing was nine of
	// the eleven milliseconds a 26,000-row result took.
	parts := k.parts
	if parts == nil {
		parts = []ordKey{k}
	}
	n := len(rows)
	w0 := make([]uint64, n) // the exact leading keys, packed high to low
	w1 := make([]uint64, n) // the key after them
	col := make([]int64, n)
	bits0, bits1 := 0, 0
	used := 0 // parts fully settled by the words
	for used < len(parts) && parts[used].lead != nil {
		pk := parts[used]
		lo, hi := int64(math.MaxInt64), int64(math.MinInt64)
		for i, r := range rows {
			v := pk.lead(r)
			col[i] = v
			lo, hi = min(lo, v), max(hi, v)
		}
		width := bits.Len64(uint64(hi) - uint64(lo))
		if !pk.loose && bits0+width <= 64 {
			for i := range col {
				w0[i] = w0[i]<<width | (uint64(col[i]) - uint64(lo))
			}
			bits0 += width
			used++
			continue
		}
		for i := range col {
			w1[i] = uint64(col[i]) - uint64(lo)
		}
		bits1 = width
		if !pk.loose {
			used++
		}
		break
	}
	rest := parts[used:]
	perm := make([]int32, n)
	for i := range perm {
		perm[i] = int32(i)
	}
	tmp := make([]int32, n)
	radix := func(key []uint64, width int) {
		for shift := 0; shift < width; shift += 8 {
			var pos [257]int32
			for _, i := range perm {
				pos[(key[i]>>shift)&255+1]++
			}
			for d := 1; d < 257; d++ {
				pos[d] += pos[d-1]
			}
			for _, i := range perm {
				d := (key[i] >> shift) & 255
				tmp[pos[d]] = i
				pos[d]++
			}
			perm, tmp = tmp, perm
		}
	}
	radix(w1, bits1)
	radix(w0, bits0)
	if len(rest) > 0 {
		tie := func(a, b int32) int {
			for i := range rest {
				if c := rest[i].cmp(rows[a], rows[b]); c != 0 {
					return c
				}
			}
			return int(a) - int(b)
		}
		for lo := 0; lo < n; {
			hi := lo + 1
			for hi < n && w0[perm[hi]] == w0[perm[lo]] && w1[perm[hi]] == w1[perm[lo]] {
				hi++
			}
			if hi-lo > 1 {
				slices.SortFunc(perm[lo:hi], tie)
			}
			lo = hi
		}
	}
	out := make([][]fval, n)
	for i, at := range perm {
		out[i] = rows[at]
	}
	copy(rows, out)
}

func groupConcat(vals []string) string { return strings.Join(vals, ",") }

func distinctStrings(in []string) []string {
	seen := map[string]bool{}
	out := make([]string, 0, len(in))
	for _, v := range in {
		if !seen[v] {
			seen[v] = true
			out = append(out, v)
		}
	}
	return out
}

func containsStr(list []string, s string) bool {
	return slices.Contains(list, s)
}

func (g *Graph) sym(id int32) *Symbol {
	if id < 1 || int(id) > len(g.Symbols) {
		return nil
	}
	return g.Symbols[id-1]
}

func dI(col int) ordKey {
	return ordKey{cmp: func(a, b []fval) int { return cmpI32(b[col].i, a[col].i) },
		lead: func(r []fval) int64 { return -r[col].i }}
}

func dSum(cols ...int) ordKey {
	sum := func(r []fval) int64 {
		t := int64(0)
		for _, c := range cols {
			t += r[c].i
		}
		return t
	}
	return ordKey{cmp: func(a, b []fval) int { return cmpI32(sum(b), sum(a)) },
		lead: func(r []fval) int64 { return -sum(r) }}
}

func dWeighted(w [][3]int32) ordKey {
	sum := func(r []fval) int64 {
		t := int64(0)
		for _, x := range w {
			t += int64(x[0]) * r[x[1]].i
		}
		return t
	}
	return ordKey{cmp: func(a, b []fval) int { return cmpI32(sum(b), sum(a)) },
		lead: func(r []fval) int64 { return -sum(r) }}
}

func dLenStr(col int) ordKey {
	return ordKey{cmp: func(a, b []fval) int { return cmpI32(int64(len(b[col].s)), int64(len(a[col].s))) },
		lead: func(r []fval) int64 { return -int64(len(r[col].s)) }}
}

// symsByName is every symbol in (name, id) order. Five queries and the build
// ask for it; the order depends only on the symbol table, so it is sorted
// once per table rather than once per caller.
func (g *Graph) symsByName() []*Symbol {
	if len(g.nameOrder) != len(g.Symbols) {
		g.nameOrder = g.scanByName()
	}
	return g.nameOrder
}

func (g *Graph) scanByName() []*Symbol {
	out := make([]*Symbol, 0, len(g.Symbols))
	for i := range g.Symbols {
		out = append(out, g.Symbols[i])
	}
	sort.SliceStable(out, func(a, b int) bool {
		if out[a].Name() != out[b].Name() {
			return out[a].Name() < out[b].Name()
		}
		return out[a].ID < out[b].ID
	})
	return out
}

func (g *Graph) symsByID() []*Symbol { return g.scanByID() }

func (g *Graph) scanByID() []*Symbol {
	out := make([]*Symbol, 0, len(g.Symbols))
	for i := range g.Symbols {
		out = append(out, g.Symbols[i])
	}
	return out
}

func (g *Graph) hazardsByCatN(cat string) []int32 {
	out := slices.Clone(g.hz().byCat[cat])
	slices.SortStableFunc(out, func(a, b int32) int {
		return cmp.Compare(g.Hazards[b].N, g.Hazards[a].N)
	})
	return out
}

func (g *Graph) hazardsByPattern(pats []string) []*Hazard {
	sorted := append([]string{}, pats...)
	sort.Strings(sorted)
	var out []*Hazard
	ix := g.hz()
	for _, pat := range sorted {
		for _, i := range ix.byPat[pat] {
			out = append(out, &g.Hazards[i])
		}
	}
	return out
}

var scanOrder = "file"

func (g *Graph) syms() []*Symbol {
	if g.symsCache != nil && g.symsCacheOrder == scanOrder {
		return g.symsCache
	}
	out := g.symsUncached()
	g.symsCache, g.symsCacheOrder = out, scanOrder
	return out
}

func (g *Graph) symsUncached() []*Symbol {
	if scanOrder == "name" {
		return g.symsByName()
	}
	switch scanOrder {
	case "name":
		return g.symsByName()
	case "file":
		out := make([]*Symbol, 0, len(g.Symbols))
		for i := range g.Symbols {
			out = append(out, g.Symbols[i])
		}
		sort.SliceStable(out, func(a, b int) bool {
			fa, fb := g.fileOf2(out[a].FileID), g.fileOf2(out[b].FileID)
			if fa == nil || fb == nil {
				return out[a].ID < out[b].ID
			}
			if fa.Path() != fb.Path() {
				return fa.Path() < fb.Path()
			}
			if out[a].LineStart != out[b].LineStart {
				return out[a].LineStart < out[b].LineStart
			}
			return out[a].ID < out[b].ID
		})
		return out
	}
	return g.symsByID()
}

func (g *Graph) symsName() []*Symbol { return g.symsByName() }

func mM1(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"module", "fns", "fnptr", "via_macro", "external",
		"unresolved", "pct_unseen"}
	type agg struct {
		fns, fp, mac, ext, unr, calls int32
	}
	m := make([]agg, len(g.Modules)+1)
	for _, s := range g.Symbols {
		if s.Kind() != "function" || s.ModuleID < 0 || int(s.ModuleID) >= len(m) {
			continue
		}
		a := &m[s.ModuleID]
		a.fns++
		a.fp += g.rv(s).NFnptrCalls
		a.mac += g.rv(s).NMacroCalls
		a.ext += g.rv(s).NExternalCalls
		a.unr += s.NUnresolvedCalls
		a.calls += s.NCalls
	}
	var rows [][]fval
	for i := range g.Modules {
		mod := &g.Modules[i]
		if !likeMatch(mod.Name(), p.mod) {
			continue
		}
		if mod.ID < 0 || int(mod.ID) >= len(m) || m[mod.ID].fns == 0 {
			continue
		}
		a := &m[mod.ID]
		var pv fval = fNull()
		if a.calls != 0 {
			pv = fI32(pct(int64(a.fp+a.unr), int64(a.calls)))
		}
		rows = append(rows, mkrow(fStr(mod.Name()), fI32(a.fns), fI32(a.fp),
			fI32(a.mac), fI32(a.ext), fI32(a.unr), pv))
	}
	orderBy(rows, dI(2), dI(5))
	return cols, limit(rows, p)
}

func mM2(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "fan_in", "sites", "cyclo", "sloc", "stat", "inl",
		"module", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.Kind() != "function" || !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.FanIn), fI32(s.NCallsites),
			fI32(s.Cyclomatic), fI32(s.Sloc), fI32(s.IsStatic), fI32(s.IsInline),
			fOptStr(g.modName(s.ModuleID), s.ModuleID != 0), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(3), cmpS(0))
	return cols, limit(rows, p)
}

func mM3(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "risk", "cyclo", "cog", "nest", "mem", "io", "int_",
		"rec", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.Kind() != "function" || !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.RiskScore),
			fI32(s.Cyclomatic), fI32(s.Cognitive), fI32(s.MaxNesting),
			fI32(s.NMemory), fI32(s.NIO), fI32(s.NInteger), fI32(s.IsRecursive),
			fStr(g.at(s))))
	}
	orderBy(rows, dI(1), cmpS(0))
	return cols, limit(rows, p)
}

func mM4(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "xalloc", "direct", "loops", "module", "at"}
	n1 := len(g.Symbols) + 1
	ok := func(id int32) bool { return id >= 0 && int(id) < n1 }
	// call sites per (caller, callee), as runs. A pair split across two runs
	// is harmless: everything below is a sum over them.
	type run struct{ c, d, n int32 }
	var mult []run
	for i := range g.Callsites {
		c := &g.Callsites[i]
		if k := len(mult) - 1; k >= 0 && mult[k].c == c.CallerID && mult[k].d == c.CalleeID {
			mult[k].n++
		} else {
			mult = append(mult, run{c.CallerID, c.CalleeID, 1})
		}
	}
	direct := make([]int32, n1)
	for _, i := range g.hazCats("alloc") {
		h := &g.Hazards[i]
		lp := strings.ToLower(h.Pattern())
		if h.Pattern() == "free" || strings.HasSuffix(lp, "free") || !ok(h.SymbolID) {
			continue
		}
		direct[h.SymbolID] += h.N
	}
	total := slices.Clone(direct)
	cur := direct
	for range 3 {
		next := make([]int32, n1)
		for _, k := range mult {
			if ok(k.c) && ok(k.d) && cur[k.d] != 0 {
				next[k.c] += k.n * cur[k.d]
			}
		}
		for i, v := range next {
			total[i] += v
		}
		cur = next
	}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.Kind() != "function" || !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		if total[s.ID] < 8 {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(total[s.ID]),
			fI32(direct[s.ID]), fI32(s.NLoops),
			fOptStr(g.modName(s.ModuleID), s.ModuleID != 0), fStr(g.at(s))))
	}
	orderBy(rows, dI(1))
	return cols, limit(rows, p)
}

func mM5(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"from_module", "to_module", "edges", "calls", "distinct_targets"}
	type ek struct{ a, b int32 }
	type agg struct{ edges, calls, tgt int32 }
	// Walk the edges callee by callee. A callee has few calling modules, so
	// they are gathered in a short list and the module-pair table is touched
	// once per (callee, calling module) instead of once per edge -- and
	// "distinct targets" is then simply how often that happens. `edges` is a
	// count of edge rows: the table holds one row per (caller, callee), which
	// --selfcheck verifies.
	m := map[ek]*agg{}
	in := g.edgeCSR(true)
	type part struct{ mod, edges, calls int32 }
	var parts []part
	for id := int32(1); int(id) <= len(g.Symbols); id++ {
		b := g.sym(id)
		parts = parts[:0]
	edges:
		for _, j := range in.of(id) {
			e := &g.Edges[j]
			a := g.sym(e.CallerID)
			if e.SameModule != 0 || a == nil {
				continue
			}
			for k := range parts {
				if parts[k].mod == a.ModuleID {
					parts[k].edges++
					parts[k].calls += e.NCalls
					continue edges
				}
			}
			parts = append(parts, part{a.ModuleID, 1, e.NCalls})
		}
		for _, pt := range parts {
			k := ek{pt.mod, b.ModuleID}
			v := m[k]
			if v == nil {
				v = &agg{}
				m[k] = v
			}
			v.edges += pt.edges
			v.calls += pt.calls
			v.tgt++
		}
	}
	keys := make([]ek, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	slices.SortFunc(keys, func(x, y ek) int {
		if x.a != y.a {
			return cmp.Compare(x.a, y.a)
		}
		return cmp.Compare(x.b, y.b)
	})
	var rows [][]fval
	for _, k := range keys {
		if !likeMatch(g.modName(k.a), p.mod) {
			continue
		}
		v := m[k]
		rows = append(rows, mkrow(fStr(g.modName(k.a)), fStr(g.modName(k.b)),
			fI32(v.edges), fI32(v.calls), fI32(v.tgt)))
	}
	orderBy(rows, dI(2))
	return cols, limit(rows, p)
}

func mM6(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"header", "rebuilt_files", "sloc", "syms", "min_depth"}
	// A breadth-first walk of the include graph from every file, four levels
	// deep. Each file's walk marks what it has reached with its own number in
	// one shared array, which replaces a map per file.
	nf := len(g.Files) + 1
	ok := func(id int32) bool { return id >= 1 && int(id) < nf }
	inc := make([][]int32, nf)
	for i := range g.Imports {
		im := &g.Imports[i]
		if im.HasTargetID && ok(im.FileID) && ok(im.TargetID) {
			inc[im.FileID] = append(inc[im.FileID], im.TargetID)
		}
	}
	count := make([]int32, nf)
	minD := make([]int32, nf)
	seen := make([]int32, nf)
	var cur, next []int32
	for src := int32(1); int(src) < nf; src++ {
		if len(inc[src]) == 0 {
			continue
		}
		reach := func(t, d int32) {
			seen[t] = src
			count[t]++
			if minD[t] == 0 || d < minD[t] {
				minD[t] = d
			}
		}
		cur = cur[:0]
		for _, t := range inc[src] {
			if seen[t] != src {
				reach(t, 1)
				cur = append(cur, t)
			}
		}
		for depth := int32(2); depth <= 4 && len(cur) > 0; depth++ {
			next = next[:0]
			for _, h := range cur {
				for _, t := range inc[h] {
					if t != src && seen[t] != src {
						reach(t, depth)
						next = append(next, t)
					}
				}
			}
			cur, next = next, cur
		}
	}
	var rows [][]fval
	for h := int32(1); int(h) < nf; h++ {
		if count[h] == 0 {
			continue
		}
		f := g.fileOf2(h)
		if f == nil || !likeMatch(g.modName(f.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(f.Path()), fI32(count[h]), fI32(f.Sloc),
			fI32(f.NSymbols), fI32(minD[h])))
	}
	orderBy(rows, dI(1), dI(2))
	return cols, limit(rows, p)
}

func mM7(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "depth", "loops", "calls", "divs", "libm", "brs",
		"sloc", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.MaxLoopDepth <= 1 || !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.MaxLoopDepth),
			fI32(s.NLoops), fI32(s.CallInLoop), fI32(g.rv(s).DivInLoop),
			fI32(g.rv(s).LibmInLoop), fI32(s.BranchInLoop), fI32(s.Sloc), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(3))
	return cols, limit(rows, p)
}

func mM8(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "libm", "libm_in_loop", "loops", "depth", "sloc",
		"fan_in", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.NLibm <= 0 || s.NLoops <= 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.NLibm),
			fI32(g.rv(s).LibmInLoop), fI32(s.NLoops), fI32(s.MaxLoopDepth),
			fI32(s.Sloc), fI32(s.FanIn), fStr(g.at(s))))
	}
	orderBy(rows, dI(2), dI(1))
	return cols, limit(rows, p)
}

func mM9(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "intrin", "hints", "restrict_", "builtins",
		"depth", "atomics", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.NIntrinsic <= 0 && s.NLikely <= 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.NIntrinsic),
			fI32(s.NLikely), fI32(s.NRestrict), fI32(s.NBuiltin),
			fI32(s.MaxLoopDepth), fI32(s.NAtomic), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(2))
	return cols, limit(rows, p)
}

func mM10(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"struct_", "kind", "sz", "pad", "tail", "packed_sz",
		"saves", "lines64", "lines64_packed", "algn", "at"}
	packed := g.shapeDense("packed_size")
	var rows [][]fval
	for _, ss := range g.SSize {
		if ss.Exact != 1 || ss.TotalPad <= 0 {
			continue
		}
		s := g.sym(ss.SymbolID)
		if s == nil || !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		best := ss.TotalSize
		if v := packed[ss.SymbolID]; v > 0 {
			best = v
		}
		rows = append(rows, mkrow(fStr(s.Name()), fStr(s.Kind()),
			fI32(ss.TotalSize), fI32(ss.TotalPad), fI32(ss.TailPad),
			fI32(best), fI32(ss.TotalSize-best), fI32(ss.NLines64),
			fI32((best+63)/64), fI32(ss.MaxAlign), fStr(g.at(s))))
	}
	orderBy(rows, dI(6), dI(3), cmpS(0))
	return cols, limit(rows, p)
}

func mM11(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"struct_", "sz", "pad", "lines64", "algn", "fields", "at"}
	nFields := g.perSym()
	for i := range g.Layout {
		nFields.add(g.Layout[i].SymbolID, 1)
	}
	var rows [][]fval
	for _, ss := range g.SSize {
		if ss.Exact != 1 || ss.TotalSize < 65 || ss.TotalSize > 128 {
			continue
		}
		s := g.sym(ss.SymbolID)
		if s == nil || !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(ss.TotalSize),
			fI32(ss.TotalPad), fI32(ss.NLines64), fI32(ss.MaxAlign),
			fI32(nFields.at(ss.SymbolID)), fStr(g.at(s))))
	}
	orderBy(rows, cmpI(1))
	return cols, limit(rows, p)
}

func mM12(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"struct_", "fields", "ptrs", "fnptr", "arrays", "pct_ptr", "at"}
	type sh struct{ n, ptr, fnp, arr int32 }
	m := make([]sh, len(g.Symbols)+1)
	for i := range g.Layout {
		l := &g.Layout[i]
		if l.InUnion != 0 || l.Depth != 0 {
			continue
		}
		s := g.sym(l.SymbolID)
		if s == nil || (s.Kind() != "struct" && s.Kind() != "union") {
			continue
		}
		e := &m[l.SymbolID]
		e.n++
		if l.PtrDepth > 0 {
			e.ptr++
		}
		if l.IsFnptr == 1 {
			e.fnp++
		}
		if l.ArrayLen > 0 {
			e.arr++
		}
	}
	var rows [][]fval
	for _, s := range g.syms() {
		e := &m[s.ID]
		if e.n == 0 {
			continue
		}
		if e.n < 6 || float64(e.ptr)*100/float64(e.n) < 50 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(e.n), fI32(e.ptr),
			fI32(e.fnp), fI32(e.arr),
			fI32(pct(int64(e.ptr), int64(e.n))), fStr(g.at(s))))
	}
	orderBy(rows, dI(2))
	return cols, limit(rows, p)
}

// chainBytes is, per function, the most automatic storage any acyclic call
// path starting there can have live at once: its own frame plus the deepest
// callee's. An edge back into a function already on the path is recursion and
// is not followed -- `cyc` reports that the figure is a floor for that row.
func (g *Graph) chainBytes(frame []int32) (total []int64, depth []int32, cyc []bool) {
	n := len(g.Symbols) + 1
	out := g.edgeCSR(false)
	total = make([]int64, n)
	depth = make([]int32, n)
	cyc = make([]bool, n)
	state := make([]uint8, n) // 1 on the path, 2 done
	type fr struct {
		id   int32
		next int
		best int64
		dep  int32
	}
	var stack []fr
	for _, s := range g.syms() {
		if s.Kind() != "function" || state[s.ID] == 2 {
			continue
		}
		stack = append(stack[:0], fr{id: s.ID})
		state[s.ID] = 1
		for len(stack) > 0 {
			f := &stack[len(stack)-1]
			kids := out.of(f.id)
			if f.next < len(kids) {
				ed := &g.Edges[kids[f.next]]
				f.next++
				c := ed.CalleeID
				if ed.IsSelf != 0 || c < 0 || int(c) >= n {
					continue
				}
				switch state[c] {
				case 0:
					state[c] = 1
					stack = append(stack, fr{id: c})
				case 1:
					cyc[f.id] = true
				case 2:
					if total[c] > f.best {
						f.best, f.dep = total[c], depth[c]
					}
					if cyc[c] {
						cyc[f.id] = true
					}
				}
				continue
			}
			total[f.id] = int64(frame[f.id]) + f.best
			depth[f.id] = f.dep + 1
			state[f.id] = 2
			done := *f
			stack = stack[:len(stack)-1]
			if len(stack) > 0 {
				par := &stack[len(stack)-1]
				if total[done.id] > par.best {
					par.best, par.dep = total[done.id], depth[done.id]
				}
				if cyc[done.id] {
					cyc[par.id] = true
				}
			}
		}
	}
	return
}

func mM13(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "frame_bytes", "biggest_local", "unsized", "locals",
		"chain_bytes", "chain_depth", "cyclic", "rec", "at"}
	frame := g.shapeDense("frame_bytes")
	big := g.shapeDense("frame_max_local")
	uns := g.shapeDense("frame_unsized")
	total, depth, cyc := g.chainBytes(frame)
	var rows [][]fval
	for _, s := range g.syms() {
		if s.Kind() != "function" || s.NLocals <= 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(frame[s.ID]),
			fI32(big[s.ID]), fI32(uns[s.ID]), fI32(s.NLocals),
			fval{kind: fkInt, i: total[s.ID]}, fI32(depth[s.ID]),
			fI32(b2i(cyc[s.ID])), fI32(s.IsRecursive), fStr(g.at(s))))
	}
	orderBy(rows, dI(5), dI(1), cmpS(0))
	return cols, limit(rows, p)
}

func mM14(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "casts", "derefs", "shifts", "sizeofs", "mem", "io", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.NCast <= 0 || !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.NCast), fI32(s.NDeref),
			fI32(s.NShift), fI32(s.NSizeof), fI32(s.NMemory), fI32(s.NIO),
			fStr(g.at(s))))
	}
	orderBy(rows, dI(1))
	return cols, limit(rows, p)
}

func macroQ(cols []string, pred func(m *Macro) bool, g *Graph, p params,
	fill func(s *Symbol, m *Macro) []fval,
	keys ...ordKey) ([]string, [][]fval) {
	bySym := g.six().macro
	var rows [][]fval
	for _, s := range g.syms() {
		m := bySym[s.ID]
		if m == nil || !pred(m) {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, fill(s, m))
	}
	orderBy(rows, then(keys...))
	return cols, limit(rows, p)
}

func mM15(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "uses", "params", "body_len", "multiline", "at"}
	return macroQ(cols, func(m *Macro) bool { return m.IsFunctionlike == 1 }, g, p,
		func(s *Symbol, m *Macro) []fval {
			return mkrow(fStr(s.Name()), fI32(m.NUses), fI32(m.NParams),
				fI32(m.BodyLen), fI32(m.IsMultiline), fStr(g.at(s)))
		}, dI(1), dI(3))
}

func mM16(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"expr", "sites", "files", "forms", "in_files"}
	type agg struct {
		sites  int32
		files  map[int32]bool
		forms  []string
		inFile []string
	}
	m := map[string]*agg{}
	var order []string
	for _, cb := range g.Cfgs {
		if cb.IsConfig != 1 {
			continue
		}
		a := m[cb.Expr()]
		if a == nil {
			a = &agg{files: map[int32]bool{}}
			m[cb.Expr()] = a
			order = append(order, cb.Expr())
		}
		a.sites++
		if !a.files[cb.FileID] {
			a.files[cb.FileID] = true
			if f := g.fileOf2(cb.FileID); f != nil && !containsStr(a.inFile, f.Basename()) {
				a.inFile = append(a.inFile, f.Basename())
			}
		}
		if !containsStr(a.forms, cb.Directive()) {
			a.forms = append(a.forms, cb.Directive())
		}
	}
	var rows [][]fval
	for _, e := range order {
		a := m[e]
		ok := false
		for fid := range a.files {
			if f := g.fileOf2(fid); f != nil && likeMatch(g.modName(f.ModuleID), p.mod) {
				ok = true
			}
		}
		if !ok {
			continue
		}
		rows = append(rows, mkrow(fStr(e), fI32(a.sites), fI32(int32(len(a.files))),
			fStr(groupConcat(a.forms)), fStr(groupConcat(a.inFile))))
	}
	orderBy(rows, dI(1))
	return cols, limit(rows, p)
}

func mM17(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "small", "large", "defs", "files"}
	type grp struct {
		sloc   []int32
		files  []string
		fileID map[int32]bool
		defs   int32
	}
	m := map[string]*grp{}
	var order []string
	nameN := g.six().nameN
	for _, s := range g.syms() {
		// a name only one symbol carries cannot have a second definition
		if s.Kind() != "function" || nameN[s.ID] < 2 {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		mod := g.modName(s.ModuleID)
		if !likeMatch(mod, p.mod) {
			continue
		}
		if s.ModuleID >= 1 && int(s.ModuleID) <= len(g.Modules) {
			if k := g.Modules[s.ModuleID-1]; k.Kind() == "test" || k.Kind() == "tool" {
				continue
			}
		}
		if s.Name() == "main" || s.Name() == "LLVMFuzzerTestOneInput" ||
			s.Name() == "usage" || s.Name() == "help" {
			continue
		}
		gp := m[s.Name()]
		if gp == nil {
			gp = &grp{fileID: map[int32]bool{}}
			m[s.Name()] = gp
			order = append(order, s.Name())
		}
		gp.sloc = append(gp.sloc, s.Sloc)
		gp.defs++
		gp.fileID[f.ID] = true
		if !containsStr(gp.files, f.Basename()) {
			gp.files = append(gp.files, f.Basename())
		}
	}
	var rows [][]fval
	for _, n := range order {
		gp := m[n]
		lo, hi := int32(1<<30), int32(0)
		for _, v := range gp.sloc {
			if v < lo {
				lo = v
			}
			if v > hi {
				hi = v
			}
		}
		if gp.defs <= 1 || hi < 3*lo+3 {
			continue
		}
		if len(gp.files) == 1 && gp.files[0] == gp.files[0] {

		}
		if len(gp.fileID) >= 2 && minOf(gp.files) == maxOf(gp.files) {
			continue
		}
		rows = append(rows, mkrow(fStr(n), fI32(lo), fI32(hi), fI32(gp.defs),
			fStr(groupConcat(distinctStrings(gp.files)))))
	}
	orderBy(rows, dWeighted([][3]int32{{1, 2}, {-1, 1}}))
	return cols, limit(rows, p)
}

func minOf(v []string) string {
	m := v[0]
	for _, x := range v {
		if x < m {
			m = x
		}
	}
	return m
}

func maxOf(v []string) string {
	m := v[0]
	for _, x := range v {
		if x > m {
			m = x
		}
	}
	return m
}

func mM18(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "fan_in", "sloc", "cyclo", "hidden", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.Kind() != "function" || s.IsInline != 1 || s.IsStatic != 1 ||
			s.FanIn < 3 || s.Sloc < 4 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.FanIn), fI32(s.Sloc),
			fI32(s.Cyclomatic), fI32(s.FanIn*s.Sloc), fStr(g.at(s))))
	}
	orderBy(rows, dI(4), cmpS(0))
	return cols, limit(rows, p)
}

func mM19(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "cyclo", "sloc", "cmts", "doc", "nest", "fan_in", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.Kind() != "function" || s.Cyclomatic < 20 ||
			s.NCommentLines*20 >= s.Sloc {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.Cyclomatic),
			fI32(s.Sloc), fI32(s.NCommentLines), fI32(s.HasDoc),
			fI32(s.MaxNesting), fI32(s.FanIn), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), cmpS(0))
	return cols, limit(rows, p)
}

func mM20(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"path", "rule", "objs", "srcs", "archives", "at_line"}
	var rows [][]fval
	for _, r := range g.MkRules {
		if r.NObjs+r.NSrcs < 2 {
			continue
		}
		rows = append(rows, mkrow(fStr(r.Path()), fStr(r.Rule()), fI32(r.NObjs),
			fI32(r.NSrcs), fI32(r.UsesAr), fI32(r.Line)))
	}
	orderBy(rows, dSum(2, 3))
	return cols, limit(rows, p)
}

func mM21(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"module", "files", "parsed", "errors", "unclosed", "sloc",
		"fns", "includes", "ms"}
	type agg struct {
		files, parsed, errs, unc, sloc, fns, inc, ms int32
	}
	m := map[int32]*agg{}
	for i := range g.Files {
		f := &g.Files[i]
		a := m[f.ModuleID]
		if a == nil {
			a = &agg{}
			m[f.ModuleID] = a
		}
		a.files++
		a.parsed += f.Parsed
		a.errs += f.NParseErrors
		a.unc += f.NMissingNodes
		a.sloc += f.Sloc
		a.fns += f.NFunctions
		a.inc += f.NImports
		a.ms += int32(f.ParseMs + 0.5)
	}
	var rows [][]fval
	for i := range g.Modules {
		mod := &g.Modules[i]
		a := m[mod.ID]
		if a == nil || !likeMatch(mod.Name(), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(mod.Name()), fI32(a.files), fI32(a.parsed),
			fI32(a.errs), fI32(a.unc), fI32(a.sloc), fI32(a.fns), fI32(a.inc),
			fI32(a.ms)))
	}
	orderBy(rows, dI(5))
	return cols, limit(rows, p)
}

func mM22(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "gotos", "labels", "cyclo", "sloc", "fan_in", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.NGotos <= 3 {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.NGotos), fI32(s.NLabels),
			fI32(s.Cyclomatic), fI32(s.Sloc), fI32(s.FanIn), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(3))
	return cols, limit(rows, p)
}

func mM23(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "nesting", "cyclo", "loops", "sloc", "fan_in", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.MaxNesting <= 5 || s.Kind() != "function" {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.MaxNesting),
			fI32(s.Cyclomatic), fI32(s.NLoops), fI32(s.Sloc), fI32(s.FanIn),
			fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(2), cmpS(0))
	return cols, limit(rows, p)
}

func mM24(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "n_params", "sloc", "cyclo", "fan_in", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.NParams <= 6 || s.Kind() != "function" {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.NParams), fI32(s.Sloc),
			fI32(s.Cyclomatic), fI32(s.FanIn), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(4), cmpS(0))
	return cols, limit(rows, p)
}

func mM25(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "n_caller_modules", "fan_in", "cyclo", "sloc",
		"modules", "at"}
	// the edges into each function, in edge order -- scanning the whole edge
	// table once per function took 1.9s on PostgreSQL
	in := g.edgeCSR(true)
	var mods []int32
	// seen[m] holds the id of the last function that counted module m, so a
	// function called from a thousand places does not search its own list a
	// thousand times
	seen := make([]int32, len(g.Modules)+1)
	var rows [][]fval
	for _, s := range g.syms() {
		if s.Kind() != "function" {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		mods = mods[:0]
		for _, j := range in.of(s.ID) {
			e := &g.Edges[j]
			if e.IsSelf != 0 {
				continue
			}
			cs := g.sym(e.CallerID)
			if cs == nil {
				continue
			}
			if m := cs.ModuleID; uint32(m) < uint32(len(seen)) {
				if seen[m] != s.ID {
					seen[m] = s.ID
					mods = append(mods, m)
				}
			} else if !slices.Contains(mods, m) {
				mods = append(mods, m)
			}
		}
		if len(mods) <= 5 {
			continue
		}
		names := []string{}
		for _, m := range mods {
			if n := g.modName(m); n != "" {
				names = append(names, n)
			}
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(int32(len(mods))),
			fI32(s.FanIn), fI32(s.Cyclomatic), fI32(s.Sloc),
			fStr(groupConcat(distinctStrings(names))), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(2), cmpS(0))
	return cols, limit(rows, p)
}

func mM26(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "magic_numbers", "float_literals", "cyclo", "sloc",
		"fan_in", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.NMagic <= 10 || s.Kind() != "function" {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.NMagic),
			fI32(s.NFloatLit), fI32(s.Cyclomatic), fI32(s.Sloc), fI32(s.FanIn),
			fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(5), cmpS(0))
	return cols, limit(rows, p)
}

func mM27(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "reachable_fns", "transitive_callers", "fan_out",
		"cyclo", "module", "at"}
	var rows [][]fval
	for _, r := range g.Reach {
		s := g.sym(r.SymbolID)
		if s == nil || s.Kind() != "function" {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(r.NTransitiveOut),
			fI32(r.NTransitive), fI32(s.FanOut), fI32(s.Cyclomatic),
			fOptStr(g.modName(s.ModuleID), s.ModuleID != 0), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), cmpS(0))
	return cols, limit(rows, p)
}

func mM28(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"module", "mutable_globals", "static_ones", "writer_fns",
		"write_sites"}
	gs := map[int32]int32{}
	ss := map[int32]int32{}
	for _, gl := range g.Globals {
		if gl.IsConst != 0 {
			continue
		}
		f := g.fileOf2(gl.FileID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		gs[f.ModuleID]++
		ss[f.ModuleID] += gl.IsStatic
	}
	wf := map[int32]int32{}
	ws := map[int32]int32{}
	for _, s := range g.syms() {
		if s.Kind() != "function" || g.rv(s).NGlobalWrite <= 0 {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		wf[f.ModuleID]++
		ws[f.ModuleID] += g.rv(s).NGlobalWrite
	}
	var rows [][]fval
	for i := range g.Modules {
		mod := &g.Modules[i]
		if !likeMatch(mod.Name(), p.mod) {
			continue
		}
		if gs[mod.ID]+wf[mod.ID] <= 0 {
			continue
		}
		rows = append(rows, mkrow(fStr(mod.Name()), fI32(gs[mod.ID]),
			fI32(ss[mod.ID]), fI32(wf[mod.ID]), fI32(ws[mod.ID])))
	}
	orderBy(rows, dI(1), dI(3))
	return cols, limit(rows, p)
}

func mM29(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "returns_", "fail_shapes", "null_checks", "gotos",
		"err_pct", "cyclo", "fan_in", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.Kind() != "function" || s.NReturns < 4 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		shapes := g.rv(s).RetNull + g.rv(s).RetNeg + g.rv(s).RetZero
		var pv fval = fNull()
		if den := s.NReturns + s.NBranches; den != 0 {
			pv = fI32(pct(int64(shapes+s.NNullCheck), int64(den)))
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.NReturns), fI32(shapes),
			fI32(s.NNullCheck), fI32(s.NGotos), pv, fI32(s.Cyclomatic),
			fI32(s.FanIn), fStr(g.at(s))))
	}
	orderBy(rows, dI(5), dI(7), cmpS(0))
	return cols, limit(rows, p)
}

func mM30(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"fn", "sites", "with_sizeof", "raw_bytes", "callers",
		"pct_sizeof"}
	type agg struct {
		sites, with, raw int32
		callers          map[string]bool
	}
	m := map[string]*agg{}
	var order []string
	for _, a := range g.Allocs {
		s := g.sym(a.SymbolID)
		if s == nil || !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		e := m[a.Fn()]
		if e == nil {
			e = &agg{callers: map[string]bool{}}
			m[a.Fn()] = e
			order = append(order, a.Fn())
		}
		e.sites++
		if strings.Contains(a.SizeExpr(), "sizeof") {
			e.with++
		} else {
			e.raw++
		}
		e.callers[s.Name()] = true
	}
	var rows [][]fval
	for _, fn := range order {
		e := m[fn]
		pv := 0.0
		if e.sites != 0 {
			pv = math.Round(float64(e.with)*100/float64(e.sites)*10) / 10
		}
		rows = append(rows, mkrow(fStr(fn), fI32(e.sites), fI32(e.with),
			fI32(e.raw), fI32(int32(len(e.callers))), fF(float64(pv))))
	}
	orderBy(rows, dI(1))
	return cols, limit(rows, p)
}

func mM31(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "copy_sites", "mem_ops_total", "loop_depth",
		"allocs_in_loop", "fan_in", "module", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.Kind() != "function" || g.rv(s).NMemcpy < 3 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(g.rv(s).NMemcpy),
			fI32(s.NMemory), fI32(s.MaxLoopDepth), fI32(s.AllocInLoop),
			fI32(s.FanIn), fOptStr(g.modName(s.ModuleID), s.ModuleID != 0),
			fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(5), cmpS(0))
	return cols, limit(rows, p)
}

func mM32(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "uses", "body_len", "expanded_bytes", "multiline",
		"params", "at"}
	return macroQ(cols, func(m *Macro) bool { return m.IsFunctionlike == 1 && m.NUses > 0 },
		g, p, func(s *Symbol, m *Macro) []fval {
			return mkrow(fStr(s.Name()), fI32(m.NUses), fI32(m.BodyLen),
				fI32(m.NUses*m.BodyLen), fI32(m.IsMultiline), fI32(m.NParams),
				fStr(g.at(s)))
		}, dI(3))
}

func mM33(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"struct_", "sz", "pad", "ptr_fields", "fnptr_fields",
		"includers", "lines64", "at"}
	ptr, fnp := g.perSym(), g.perSym()
	for i := range g.Layout {
		l := &g.Layout[i]
		if l.PtrDepth > 0 && l.InUnion == 0 && l.Depth == 0 {
			ptr.add(l.SymbolID, 1)
		}
		if l.IsFnptr == 1 {
			fnp.add(l.SymbolID, 1)
		}
	}
	// distinct files including each header
	pairs := make([]uint64, 0, len(g.Imports))
	for i := range g.Imports {
		if im := &g.Imports[i]; im.HasTargetID {
			pairs = append(pairs, uint64(uint32(im.TargetID))<<32|uint64(uint32(im.FileID)))
		}
	}
	slices.Sort(pairs)
	incl := g.perFile()
	for i, k := range pairs {
		if i == 0 || k != pairs[i-1] {
			incl.add(int32(k>>32), 1)
		}
	}
	var rows [][]fval
	for _, ss := range g.SSize {
		if ss.Exact != 1 {
			continue
		}
		s := g.sym(ss.SymbolID)
		if s == nil {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.Ext() != ".h" {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(ss.TotalSize),
			fI32(ss.TotalPad), fI32(ptr.at(ss.SymbolID)), fI32(fnp.at(ss.SymbolID)),
			fI32(incl.at(f.ID)), fI32(ss.NLines64), fStr(g.at(s))))
	}
	orderBy(rows, dI(5), dI(3), dI(1))
	return cols, limit(rows, p)
}

func mM34(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"path", "module", "n_markers", "todo", "fixme", "xxx",
		"hack", "bug", "unsafe", "deprecated", "sloc", "risk"}
	types := []string{"TODO", "FIXME", "XXX", "HACK", "BUG", "UNSAFE",
		"DEPRECATED"}
	type agg struct {
		n  int32
		by map[string]int32
	}
	m := map[int32]*agg{}
	for _, mk := range g.Markers {
		a := m[mk.FileID]
		if a == nil {
			a = &agg{by: map[string]int32{}}
			m[mk.FileID] = a
		}
		a.n++
		a.by[mk.Kind()]++
	}
	var rows [][]fval
	for i := range g.Files {
		f := &g.Files[i]
		a := m[f.ID]
		if a == nil {
			continue
		}
		if !likeMatch(g.modName(f.ModuleID), p.mod) {
			continue
		}
		mod := g.modName(f.ModuleID)
		if mod == "" {
			mod = "(none)"
		}
		row := mkrow(fStr(f.Path()), fStr(mod), fI32(a.n))
		for _, t := range types {
			row = append(row, fI32(a.by[t]))
		}
		row = append(row, fI32(f.Sloc), fI32(f.TotalRisk))
		rows = append(rows, row)
	}
	orderBy(rows, dI(2), dI(10))
	return cols, limit(rows, p)
}

func mM35(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"shape", "copies", "sloc", "cyclo", "params",
		"duplicated_sloc", "modules"}
	// the fingerprint is four numbers; it is only spelled out as text for the
	// shapes that turn out to have a second copy
	type key struct{ sloc, cyc, params, calls int32 }
	type grp struct {
		copies, dup int32
		mods        []int32
	}
	m := map[key]*grp{}
	for _, s := range g.syms() {
		if s.Kind() != "function" || s.Sloc < 10 || s.Cyclomatic < 3 {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		k := key{s.Sloc, s.Cyclomatic, s.NParams, s.NCalls}
		gp := m[k]
		if gp == nil {
			gp = &grp{}
			m[k] = gp
		}
		gp.copies++
		gp.dup += s.Sloc
		if !slices.Contains(gp.mods, s.ModuleID) {
			gp.mods = append(gp.mods, s.ModuleID)
		}
	}
	type shape struct {
		sig string
		k   key
	}
	var order []shape
	for k, gp := range m {
		if gp.copies > 1 {
			order = append(order, shape{strconv.Itoa(int(k.sloc)) + "/" +
				strconv.Itoa(int(k.cyc)) + "/" + strconv.Itoa(int(k.params)) + "/" +
				strconv.Itoa(int(k.calls)), k})
		}
	}
	slices.SortFunc(order, func(a, b shape) int { return strings.Compare(a.sig, b.sig) })
	var rows [][]fval
	for _, sh := range order {
		gp := m[sh.k]
		names := make([]string, 0, len(gp.mods))
		for _, mid := range gp.mods {
			names = append(names, g.modName(mid))
		}
		sort.Strings(names)
		rows = append(rows, mkrow(fStr(sh.sig), fI32(gp.copies), fI32(sh.k.sloc),
			fI32(sh.k.cyc), fI32(sh.k.params), fI32(gp.dup),
			fStr(groupConcat(distinctStrings(names)))))
	}
	orderBy(rows, dI(5), dI(1))
	return cols, limit(rows, p)
}

func mM36(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "aliasing_pairs", "restrict_hints", "params",
		"cyclo", "depth", "fan_in", "at"}
	start, idx := groupRows(len(g.Params), len(g.Symbols)+1, func(i int) int32 {
		if pr := &g.Params[i]; pr.IsRef == 1 && pr.Type() != "" {
			return pr.SymbolID
		}
		return -1
	})
	var rows [][]fval
	for _, s := range g.syms() {
		ps := idx[start[s.ID]:start[s.ID+1]]
		if len(ps) == 0 || s.Kind() != "function" || s.NRestrict != 0 {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		// pointer parameters that share a type with another one
		pairs := int32(0)
		for _, a := range ps {
			for _, b := range ps {
				if a != b && g.Params[a].Type() == g.Params[b].Type() {
					pairs++
					break
				}
			}
		}
		if pairs == 0 {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(pairs), fI32(s.NRestrict),
			fI32(s.NParams), fI32(s.Cyclomatic), fI32(s.MaxLoopDepth),
			fI32(s.FanIn), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(5), dI(6), cmpS(0))
	return cols, limit(rows, p)
}

func mM37(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"path", "module", "sloc", "fns", "types", "cyclo", "worst",
		"risk", "includes", "risk_per_100_sloc"}
	var rows [][]fval
	for i := range g.Files {
		f := &g.Files[i]
		if f.IsTest != 0 || f.Sloc <= 0 {
			continue
		}
		mod := g.modName(f.ModuleID)
		if !likeMatch(mod, p.mod) {
			continue
		}
		if mod == "" {
			mod = "(none)"
		}
		rows = append(rows, mkrow(fStr(f.Path()), fStr(mod), fI32(f.Sloc),
			fI32(f.NFunctions), fI32(f.NTypes), fI32(f.TotalCyclo),
			fI32(f.MaxCyclo), fI32(f.TotalRisk), fI32(f.NImports),
			fI32(pct(int64(f.TotalRisk), int64(f.Sloc)))))
	}
	orderBy(rows, dI(7), dI(2))
	return cols, limit(rows, p)
}

func mM38(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "fan_in", "fan_out", "cyclo", "sloc", "documented",
		"params", "declared_in", "at"}
	x := g.six()
	var rows [][]fval
	for _, s := range g.syms() {
		if s.Kind() != "function" || s.IsStatic != 0 || s.IsInline != 0 {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.Ext() != ".c" || x.declHdr[s.ID] == 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		nDecl := x.declN[s.ID]
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.FanIn), fI32(s.FanOut),
			fI32(s.Cyclomatic), fI32(s.Sloc), fI32(s.HasDoc), fI32(s.NParams),
			fI32(nDecl), fStr(g.at(s))))
	}
	orderBy(rows, dI(1), dI(4), cmpS(0))
	return cols, limit(rows, p)
}

func mM39(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "macro_params", "body_len", "uses", "multiline",
		"do_while", "at"}
	return macroQ(cols, func(m *Macro) bool { return m.IsFunctionlike == 1 }, g, p,
		func(s *Symbol, m *Macro) []fval {
			sp := strings.ReplaceAll(m.Body(), " ", "")
			dw := int32(0)
			if strings.Contains(sp, "do{") && strings.Contains(sp, "while(") {
				dw = 1
			}
			return mkrow(fStr(s.Name()), fI32(m.NParams), fI32(m.BodyLen),
				fI32(m.NUses), fI32(m.IsMultiline), fI32(dw), fStr(g.at(s)))
		}, cmpI(3), dI(2))
}

func mM40(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "locals", "uninitialised", "const_locals",
		"in_loop", "cyclo", "sloc", "at"}
	type agg struct {
		n, uninit, cst, loop int32
	}
	m := make([]agg, len(g.Symbols)+1)
	for i := range g.Locals {
		l := &g.Locals[i]
		if l.SymbolID < 0 || int(l.SymbolID) >= len(m) {
			continue
		}
		a := &m[l.SymbolID]
		a.n++
		if l.HasInit == 0 {
			a.uninit++
		}
		a.cst += l.IsConst
		a.loop += l.InLoop
	}
	var rows [][]fval
	for _, s := range g.syms() {
		a := &m[s.ID]
		if a.n == 0 || s.Kind() != "function" {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) || a.uninit < 3 {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(a.n), fI32(a.uninit),
			fI32(a.cst), fI32(a.loop), fI32(s.Cyclomatic), fI32(s.Sloc),
			fStr(g.at(s))))
	}
	orderBy(rows, dI(2), dI(5), cmpS(0))
	return cols, limit(rows, p)
}

func mM41(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"lock_name", "held_by_fns", "acquire_sites", "n_files",
		"cyclo_under_lock", "worst_cyclo", "io_under_lock", "allocs_under_lock",
		"modules"}
	type agg struct {
		fns                   map[int32]bool
		fileSet               map[int32]bool
		sites                 int32
		cyc, worst, io, alloc int32
		mods                  []string
	}
	m := map[string]*agg{}
	var order []string
	for i, l := range g.Locks {
		s := g.sym(l.SymbolID)
		if s == nil || !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		a := m[l.Name()]
		if a == nil {
			a = &agg{fns: map[int32]bool{}, fileSet: map[int32]bool{}}
			m[l.Name()] = a
			order = append(order, l.Name())
		}
		_ = i
		a.sites++
		a.fns[s.ID] = true
		a.cyc += s.Cyclomatic
		if s.Cyclomatic > a.worst {
			a.worst = s.Cyclomatic
		}
		a.io += s.NIO
		a.alloc += s.NAlloc
		a.mods = appendDistinct(a.mods, g.modName(s.ModuleID))
		if f := g.fileOf(s.ID); f != nil {
			a.fileSet[f.ID] = true
		}
	}
	var rows [][]fval
	for _, n := range order {
		a := m[n]
		rows = append(rows, mkrow(fStr(n), fI32(int32(len(a.fns))), fI32(a.sites),
			fI32(int32(len(a.fileSet))), fI32(a.cyc), fI32(a.worst), fI32(a.io), fI32(a.alloc),
			fStr(groupConcat(distinctStrings(a.mods)))))
	}
	orderBy(rows, dI(2), dI(4))
	return cols, limit(rows, p)
}

func mM42(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"typedef_name", "underlying", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if s.Kind() != "typedef" || !s.HasReturnType {
			continue
		}
		rt := s.ReturnType()
		if !strings.Contains(rt, "*") && !strings.Contains(rt, "]") {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fStr(rt), fStr(g.at(s))))
	}
	orderBy(rows, cmpS(0))
	return cols, limit(rows, p)
}

func mM43(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"path", "n_static_inline", "sloc", "total_fan_in",
		"worst_fan_in", "cyclo", "times_included"}
	incl := map[int32]int32{}
	for _, im := range g.Imports {
		if im.HasTargetID {
			incl[im.TargetID]++
		}
	}
	type agg struct {
		n, sloc, fan, worst, cyc int32
	}
	m := map[int32]*agg{}
	for _, s := range g.syms() {
		if s.Kind() != "function" || s.IsStatic != 1 || s.IsInline != 1 {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.Ext() != ".h" {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		a := m[f.ID]
		if a == nil {
			a = &agg{}
			m[f.ID] = a
		}
		a.n++
		a.sloc += s.Sloc
		a.fan += s.FanIn
		if s.FanIn > a.worst {
			a.worst = s.FanIn
		}
		a.cyc += s.Cyclomatic
	}
	var rows [][]fval
	for _, fid := range sortedIDs(m) {
		a := m[fid]
		f := g.fileOf2(fid)
		rows = append(rows, mkrow(fStr(f.Path()), fI32(a.n), fI32(a.sloc),
			fI32(a.fan), fI32(a.worst), fI32(a.cyc), fI32(incl[fid])))
	}
	orderBy(rows, dI(3), dI(1))
	return cols, limit(rows, p)
}

func sortedIDs[V any](m map[int32]V) []int32 {
	out := make([]int32, 0, len(m))
	for k := range m {
		out = append(out, k)
	}
	slices.Sort(out)
	return out
}

func mM44(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"module", "fns", "returns_null", "returns_neg",
		"returns_zero", "mixed", "pct_mixed"}
	type agg struct{ fns, nul, neg, zer, mixed int32 }
	m := map[int32]*agg{}
	for _, s := range g.syms() {
		if s.Kind() != "function" || s.NReturns == 0 {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil || f.IsTest != 0 {
			continue
		}
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		a := m[f.ModuleID]
		if a == nil {
			a = &agg{}
			m[f.ModuleID] = a
		}
		a.fns++
		a.nul += b2i(g.rv(s).RetNull > 0)
		a.neg += b2i(g.rv(s).RetNeg > 0)
		a.zer += b2i(g.rv(s).RetZero > 0)
		shapes := 0
		for _, v := range [][2]int32{{g.rv(s).RetNull, 0}, {g.rv(s).RetNeg, 0}, {g.rv(s).RetZero, 0}} {
			if v[0] > 0 {
				shapes++
			}
		}
		a.mixed += b2i(shapes >= 2)
	}
	var rows [][]fval
	for _, mid := range sortedIDs(m) {
		a := m[mid]
		if a.fns < 8 {
			continue
		}
		rows = append(rows, mkrow(fStr(g.modName(mid)), fI32(a.fns),
			fI32(a.nul), fI32(a.neg), fI32(a.zer), fI32(a.mixed),
			fI32(pct(int64(a.mixed), int64(a.fns)))))
	}
	orderBy(rows, dI(6), dI(1), cmpS(0))
	return cols, limit(rows, p)
}

func mM45(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"path", "module", "extern_decls", "also_in_a_header", "sloc"}
	hdrNames := map[string]bool{}
	for _, d := range g.Decls {
		if f := g.fileOf2(d.FileID); f != nil && f.Ext() == ".h" {
			hdrNames[d.Name()] = true
		}
	}
	srcN := map[int32]int32{}
	inhN := map[int32]int32{}
	seen := map[int32]map[string]bool{}
	for _, d := range g.Decls {
		f := g.fileOf2(d.FileID)
		if f == nil || f.Ext() != ".c" {
			continue
		}
		srcN[d.FileID]++
		if hdrNames[d.Name()] {
			if seen[d.FileID] == nil {
				seen[d.FileID] = map[string]bool{}
			}
			if !seen[d.FileID][d.Name()] {
				seen[d.FileID][d.Name()] = true
				inhN[d.FileID]++
			}
		}
	}
	var rows [][]fval
	for _, fid := range sortedIDs(srcN) {
		if srcN[fid] < 3 {
			continue
		}
		f := g.fileOf2(fid)
		mod := g.modName(f.ModuleID)
		if !likeMatch(mod, p.mod) {
			continue
		}
		if mod == "" {
			mod = "(none)"
		}
		rows = append(rows, mkrow(fStr(f.Path()), fStr(mod), fI32(srcN[fid]),
			fI32(inhN[fid]), fI32(f.Sloc)))
	}
	orderBy(rows, dI(2), dI(4))
	return cols, limit(rows, p)
}

func mM46(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "module", "epoll", "uring", "kqueue", "waits", "et",
		"oneshot", "write_ready", "err_flags", "rearms", "deregs", "eagain",
		"eintr", "calls", "at"}
	bySym := map[int32][]string{}
	for i := range g.EvOps {
		e := &g.EvOps[i]
		if !containsStr(bySym[e.SymbolID], e.Fn()) {
			bySym[e.SymbolID] = append(bySym[e.SymbolID], e.Fn())
		}
	}
	var rows [][]fval
	for _, s := range g.syms() {
		if g.rv(s).NEpoll+g.rv(s).NUring+g.rv(s).NKqueue <= 0 {
			continue
		}
		mod := g.modName(s.ModuleID)
		if !likeMatch(mod, p.mod) {
			continue
		}
		if mod == "" {
			mod = "(none)"
		}
		var calls fval = fNull()
		if l := bySym[s.ID]; len(l) > 0 {
			calls = fStr(groupConcat(l))
		}
		rows = append(rows, mkrow(fStr(s.Name()), fStr(mod), fI32(g.rv(s).NEpoll),
			fI32(g.rv(s).NUring), fI32(g.rv(s).NKqueue), fI32(g.rv(s).NEventWait), fI32(g.rv(s).NEtReg),
			fI32(g.rv(s).NOneshotReg), fI32(g.rv(s).NWriteReady), fI32(g.rv(s).NErrFlag),
			fI32(g.rv(s).NRearm), fI32(g.rv(s).NDereg), fI32(g.rv(s).NEagain), fI32(g.rv(s).NEintr),
			calls, fStr(g.at(s)), fI32(s.Cyclomatic)))
	}
	orderBy(rows, dSum(2, 3, 4), dI(len(cols)))
	for i, r := range rows {
		rows[i] = r[:len(cols)]
	}
	return cols, limit(rows, p)
}

func mM47(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "waits", "cyclo", "cog", "nest", "loop_depth",
		"calls_in_loop", "io_in_loop", "branches_in_loop", "sloc", "fan_in", "at"}
	var rows [][]fval
	for _, s := range g.syms() {
		if g.rv(s).NEventWait <= 0 || !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(g.rv(s).NEventWait),
			fI32(s.Cyclomatic), fI32(s.Cognitive), fI32(s.MaxNesting),
			fI32(s.MaxLoopDepth), fI32(s.CallInLoop), fI32(s.IOInLoop),
			fI32(s.BranchInLoop), fI32(s.Sloc), fI32(s.FanIn), fStr(g.at(s))))
	}
	orderBy(rows, dI(2), dI(9))
	return cols, limit(rows, p)
}

func mM48(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"module", "fns", "epoll_fns", "uring_fns", "kqueue_fns",
		"wait_fns", "edge_triggered", "watches_errors", "handles_eintr",
		"sets_nonblocking", "eintr_blind", "et_unprepared", "leaks_instance",
		"api_calls"}
	type agg struct{ c [13]int32 }
	m := map[int32]*agg{}
	for _, s := range g.syms() {
		if g.rv(s).NEpoll+g.rv(s).NUring+g.rv(s).NKqueue <= 0 {
			continue
		}
		f := g.fileOf(s.ID)
		if f == nil {
			continue
		}
		a := m[f.ModuleID]
		if a == nil {
			a = &agg{}
			m[f.ModuleID] = a
		}
		a.c[0]++
		a.c[1] += b2i(g.rv(s).NEpoll > 0)
		a.c[2] += b2i(g.rv(s).NUring > 0)
		a.c[3] += b2i(g.rv(s).NKqueue > 0)
		a.c[4] += b2i(g.rv(s).NEventWait > 0)
		a.c[5] += b2i(g.rv(s).NEtReg > 0)
		a.c[6] += b2i(g.rv(s).NErrFlag > 0)
		a.c[7] += b2i(g.rv(s).NEintr > 0)
		a.c[8] += b2i(g.rv(s).NNonblockSet > 0)
		a.c[9] += b2i(g.rv(s).NEventWait > 0 && g.rv(s).NEintr == 0)
		a.c[10] += b2i(g.rv(s).NEtReg > 0 && g.rv(s).NNonblockSet == 0)
		a.c[11] += b2i(g.rv(s).NEventCreate > 0 && g.rv(s).NEventDestroy == 0)
		a.c[12] += g.rv(s).NEpoll + g.rv(s).NUring + g.rv(s).NKqueue
	}
	var rows [][]fval
	for _, mid := range sortedIDs(m) {
		a := m[mid]
		mod := g.modName(mid)
		if !likeMatch(mod, p.mod) {
			continue
		}
		if mod == "" {
			mod = "(none)"
		}
		row := mkrow(fStr(mod))
		for _, v := range a.c {
			row = append(row, fI32(v))
		}
		rows = append(rows, row)
	}
	orderBy(rows, dI(13), dI(1))
	return cols, limit(rows, p)
}

var retNegNS = []string{"stdio.h", "sys/socket.h", "sys/mman.h", "sys/wait.h",
	"sys/uio.h", "unistd.h", "fcntl.h", "dirent.h", "stdio.h"}

func mM49(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"file", "ns", "call_sites", "fns", "distinct_calls"}
	// One row per (file, namespace). The first column used to be a module
	// name looked up with the FILE's id: "(none)" on nearly every row, and
	// some unrelated module's name on the rest.
	type agg struct {
		ns    string
		sites int32
		fns   []int32
		calls []string
	}
	nf := len(g.Files) + 1
	per := make([][]agg, nf)
uses:
	for i := range g.APIUses {
		a := &g.APIUses[i]
		f := g.fileOf2(a.FileID)
		if f == nil || !likeMatch(g.modName(f.ModuleID), p.mod) {
			continue
		}
		l := per[a.FileID]
		ns, fn := a.NS(), a.Fn()
		for k := range l {
			if e := &l[k]; e.ns == ns {
				e.sites++
				e.fns = append(e.fns, a.SymbolID)
				if !slices.Contains(e.calls, fn) {
					e.calls = append(e.calls, fn)
				}
				continue uses
			}
		}
		per[a.FileID] = append(l, agg{ns, 1, []int32{a.SymbolID}, []string{fn}})
	}
	var rows [][]fval
	for fid, l := range per {
		if len(l) == 0 {
			continue
		}
		slices.SortFunc(l, func(a, b agg) int { return strings.Compare(a.ns, b.ns) })
		for k := range l {
			e := &l[k]
			slices.Sort(e.fns)
			rows = append(rows, mkrow(fStr(g.Files[fid-1].Path()), fStr(e.ns),
				fI32(e.sites), fInt(len(slices.Compact(e.fns))), fInt(len(e.calls))))
		}
	}
	orderBy(rows, dI(2))
	return cols, limit(rows, p)
}

func mM50(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"file", "fns", "unchecked_returns", "unchecked_conversions",
		"ctype_ub", "varargs_unclosed", "mmap_unchecked", "api_call_sites"}
	// Per file, as the title says. The first column used to be a module name
	// looked up with the FILE's id -- "(none)" on nearly every row -- and
	// --module was matched against that same wrong name.
	type agg struct {
		hit                       bool
		ur, uc, cu, vu, mu, sites int32
		fns                       []int32
	}
	m := make([]agg, len(g.Files)+1)
	for i := range g.APIUses {
		a := &g.APIUses[i]
		f := g.fileOf2(a.FileID)
		if f == nil || !likeMatch(g.modName(f.ModuleID), p.mod) {
			continue
		}
		e := &m[a.FileID]
		e.fns = append(e.fns, a.SymbolID)
		s := g.sym(a.SymbolID)
		if s == nil {
			continue
		}
		e.hit = true
		e.sites++
		r := g.rv(s)
		ns := a.NS()
		if containsStr(retNegNS, ns) && r.NRetNegCheck == 0 {
			e.ur++
		}
		switch a.Fn() {
		case "strtol", "strtoll", "strtoul", "strtoull", "strtod":
			if !(r.NErrnoZero > 0 && r.NEndptr > 0) {
				e.uc++
			}
		}
		switch ns {
		case "ctype.h":
			if r.NUcharCast == 0 {
				e.cu++
			}
		case "stdarg.h":
			if r.NVaEnd == 0 {
				e.vu++
			}
		case "sys/mman.h":
			if r.NMapFailed == 0 {
				e.mu++
			}
		}
	}
	var rows [][]fval
	for fid := range m {
		e := &m[fid]
		if !e.hit {
			continue
		}
		slices.Sort(e.fns)
		rows = append(rows, mkrow(fStr(g.Files[fid-1].Path()),
			fInt(len(slices.Compact(e.fns))), fI32(e.ur), fI32(e.uc), fI32(e.cu),
			fI32(e.vu), fI32(e.mu), fI32(e.sites)))
	}
	orderBy(rows, dI(7))
	return cols, limit(rows, p)
}

var queries = []qentry{
	{"untrusted-frontier", "Parses attacker bytes AND does pointer/size arithmetic", "ANSWERS the functions where a memory-safety bug is actually reachable.\nACT this is the CWE-190 shape: a length from the wire, a shift or a cast,\n     then a copy. Review these before anything else on the risk list.\nMISLEADS io counts RAW descriptor calls only; a function fed by a caller\n     that did the read is just as exposed and is invisible here.", qQ1},
	{"stack-exhaustion", "Self-recursive functions, by how much stack each level costs", "ANSWERS where a deeply nested input can exhaust the C stack, and how deep\n     it has to be: `frame_bytes` is the automatic storage one call declares,\n     `levels_per_mb` how many nested calls fit in a megabyte of stack.\nACT every one of these needs a depth cap that is TESTED at the cap. A row\n     with levels_per_mb in the hundreds overflows a 512 KB thread stack on\n     input a fuzzer finds in seconds.\nMISLEADS DIRECT self-recursion only: mutual recursion through two functions\n     or a function pointer does not appear. frame_bytes sums every local\n     the body declares -- sibling scopes share slots, so it is an upper\n     bound -- and leaves out the `unsized` ones (types from headers this\n     file does not define, and VLAs), so it is also a floor. Read both.", qQ2},
	{"ownership-review", "Allocates but never frees in the same function", "ANSWERS where allocation ownership crosses a function boundary.\nACT each row must have a NAMED owner that frees it on EVERY path --\n     including the early returns and the goto-out ladder.\nMISLEADS this is a REVIEW list, not a leak list. Transferring ownership\n     to the caller is the normal C idiom and looks identical here.", qQ3},
	{"allocator-mixing", "Files that use libc malloc AND a project allocator", "ANSWERS which files split their memory across two accounting systems.\nACT pick ONE per file. A project allocator usually exists to track or cap\n     usage, and a libc allocation in the same file is invisible to it.\nMISLEADS a project allocator is recognised by NAME SHAPE (anything ending\n     in alloc/free/strdup), so an unconventionally named one is missed and\n     an unrelated `list_free` is counted. Also, mixing is sometimes right:\n     tracked buffers through the wrapper, private scratch through libc.", qQ4},
	{"alloc-per-iteration", "An allocation inside a loop body", "ANSWERS which loops pay the allocator once per item.\nACT hoist the allocation, or reserve capacity before the loop.\nMISLEADS a realloc-grow loop is amortised O(1) and belongs here anyway;\n     the count is of SITES, and one site in a hot loop beats ten cold ones.\n     An allocator is recognised by NAME -- the libc ones and anything ending\n     in alloc, strdup or memdup (zmalloc, xcalloc) -- so a wrapper with\n     another name is missed and a non-allocating `*alloc` accessor is\n     counted.", qQ5},
	{"bypass-tax", "Allocates BEFORE it knows the fast path applies", "ANSWERS which functions pay setup cost on inputs they then refuse.\nACT probe FIRST, allocate second. This is the classic bypass candidate.\nMISLEADS an allocation before a loop is usually just the output buffer,\n     which has to be allocated up front and is not a tax at all.", qQ6},
	{"race-surface", "Mutable, non-atomic, non-const file-scope state", "ANSWERS what two threads could be writing at the same time.\nACT every row needs a lock, an atomic, a thread-local, or a written proof\n     that only one thread ever touches it.\nMISLEADS it does not know which modules are threaded. The last column is\n     the only evidence offered: how many functions in the same module use\n     a concurrency primitive at all.", qQ7},
	{"per-element-dispatch", "A switch INSIDE a loop: type dispatch paid once per element", "ANSWERS where a loop re-decides the same thing on every iteration.\nACT hoist the switch out, or specialise the loop per case.\nMISLEADS a bytecode interpreter's dispatch loop is exactly this shape and\n     is correct by design. So is a state machine.", qQ8},
	{"loop-invariant-strlen", "strlen() inside a loop: accidental O(n^2)", "ANSWERS where a length is recomputed that cannot have changed.\nACT hoist it, or carry the length beside the string.\nMISLEADS a strlen over a SHORT string is a few cycles, and a loop whose\n     body mutates the string has to recompute it.", qQ9},
	{"error-shape-mix", "Functions that report failure in more than one shape", "ANSWERS where a caller can plausibly check the wrong thing.\nACT a function returning both NULL and -1 is one that callers get wrong\n     roughly half the time. Pick one convention per function and document\n     it in the header, where the caller is actually looking.\nMISLEADS returning 0 for success AND 0 as a legitimate value is the worst\n     case of all and is completely invisible here.", qQ10},
	{"dead-code", "Nothing in this tree calls these or names them", "ANSWERS what might be deletable: private functions with no caller and whose\n     name is never used as a value -- not passed as a callback, not stored\n     in a field, not listed in a file-scope table.\nACT grep the name as a STRING before deleting anything: a name assembled by\n     token pasting or looked up at run time keeps a symbol alive with no\n     reference to show for it.\nMISLEADS `graph-blindspots` measures how much of the call graph is unseen.\n     Public symbols are excluded because a caller outside this tree cannot\n     be seen at all. A function whose only caller is itself dead is NOT\n     listed -- this is one layer, not a transitive sweep. The functions kept\n     alive only by a table or a callback are in dispatch-table-orphan and\n     fnptr-blindspot-callers.", qQ11},
	{"nonreentrant-under-threads", "a libc call with a shared static buffer, in a function that also touches threads", "ANSWERS CERT CON33-C and MSC24-C, which clang-tidy states as a per-call\n     rule and cppcheck barely states at all: `localtime`, `strerror`,\n     `getenv`, `basename` and friends return a pointer into one static\n     buffer. Single-threaded that is correct and cheap. In a function that\n     also creates threads or takes locks it is a data race that corrupts\n     the OTHER thread's result, silently, under load.\nACT use the _r form -- localtime_r, strerror_r, getpwnam_r -- or copy the\n     result before releasing the lock. `patterns` names exactly which\n     calls, so the fix is mechanical.\nMISLEADS a function that touches threads is not necessarily called from\n     more than one, and pthread_create in main() next to a getenv() at\n     startup is fine. This sees categories in the same body, not the\n     happens-before between them.", qQ12},
	{"unchecked-conversion-on-an-io-path", "atoi/strtol in a function that also reads input", "ANSWERS CERT ERR34-C, which flawfinder and clang-tidy report on every\n     `atoi` in the tree. `atoi(\"42\")` on a literal is fine. `atoi` on\n     bytes that just came off a socket or a file returns 0 for the string\n     \"0\" and for garbage alike, and sets no errno to distinguish them.\n     The graph supplies the missing half: whether the same function also\n     performs I/O.\nACT use strtol with an end pointer and check both `end` and `errno`, or\n     reject the input outright. `conversions` names the exact calls;\n     `returns_value` says whether the function can even report a failure\n     to its caller, and a 0 there means the error has nowhere to go.\nMISLEADS co-occurrence in one body is not dataflow -- a function may read\n     a file and separately parse a constant. A wrapper that validates\n     before calling this one makes the row correct and harmless, and that\n     wrapper is not visible in the row.", qQ13},
	{"buffer-overflow-surface", "sprintf/strcpy/strcat/gets without bounds (CERT STR31-C)", "ANSWERS where unbounded buffer operations are used: sprintf, strcpy,\n     strcat, gets. Each can write past the end of a buffer if the input\n     is larger than expected.\nACT use snprintf, strncpy (or strlcpy), strncat, fgets. Check return values.\nMISLEADS a sprintf on a buffer known to be large enough is safe, but the\n     graph cannot prove the buffer size.", qQ14},
	{"format-string-injection", "printf with non-literal format string (CERT STR30-C)", "ANSWERS where printf/fprintf/sprintf is called with a format string that is\n     not a string literal, enabling format string attacks if the string\n     contains user-controlled %s or %n.\nACT use printf('%s', user_string) not printf(user_string).\nMISLEADS a format string from a trusted constant array is safe. The graph\n     sees the call but not the argument source.", qQ15},
	{"memory-leak-surface", "malloc/calloc/realloc without matching free (CERT MEM31-C)", "ANSWERS where a function allocates memory but has no matching free, which\n     is a leak if the allocation outlives the function and is not stored.\nACT free the allocation before returning, or document ownership transfer.\nMISLEADS a function that allocates and returns the pointer to the caller is\n     not leaking; the caller is responsible. The graph sees alloc/free in\n     the same function, not across functions.", qQ16},
	{"double-free-surface", "free called more than once on same pointer (CERT MEM40-C)", "ANSWERS where a function has more free calls than alloc calls, which may\n     indicate a double-free if the same pointer is freed on multiple paths.\nACT track the pointer state; set to NULL after free and check before.\nMISLEADS a function that frees pointers allocated in other functions has\n     more frees than allocs by design. The imbalance is a signal, not proof.", qQ17},
	{"null-deref-surface", "Pointer dereference without null check (CERT ERR30-C)", "ANSWERS where a function dereferences pointers but has no null checks,\n     which means a NULL pointer will crash the program.\nACT check for NULL before dereferencing, especially after malloc/calloc.\nMISLEADS a pointer known to be non-null (from a trusted source) does not\n     need a check. The graph sees n_null_check but not which pointers.", qQ18},
	{"integer-overflow-surface", "Arithmetic without bounds checking (CERT INT32-C)", "ANSWERS where integer arithmetic is performed without overflow checking,\n     which is UB for signed integers and wraps for unsigned.\nACT use checked arithmetic (__builtin_add_overflow) or validate inputs.\nMISLEADS arithmetic on known-small constants is safe. The graph counts\n     arithmetic sites but not the operand ranges.", qQ19},
	{"division-by-zero-surface", "Division by a variable the function never tests (CERT INT33-C)", "ANSWERS where `/` or `%` has a plain variable as its divisor and that name\n     is never compared with anything or used as a condition anywhere in the\n     function. `var_divs` is every non-literal divisor, for scale.\nACT follow the divisor back to its source. A parameter needs a guard here or\n     a documented precondition; a length or count needs its empty case.\nMISLEADS the test is function-wide and by NAME: a guard in the caller, or on\n     an alias of the same value, is invisible and the row is a false alarm.\n     A divisor that is an expression (`a->n`, `f(x)`, `n + 1`) is counted in\n     var_divs but never judged, so its absence here proves nothing.", qQ20},
	{"switch-no-default", "Switch without a default case (MISRA-C 16.4)", "ANSWERS where a switch statement has no default case, so unhandled values\n     fall through silently. MISRA-C requires a default in every switch.\nACT add a default case, even if it only asserts or does nothing.\nMISLEADS a switch over an exhaustive enum may not need a default, but a\n     future value will be silently ignored.", qQ21},
	{"race-condition-surface", "Shared static variable accessed from concurrent context (CERT CON33-C)", "ANSWERS where a function reads/writes a static variable and also touches\n     threads, locks, or atomics, indicating potential data races.\nACT protect the shared variable with a mutex, or use thread-local storage.\nMISLEADS a static variable set once at startup and only read afterward is\n     safe. The graph sees co-occurrence, not the happens-before relation.", qQ22},
	{"signal-handler-unsafe", "Unsafe function called inside a signal handler (CERT SIG30-C)", "ANSWERS where a function that may be called as a signal handler calls\n     non-async-signal-safe functions (malloc, printf, syslog).\nACT only call async-signal-safe functions inside signal handlers.\nMISLEADS relies on function name matching (contains 'sig' or 'handler'),\n     which is crude; a function registered via signal(2) at runtime with a\n     non-obvious name is not detected.", qQ23},
	{"infinite-loop", "A constant-true loop with no way out (CERT MSC53-C)", "ANSWERS which `while (1)` / `for (;;)` loops contain no break, return, goto\n     or call to exit/abort/longjmp anywhere in their body.\nACT confirm each one is a thread main or an event loop that is meant to run\n     until the process dies -- and that something outside can stop it.\nMISLEADS a worker thread and a scheduler are intentionally endless and are\n     most of this list in a healthy tree. A loop left only through a called\n     function that never returns (a project `fatal()`, pthread_cancel) is\n     reported; a loop whose only break belongs to a nested switch is not.", qQ24},
	{"unused-return-value", "Calls that drop a non-void result on the floor (CERT EXP12-C)", "ANSWERS which functions return a value that callers throw away, counted per\n     call SITE: `f(x);` as a whole statement, with f returning non-void.\nACT a status code that most callers ignore is an error channel nobody reads:\n     check it, or change the function to void and say so. `(void)f(x);` is\n     the way to mark a deliberate discard and is not counted.\nMISLEADS chaining APIs return their argument for convenience (a builder, an\n     sds-style append used for its side effect) and are correct to ignore.\n     Sites are matched by NAME, so a function defined more than once is left\n     out entirely, and a call through a pointer is never seen.", qQ25},
	{"macro-side-effect", "Heavily-used function-like macro that may evaluate arguments twice (CERT PRE31-C)", "ANSWERS the function-like macros with the most use sites -- each use\n     passes its arguments through the macro body once PER REFERENCE in\n     that body, so an argument with side effects (i++, f()) is\n     evaluated however many times the body names it. `uses` is the\n     call-shaped reference count from call resolution.\nACT convert the top rows to static inline functions: identical\n     runtime cost, but types checked, breakpoints possible, and the\n     double-evaluation hazard gone.\nMISLEADS a macro that references each argument exactly once is safe\n     and lands here anyway (the body text is captured in macros.body\n     -- read it before acting); a macro used only as a non-call token\n     shows zero uses.", qQ26},
	{"vtable-risk", "Functions reached through function pointers or dynamic member calls", "ANSWERS where runtime target ambiguity is densest: the reads through\n     `ops->`-style members and `(*fp)()` calls that a static call graph\n     cannot resolve. Every one is a site whose callee is decided at runtime.\nACT audit the dispatch site: is the function-pointer slot ever written with\n     something other than the one obvious initializer? If so, the edge here\n     is a security boundary, not an abstraction.\nMISLEADS counts CALL SITES, not distinct targets, and the brace scanner\n     sees the `->`-shaped member call syntax only; a struct passed around\n     and invoked through a local alias (`ops->read` copied into a local\n     `fp`) shows up as the alias's direct call instead.", qQ27},
	{"header-scope-ratio", "User-header vs system-header include ratio per file", "ANSWERS which files lean almost entirely on system headers (<...>) and so\n     carry little project-local coupling -- and which pull in mostly user\n     headers (\"...\"), marking them as tightly coupled to the tree.\nACT a file at ~0%% user headers is often a portability shim; a file at\n     100%% user headers that is itself widely included deserves a look for\n     layering violations.\nMISLEADS a `<system>` include that RESOLVES inside the tree is counted as\n     user here (the analyzer treats project headers on the include path as\n     internal), so the ratio is about the include SPELLING, not about where\n     the file actually lives.", qQ28},
	{"recursion-loops", "Mutually recursive function pairs (a calls b, b calls a)", "ANSWERS the call pairs that can recurse unboundedly even though no single\n     function calls itself. Each row is a 2-cycle in the resolved edge set.\nACT add a depth cap or an iteration guard on the LOWER-LEVEL member of the\n     pair; the higher one is where the recursion is entered.\nMISLEADS finds direct 2-cycles only. Cycles of length >= 3 (a->b->c->a)\n     need a full SCC walk and do NOT appear; self-recursion is covered by\n     `stack-exhaustion`, not here. Edges are name-resolved, so two functions\n     that share one name are conflated.", qQ29},
	{"global-state-mutation", "Non-static, non-const globals: the state every translation unit shares", "ANSWERS the file-scope objects without internal linkage (no `static`), so\n     any translation unit that declares them extern can mutate them. Where\n     `race-surface` asks which mutable globals two THREADS could clobber\n     (it includes statics and singles out non-atomic ones), this asks\n     which globals merely EXIST as cross-TU seams -- the ones that make a\n     function untestable in isolation and a refactor a hunt through every\n     declaring file.\nACT make them static and route access through one setter, or move them\n     into a context struct passed explicitly.\nMISLEADS the brace scanner cannot see WHICH functions write the global,\n     only that it exists and is shared (`race-surface` is the same data\n     through a thread-race lens); `has_init` being 0 does not prove there\n     is no initializer (a forward-declared extern has none by\n     construction). Config-style `extern const` tables are correctly\n     excluded (is_const). The scanner records every file-scope declarator\n     including function parameters of that shape, so private locals and\n     pointer parameters can surface as rows; shared mutable file-scope\n     objects are the rows that matter.", qQ30},
	{"unreferenced-includes", "Headers included but none of their functions are ever called", "ANSWERS the #include lines whose target file's functions have zero\n     resolved callers anywhere in the tree -- the include-graph shape that\n     slows rebuilds without contributing edges.\nACT drop the include if the header only carried types you no longer use;\n     otherwise expect one of the covered-by-macro / config-gated cases.\nMISLEADS a header used ONLY for types, macros, or constants looks dead\n     here by construction (the scanner sees function calls only); a header\n     whose functions are CALLED THROUGH POINTERS is also invisible. This is\n     a candidate list, not a delete list.", qQ31},
	{"blast-radius", "Symbols with the largest transitive caller sets", "ANSWERS which functions, if changed, can disturb the most of the tree:\n     the count of DISTINCT functions that can reach this symbol through\n     resolved edges at any depth.\nACT treat the top rows as API changes requiring a caller sweep: every\n     row's callers are the blast zone. For a public entry point the number\n     is meaningless by design -- see MISLEADS.\nMISLEADS `n_transitive` counts callers THROUGH RESOLVED EDGES ONLY; a\n     symbol called dynamically (function pointer) or across a name\n     collision undercounts, and a widely-included header's inline helpers\n     are undercounted for the same reason. The pass is an SCC fold over\n     the whole graph (exact on the edges that exist); `call-chain-depth`\n     in METRICS is the same table read DOWNWARD.", qQ32},
	{"cross-file-struct-coupling", "Struct types that span translation units via layout surface", "ANSWERS the struct definitions whose shape (size, pointer fields, padding)\n     makes them load-bearing across files: a struct this large or this\n     pointer-heavy, defined once, is almost certainly passed between\n     translation units and is a compatibility surface.\nACT treat layout changes (field reorder, pointer widening) as ABI changes\n     for every included_by file; the pad columns show where the wasted\n     bytes are.\nMISLEADS the brace scanner cannot see WHERE a struct is instantiated; this\n     ranks by DEFINED shape, not by measured usage, so a huge struct that\n     never leaves its file appears here too. `exact=0` sizes (unknown field\n     types) are excluded rather than guessed.", qQ33},
	{"extern-linkage-density", "Translation units leaning on extern declarations", "ANSWERS where file boundaries are load-bearing on bare extern promises:\n     the count of extern-declared globals a file relies on, and the volume\n     of calls resolved only because a prototype declared the callee (no\n     definition in this unit).\nACT high extern + low definition density is where a symbol rename or a\n     signature change breaks the tree without the compiler naming every\n     victim; consider moving the shared declarations into a header.\nMISLEADS n_external_calls counts calls whose callee was declared but not\n     defined in the unit -- libc and POSIX calls dominate any realistic\n     file, so compare DENSITY across files, not the raw number.", qQ34},
	{"cross-tu-signature-drift", "One function name, two different definitions across TUs", "ANSWERS function names defined with >= 2 different signatures across\n     translation units: UB per C99 6.2.7 (MISRA 8.3). The linker picks\n     one definition and every caller of the other shape is miscompiled.\nACT pick one signature; rename or delete the other definition. The row\n     names every file involved.\nMISLEADS typedef-equivalent types compare different textually, so a\n     benign `int foo(int)` vs `int foo(int32_t)` pair is reported;\n     K&R definitions yield empty signature text and are excluded, and a\n     static fn in one file plus an extern fn of the same name in another\n     is NOT a link conflict yet still reads as drift here.", qQ35},
	{"linkage-scope-mismatch", "External-linkage functions whose callers all live in one TU", "ANSWERS non-static functions whose resolved callers all sit in one\n     translation unit: they should be static (MISRA 8.7, cppcheck). An\n     external-linkage symbol is a coupling surface for the whole\n     binary; a one-TU usage pattern says the author forgot the keyword.\nACT make it static; if fan_in is also 0, `dead-code` owns the deletion\n     question instead.\nMISLEADS name-based resolution undercounts callers, so a function used\n     from a second TU through a macro or a function pointer reads as\n     one-TU here; fnptr-dispatched users are invisible; a header inline\n     absorbed elsewhere never appears. When in doubt, the compiler's\n     own -Wmissing-prototypes is the tiebreaker.", qQ36},
	{"risky-process-apis", "Sites of dangerous process/temp APIs (CERT ENV33-C, flawfinder)", "ANSWERS reachable sites of the dangerous process and temp-file API set:\n     system/popen/exec for process boundaries, mktemp/tmpnam/tempnam for\n     race-prone temp files, access for TOCTOU-prone checks.\nACT replace mktemp with mkstemp; interrogate every system/popen -- each\n     is a command-injection review item when the argument is not a\n     constant; prefer execve with an explicit argv over execl/execvp\n     when the argument list is built at runtime.\nMISLEADS the capture is the call scanner, independent of resolution, so\n     libc calls that would otherwise classify as external still appear;\n     the denylist is the fixed set above -- rand/setenv are NOT captured\n     and are absent by design (their risk is caller-context, which this\n     query does not model); errno-checking callers are not distinguished.", qQ37},
	{"hardcoded-secret-candidates", "Credential-shaped string literals (OWASP G07)", "ANSWERS string literals at least 12 chars long whose text names a\n     credential (password, token, api_key, secret, bearer, jwt, ...) --\n     the literal that a committed secret looks like.\nACT rotate and move to a secret manager; never commit the literal.\nMISLEADS a format string or test fixture containing the WORD token/pass\n     reads as a candidate (the filter is the literal's own text, not its\n     use); values over 200 chars are truncated at capture; a secret\n     built from parts or read from an env var is invisible here. This\n     is a candidate list, not a verdict.", qQ38},
	{"include-cycles", "User headers that include each other, directly or via a chain", "ANSWERS user headers that include each other: the include graph is a\n     DAG in any well-formed project, so a cycle means the headers are\n     mutually dependent and the build order is an accident of include\n     guards.\nACT break the cycle with a forward declaration or by shrinking one\n     header; rows name both endpoints and the cycle length.\nMISLEADS include guards make cycles harmless at compile time, so this\n     is a maintainability smell, not a defect; the cycle set is\n     computed at BUILD time (bounded DFS, depth cap 8) into the\n     include_cycles table -- a recursive-CTE version of this query\n     enumerated paths rather than cycles and never finished on large\n     trees; is_relative=1 means user \"...\" includes, and a <...>\n     include that resolves in-tree is excluded from the walk.", qQ39},
	{"const-cast-away", "Casts that drop const from const-declared names (CERT EXP05-C)", "ANSWERS `(T*)name` casts applied to a name declared const: the\n     const-qualified promise is stripped by the cast, and any write\n     through the result is UB in the caller's face.\nACT remove the cast, or drop const from the declaration if the\n     function genuinely mutates (and say why).\nMISLEADS the const test is against the local/param declaration in the\n     SAME function: a const GLOBAL or a const from another translation\n     unit is invisible here. The cast's TARGET type is not examined, so\n     `(const char *)p` on a const p -- which drops nothing -- and a\n     `(void *)p` handed to a hashing helper are both counted.", qQ40},
	{"fnptr-blindspot-callers", "Functions with no caller whose name is used as a value in a body", "ANSWERS functions with fan_in 0 that some function body names without\n     calling: passed as a callback, stored in a field, or taken with &.\n     They ARE used, but every call goes through a pointer, so every\n     fan-in-based number understates them.\nACT read these as live API surface; a pointer-dispatched function is a\n     plugin point or a registered handler.\nMISLEADS only uses inside function BODIES are here. A name listed in a\n     file-scope initializer (the dominant dispatch-table pattern) is in\n     dispatch-table-orphan instead. `addr_taken` also counts `&x` of a\n     local that happens to share the function's name.", qQ41},
	{"extern-symbol-asymmetry", "Declared in a prototype, never defined in the tree", "ANSWERS names with a prototype/extern declaration but no definition\n     anywhere in the tree: either the definition lives in a library\n     this run skipped (fine), or the symbol is promised but missing\n     (a link error waiting for the first caller).\nACT for each row decide: library boundary (ignore) or genuinely\n     missing (define or delete the prototype).\nMISLEADS a definition behind `#ifdef` that the scan excluded reads as\n     missing; static functions are excluded from the definition side\n     only when their name matches -- a same-named static in one file\n     does NOT satisfy an extern promise in another; libc prototypes in\n     system headers are not scanned (only this tree's files are).", qQ42},
	{"toctou-access-open", "access(X) then open(X) on the same variable (CERT POS01-C)", "ANSWERS functions that both access(X) and open(X) with the same\n     variable: the permission check and the use are two system calls,\n     and the file can be swapped between them. The window is the whole\n     race.\nACT open first, then fstat the descriptor; never trust access for\n     security decisions.\nMISLEADS same-function variable-name pairing, not data flow: access on\n     a path built from a different variable than the open is missed,\n     and access+open on a CONSTANT path (no race on most filesystems\n     in practice) reads the same as the race; `faccessat` with\n     AT_EACCESS is a different capture and is absent.", qQ43},
	{"lock-imbalance", "Acquires a lock more often than it releases it", "ANSWERS the functions where an early return or a goto-out ladder can\n     leave a mutex held: acquire sites outnumber release sites in one\n     body. The next caller to take that lock blocks forever -- and if\n     the lock is taken under load first, the hang looks like a load\n     problem, not a code problem.\nACT pair every acquire with a release on EVERY path, or move to\n     pthread_mutex_trylock with explicit unlock on the failure path.\nMISLEADS counts SITES, not paths: two acquires and two releases can\n     still leak (both releases behind the same `if`), and a function\n     whose contract is 'returns with the lock held' (an allocator's\n     arena lock, a lazy initializer) is correct and lands here anyway.\n     A release inside a called cleanup function is invisible. A call is\n     a lock operation when one WORD of its name is lock, unlock, trylock,\n     rdlock, wrlock, mutex or the like (pthread_mutex_lock, spin_unlock,\n     LOCK): a project wrapper named otherwise is not seen at all.", qQ44},
	{"lock-under-io", "A lock is held across a call that can sleep or block I/O", "ANSWERS where a function both takes a lock and performs raw descriptor\n     I/O in the same body: read/write/recv/send/mmap while holding a\n     pthread mutex. Every other thread wanting that lock stalls for the\n     duration of the I/O -- the classic latency cliff under load, and\n     with a non-recursive mutex plus a nested acquisition, a deadlock.\nACT shrink the critical section: copy what is needed under the lock,\n     release, then do the I/O. If the I/O must be covered, document why\n     and bound it (timeout, O_NONBLOCK).\nMISLEADS co-occurrence in one body is not ordering: the I/O may run\n     strictly before the first acquire or after the last release, and\n     this cannot see which. Buffered stdio (fprintf) is excluded here\n     on purpose -- it lives in its own query family; a blocking call\n     reached THROUGH a callee while the lock is held is invisible.", qQ45},
	{"memcpy-sizeof-mismatch", "Copy whose size argument names neither dst nor src", "ANSWERS the copy sites where the SIZE expression dereferences a\n     buffer that is neither argument: memcpy(a, b, sizeof(c)) copies\n     by c's width -- CERT ARR38-C's shape when a, b and c have\n     different element sizes. `size_buf` is the identifier the size\n     expression references (from its sizeof), '' when it names none.\nACT read every row: the fix is a size naming THE DESTINATION, or an\n     explicit MIN of both. Prefer strlcpy / memcpy_s where available.\nMISLEADS textual, not semantic: sizeof(*dst) vs sizeof(struct x) can\n     be equal; copying into a larger dst is legal; a length VARIABLE\n     as size reads clean here but is exactly where an unchecked value\n     bites -- this query finds the WRONG-BUFFER sites, not every bad\n     copy. Argument capture stops at one paren level.", qQ46},
	{"alloc-without-sizeof", "malloc(n * k) with no sizeof in the size expression", "ANSWERS allocation sites whose size expression names no sizeof at all:\n     malloc(count * 4) hard-codes the element width, malloc(len) may be\n     confusing bytes with elements, and neither survives the element\n     type changing under it. Each row is the exact line to re-derive.\nACT write sizeof(*p) (or sizeof(T)) explicitly, and prefer calloc for\n     arrays so the overflow check is someone else's already-written bug\n     fix.\nMISLEADS a byte-oriented API (a raw network buffer, a string + 1) is\n     CORRECT without sizeof and will dominate this list in most trees;\n     a size expression built through a project macro (`s_malloc`,\n     `ALLOC_ARRAY`) hides its sizeof from this scan entirely.", qQ47},
	{"global-writer-concentration", "Functions that WRITE file-scope state, ranked by how much they touch", "ANSWERS which functions mutate shared file-scope objects, and which\n     globals are written from the most places. `race-surface` says a\n     global EXISTS and is mutable; this says WHO actually writes it --\n     the half that turns 'could race' into 'here is the commit that\n     must take the lock'.\nACT every writer of a hot global is a serialization point: batch the\n     writes, shard the state, or make the field atomic.\nMISLEADS same-file globals only -- writes through an extern declared\n     in a header are invisible, so cross-TU writers undercount; a\n     local variable shadowing a global name is counted as the global;\n     and `g->field = x` through a pointer parameter reads as a write\n     only when the base name matches a captured global.", qQ48},
	{"truncating-cast-flow", "Fixed-width narrowing casts on data that came from wide sources", "ANSWERS `(uint32_t)x`-style casts to a STRICTLY narrower fixed-width\n     type in functions that also do heavy arithmetic or I/O -- the\n     places where a 64-bit length, hash or offset silently becomes 32\n     bits. The wrap is not UB, which is exactly why nothing complains:\n     the value just comes back wrong on input above 4 GiB.\nACT before casting, range-check and reject; after casting, never use\n     the wide original again. Prefer size_t end-to-end.\nMISLEADS a cast to uint8_t for serialization into a wire format is\n     correct and common; the scan cannot know the value fits. Only\n     fixed-width targets are counted -- casts to int/long are\n     platform-dependent and deliberately absent.", qQ49},
	{"signed-size-compare", "Signed value compared against a size or sizeof result", "ANSWERS comparisons like `i < sizeof(b)` or `n <= len` where one side is a\n     local or parameter DECLARED signed (int, long, ssize_t ...) and the\n     other is a sizeof, a strlen, or a name declared size_t/unsigned. The\n     signed side converts, so -1 becomes SIZE_MAX and the bounds check\n     PASSES for the one input that must fail it (CERT INT31-C).\nACT make the index size_t, or compare in the signed domain against a\n     checked cast of the size.\nMISLEADS types come from declarations in the same function only: a struct\n     member, a global or a call other than strlen has no known type and is\n     never the size side, so this list is a floor. A value proven\n     non-negative before the compare makes its row harmless.", qQ50},
	{"variadic-format-forwarder", "Variadic function forwarding to a vprintf-family call", "ANSWERS the wrappers that turn caller-supplied arguments into formatted\n     output: variadic definitions containing v?snprintf/v?printf/syslog.\n     Whether %n is reachable from outside depends ENTIRELY on whether\n     these forwarders pass through a caller-controlled format string --\n     they are the chokepoints worth auditing, because every other\n     printf-shaped call in the tree funnels through a handful of them.\nACT audit each: the format argument should be a literal or a fixed\n     table entry, never a parameter forwarded verbatim.\nMISLEADS a wrapper that hardcodes its own literal format ('%s') is safe\n     and indistinguishable here from one forwarding the caller's format\n     through; the format STRING itself is blanked during scanning (it is\n     a literal), so its content is not inspected.", qQ51},
	{"dispatch-table-orphan", "Referenced ONLY in file-scope initializers: live but zero fan-in", "ANSWERS functions kept alive exclusively by a dispatch-table entry at\n     file scope -- an initializer reference (`static cmd_t cmds[] =\n     {&get, &set}`) or a generated-table argument\n     (`MAKE_CMD(...,getCommand,...)`). No body contains the use, so\n     dead-code and every fan-in-based number read zero while the\n     function is very much alive: these are the plugin points of a\n     table-driven C program.\nACT treat as public API: renaming one breaks a TABLE, silently, at\n     runtime. Any signature change needs the table's owning module in\n     the review. `refs` says how many tables list it.\nMISLEADS the capture is textual (argument position, file scope, macro\n     bodies skipped), so a same-named DATA object referenced in an\n     initializer can attach to a function of that name after pruning;\n     a table entry spelled through a project macro's PARAMETER is not\n     seen (macro bodies are excluded on purpose); and a function whose\n     address is taken BOTH in a table and in a body appears here only\n     for its body-level rows.", qQ52},
	{"error-path-frees", "Allocations whose frees sit only behind error branches", "ANSWERS functions that allocate, free, and branch heavily: the shape\n    where the free list covers the error ladder but a NEW success-path\n    early return skips it. Unlike ownership-review (which flags 'no\n    free at all'), this finds the functions where a future edit is most\n    likely to introduce a leak, because the discipline exists but is\n    one return away from breaking. `allocs` counts direct\n    malloc/calloc/realloc/alloca sites; project wrappers are invisible\n    to it.\nACT when editing one of these, trace every new return against the free\n    ladder -- or convert to goto-cleanup, which makes skipping it\n    impossible.\nMISLEADS a heuristic by construction: alloc==free==1 with two branches\n    is usually fine; the ranking surfaces DENSITY of exits vs frees,\n    not an actual missing free. Transfers of ownership (returning the\n    buffer) look identical, and frees reached through a wrapper\n    function do not count, so a disciplined function using one can\n    read as frees=0 -- those land in ownership-review instead.", qQ53},
	{"realloc-idiom-risk", "p = realloc(p, n): the old block is lost on failure", "ANSWERS where a failed realloc leaks the buffer it was resizing.\nACT assign to a TEMPORARY: `tmp = realloc(p, n); if (!tmp) { /* p is still\n     valid */ } else p = tmp;`.\nMISLEADS matched textually on `x = realloc(x,`, so a realloc through a wrapper,\n     or one whose result lands in a different variable, is missed. A realloc to\n     a SMALLER size does not fail in practice, so some rows are noise.\n", qQ54},
	{"alloc-size-off-by-one", "An allocation sized from strlen() with no room for the terminator", "ANSWERS where the size expression is a string length and nothing adds one.\nACT MEM35-C: `strlen(s) + 1`, or use strdup, which cannot get this wrong.\nMISLEADS only sites whose size text contains `strlen` AND no `+` at all are\n     listed, so `strlen(s) + sizeof(hdr)` is excluded -- correctly, since that\n     may be a counted buffer. The size is TEXT, not a value: a length computed\n     two statements earlier is invisible here.\n", qQ55},
	{"resource-leak-surface", "Descriptors and FILEs opened on more paths than they are closed", "ANSWERS where an open outnumbers its close inside one function.\nACT FIO42-C: every open needs a close on EVERY path, including the error\n     returns and the goto-out ladder. Prefer one cleanup label.\nMISLEADS a function that opens and hands the descriptor to its caller is\n     correct and looks identical. Opens and closes reached through a wrapper\n     are not counted, and a resource closed by the caller reads as a leak.\n", qQ56},
	{"unchecked-alloc-result", "Allocates and never compares anything to NULL", "ANSWERS functions where no allocation result is checked at all.\nACT ERR33-C: on a 25k-function tree this list is long and mostly test code and\n     wrappers -- narrow it with --module and read the high fan_in rows first.\nMISLEADS `n_null_check` counts `== NULL` and `if (!x)` only. A check inside a\n     macro (`serverAssert(x)`), a helper, or an `assert(x)` is not seen, and a\n     wrapper that checks centrally makes every one of its callers look guilty.\n", qQ57},
	{"vla-from-input", "A variable-length array whose extent someone else chose", "ANSWERS stack allocations sized by a non-constant, worst where the length\n     arrives from I/O or from a string operation.\nACT ARR32-C: bound the length before the declaration, or heap-allocate. A VLA\n     has no failure mode -- it simply writes past the frame.\nMISLEADS the filter is `n_io>0 OR n_memory>0 OR recursive`, a PROXY for\n     \"the length may be attacker-influenced\", not evidence of it. A VLA sized\n     from a local constant is still listed if the function also does I/O.\n", qQ58},
	{"free-then-deref", "A name is freed and then dereferenced later in the same body", "ANSWERS the MEM30-C use-after-free shape that one function can contain.\nACT every row needs a read of the code: the dereference may sit on a path where\n     the pointer was reassigned, which is correct.\nMISLEADS textual and scope-limited. It cannot see that the two are on mutually\n     exclusive branches, or that the pointer was reassigned between them.\n     `p = malloc(...)` after a free is excluded; `*p = ...` is counted. A free\n     at the bottom of a loop that reallocates at the top is the common false\n     positive -- and the loop column is the only hint offered.\n", qQ59},
	{"weak-randomness", "rand() and friends where unpredictability might matter", "ANSWERS MSC30-C: the places a predictable generator is used.\nACT anything key-, token-, nonce-, or salt-related here is a finding.\n     Everything else -- shuffling test data, jitter, backoff -- is fine and is\n     still listed.\nMISLEADS presence, not reachability. Nothing here says the value is observable\n     by an attacker; the `io` and `exec` columns are the only evidence offered.\n", qQ60},
	{"shift-count-risk", "Shifts by an identifier rather than a literal", "ANSWERS INT34-C: shift counts the reader cannot check at the shift site.\nACT a count that can be negative, or >= the width of the promoted left operand,\n     is undefined. Clamp at the source, not at every shift.\nMISLEADS the great majority of these are correct bit-twiddling with a constant\n     named for readability. This ranks where to LOOK, and the count is of SITES,\n     not of bugs.\n", qQ61},
	{"errno-dependence", "Where the errno error channel is load bearing", "ANSWERS the functions whose error reporting depends on a global that any\n     intervening call can overwrite.\nACT ERR30-C: for each row, check that nothing runs between the failing call and\n     the errno read. Ranked by read count, because that is where a clobber hurts.\nMISLEADS it cannot tell a correct read from a stale one -- only that the code\n     consults errno at all. A function that reads errno correctly on every path\n     and one that reads it after three intervening printf calls look identical.\n", qQ62},
	{"env-trust-boundary", "getenv(): the process environment as an input", "ANSWERS every place the environment is read.\nACT the environment is attacker-controlled wherever the process was started by\n     a less trusted one. A length from getenv needs the same bounds treatment\n     as a length from a socket.\nMISLEADS presence only. A path from getenv used with an explicit length is fine;\n     one concatenated into a command or a path is not. The `exec` and `mem`\n     columns are the triage hint, not a verdict.\n", qQ63},
	{"assert-with-side-effect", "An assert() that does work, which NDEBUG deletes", "ANSWERS assertions whose removal would change behaviour.\nACT move the work out of the assert and assert on its RESULT. Under NDEBUG the\n     assert vanishes and so does whatever it did.\nMISLEADS the match is a shape (`=`, `++`, memcpy, malloc, printf inside the\n     parens), so `assert(x == y)` is excluded correctly but `assert(f(&x))` with\n     a read-only f is still counted. Test files are excluded; test helpers that\n     live in source files are not.\n", qQ64},
	{"lock-order-inversion", "Two locks taken in both orders by different functions", "ANSWERS the CON35-C / POS51-C deadlock shape.\nACT for each row decide whether the two functions can run at the same time.\n     If yes, this is a deadlock waiting on scheduling; if no, it is an invariant\n     someone should write down next to the lock.\nMISLEADS order is inferred from LINE NUMBER inside a function, not from control\n     flow: two acquires on one line, or one inside a conditional that actually\n     runs first, are ordered by text. Only lock-SHAPED identifiers are recorded,\n     so a lock held in a variable (`myLockAcquire(lock, ...)`) is invisible and\n     every ordering involving it is missed entirely.\n", qQ65},
	{"false-sharing-risk", "Two or more fields sharing one 64-byte cache line", "ANSWERS where independent writes to one struct will invalidate each other.\nACT only matters if different threads write different fields of one instance.\n     Aligning the hot field is the cheap fix; splitting the struct is the real\n     one.\nMISLEADS nothing here knows which fields are written by which thread. The\n     `conc_fns_in_module` column is the only evidence offered: how many\n     functions in the same module touch a concurrency primitive at all. Offsets\n     are LP64 and reported only where the layout was computed exactly.\n", qQ66},
	{"unterminated-strncpy", "strncpy/strncat sized with sizeof: no terminator guaranteed", "ANSWERS the STR31-C / STR32-C shape: a copy that fills the buffer exactly and\n     adds no NUL when the source is that long or longer.\nACT `strncpy(d, s, sizeof d - 1); d[sizeof d - 1] = '\\0';`, or use a function\n     that always terminates.\nMISLEADS matched on the SIZE ARGUMENT TEXT, so `strncpy(d, s, n)` with a runtime\n     n -- which can overrun for a different reason -- is absent. A size that is\n     provably shorter than the buffer is listed anyway.\n", qQ67},
	{"struct-padding-leak", "Structs with alignment holes that are copied or written whole", "ANSWERS DCL39-C: where padding bytes can be stored or transmitted.\nACT if the struct crosses a trust boundary -- to disk, to the wire, to another\n     process -- serialise it field by field, or pack and re-order it so there\n     are no holes.\nMISLEADS `copied_whole` counts memcpy-family sites in the same FILE as the\n     struct, not copies OF that struct: it is a proxy, not a data-flow fact.\n     Padding is computed only where every field's type could be sized, so a\n     struct containing an unknown typedef is absent rather than wrong. There\n     is no fan_in column: nothing calls a struct, so it would read 0 on\n     every row of every corpus -- the ranking is by pad bytes and copy\n     sites instead.\n", qQ68},
	{"dead-static", "static functions nothing calls and whose address is never taken", "ANSWERS unreachable code, with the stronger claim `static` allows.\nACT `static` means no other translation unit can reach it, so unlike dead-code\n     this is a candidate for deletion rather than for investigation.\nMISLEADS a static function reached through a function pointer assigned in a\n     macro or a generated table escapes the addr_taken test and appears here\n     while alive. Grep the name as a string before deleting anything.\n", qQ69},
	{"missing-prototype", "External functions defined in a .c with no prototype anywhere", "ANSWERS MISRA-C 8.2 / CERT DCL40-C: where callers get no signature check.\nACT add the prototype to the header that owns this module. Without one, every\n     caller in another translation unit is calling an implicitly-declared\n     function and an argument mismatch is silent.\nMISLEADS a prototype in a header this run did not parse -- vendored, generated,\n     or excluded -- is invisible, so those rows are wrong. It also does not\n     check that a prototype which DOES exist agrees with the definition.\n", qQ70},
	{"header-hygiene", "Headers with no inclusion guard", "ANSWERS headers that cannot safely be included twice.\nACT one line: `#ifndef FOO_H` / `#define FOO_H` ... `#endif`, or `#pragma once`.\nMISLEADS a guard is detected as an `#ifndef` in the first 12 lines, so a header\n     that uses ONLY `#pragma once` is reported as unguarded, which is wrong --\n     the preprocessor table does not record pragmas. Ranked by how many files\n     include it, since an unguarded header nobody includes twice harms nobody.\n", qQ71},
	{"module-cycles", "Module pairs that can each reach the other through calls", "ANSWERS the dependency cycles: seams that cannot be cut.\nACT a cycle means neither side can be built, tested, or reasoned about alone.\n     Break it by inverting one edge -- a callback, an interface header, an event.\nMISLEADS depth-bounded at 4 hops in each direction, so a longer return path\n     reads as no cycle. Modules are two path components, so a cycle WITHIN one\n     module is invisible here by construction.\n", qQ72},
	{"naming-coherence", "Functions that break their own file's dominant name prefix", "ANSWERS C's missing namespace, made visible.\nACT either the function is misplaced and belongs in the file that owns its\n     prefix, or the file is doing two jobs and should be split.\nMISLEADS the dominant prefix is simply the most common one in the file, so a\n     file with no dominant convention is skipped entirely (fewer than three\n     users) and a file with two legitimate conventions reports half of itself.\n     The prefix is the text before the first underscore.\n", qQ73},
	{"untested-surface", "Widely-called functions with no caller in any test file", "ANSWERS where a regression would ship.\nACT these are the highest-value test targets: high fan_in means a bug here\n     reaches far, and no test currently reaches it at all.\nMISLEADS \"reached by a test\" means a resolved call edge from a file this run\n     classified as a test. A test that drives the code through a socket, a\n     subprocess, or a function pointer produces no edge and reads as untested.\n", qQ74},
	{"io-in-loop", "A syscall inside a loop body: one per iteration", "ANSWERS where the syscall count scales with the data.\nACT batch it: readv/writev, a larger buffer, or hoist the call out of the loop.\nMISLEADS a loop over a handful of descriptors is correct and is listed. The\n     count is of call SITES in loop bodies, so one site in a hot loop beats ten\n     in a cold one -- which the ranking cannot see. `io_in_loop` has been\n     captured and indexed since the base schema and no query read it until now.\n", qQ75},
	{"lock-convoy", "A lock acquired inside a loop body", "ANSWERS where a lock is taken once per iteration instead of once per pass.\nACT hoist it, or take it around the batch. The cost is not the lock, it is the\n     queue behind it.\nMISLEADS a lock released at the end of each iteration may be REQUIRED -- a\n     per-item invariant, or a condvar wait. Loop depth is the triage hint:\n     depth 1 over a short loop is rarely the problem.\n", qQ76},
	{"branch-per-iteration", "Branches inside loop bodies, ranked by how many", "ANSWERS where the predictor re-decides on every iteration.\nACT only worth acting on where the branch is data-dependent. The switch and\n     call columns are the ones that usually are.\nMISLEADS this is the single most common shape in real code -- every loop has at\n     least one branch, including its own condition. It is a ranking of VOLUME,\n     not a finding, and a perfectly predicted branch costs almost nothing.\n", qQ77},
	{"et-without-drain", "Edge-triggered registration with no EAGAIN drain anywhere in the function", "ANSWERS the stall. epoll(7): with EPOLLET the caller \"must consider it ready\n     until the next (nonblocking) read/write yields EAGAIN\". Reading once and\n     moving on leaves the rest of the buffer in the kernel, and the next\n     epoll_wait() blocks forever -- the peer is waiting on a reply to bytes\n     this process has already been told about. EV_CLEAR is kqueue's spelling\n     of the same contract.\nACT loop until EAGAIN, or drop EPOLLET/EV_CLEAR and take level-triggered.\nMISLEADS the drain usually lives in a DIFFERENT function from the\n     registration -- a helper reads until EAGAIN and this ranks the registrar,\n     because that is the only place the flag is visible. Both counts are\n     per function and a project that centralises the drain will list every\n     registration site. It also cannot see that the descriptor is a datagram,\n     where one read really can consume everything.\n", qQ78},
	{"event-wait-eintr-blind", "A blocking wait whose return value is used without any EINTR handling", "ANSWERS where a delivered signal becomes a spin or a silent stall.\nACT epoll_wait(2), kevent(2) and io_uring_wait_cqe(3) all return -1 with\n     errno EINTR on any signal. Test for it and retry; do not fall through\n     into the loop with a negative count.\nMISLEADS the test is the presence of the token EINTR in the function -- a\n     retry in a shared wrapper does not make the caller clean, and a\n     `continue` on EINTR in a loop this function does not contain is\n     invisible. Conversely a function that names EINTR for an unrelated\n     syscall reads clean.\n", qQ79},
	{"write-ready-never-cleared", "Write-readiness registered and never withdrawn: a level-triggered busy loop", "ANSWERS the 100%-CPU-forever shape.\nACT EPOLLOUT and EVFILT_WRITE are level-triggered, so they are ready whenever\n     the socket can take a single byte -- which is nearly always. Register\n     them only when there is something queued, and remove them with\n     EPOLL_CTL_DEL / EV_DELETE the moment the queue drains.\nMISLEADS the deregistration may be in another function, and a project that\n     manages its write interest centrally will show every ADD site here.\n     EV_DELETE is also the normal teardown path, so a function that registers\n     and then tears down the whole connection counts as clean only if the\n     two are in the same body.\n", qQ80},
	{"event-error-flag-unhandled", "A poller that never looks at the error or hangup flags", "ANSWERS where disconnects and registration failures are silently dropped.\nACT epoll(7) delivers EPOLLERR and EPOLLHUP whether or not they were\n     requested, and kqueue(2) sets EV_EOF and EV_ERROR the same way. Test\n     them before the read/write flags, or a closed peer is reported as a\n     readable descriptor forever.\nMISLEADS the flags may be handled by a shared dispatcher rather than here,\n     which is the common and correct design; this ranks the functions that\n     own the mask, not the ones that get it right. A loop that only ever\n     watches timers or signals has no error flag to handle and still appears.\n", qQ81},
	{"oneshot-never-rearmed", "Delivered-once registration with no re-arm: the descriptor goes silent", "ANSWERS where a file descriptor stops reporting events permanently.\nACT EPOLLONESHOT disables the descriptor after one delivery and it stays\n     disabled until epoll_ctl(2) with EPOLL_CTL_MOD re-arms it -- ADD does\n     not re-arm, it only registers. EV_DISPATCH on kqueue needs EV_ENABLE.\n     Either re-arm or drop the flag.\nMISLEADS EPOLL_CTL_ADD is deliberately NOT counted as a re-arm, so a\n     register/delete/re-add cycle reads as unre-armed even though it works.\n     The re-arm is also usually in the I/O handler, not the registering\n     function.\n", qQ82},
	{"uring-res-unchecked", "CQEs consumed without ever testing res: io_uring errors are invisible", "ANSWERS where every I/O error becomes a successful zero-length operation.\nACT io_uring(7) is explicit: \"errno is never used for passing back error\n     information. Instead, res will contain ... -errno.\" Test `cqe->res < 0`\n     on every completion, including the ones from a batched submit.\nMISLEADS the test is a textual match for `res` in the function, so a helper\n     that checks it centrally leaves every caller listed. Multi-shot and\n     IORING_CQE_F_MORE completions need the check on each one, which this\n     cannot distinguish from a single check.\n", qQ83},
	{"uring-errno-confusion", "io_uring used together with errno: the wrong error channel", "ANSWERS a porting bug, and the most common one when synchronous I/O is\n     converted to io_uring.\nACT res carries the error. Reading errno after an io_uring operation reports\n     whatever the last unrelated call left behind -- and under SQPOLL there\n     may have been no syscall at all to set it.\nMISLEADS errno may be read here for a genuinely synchronous call in the same\n     function -- an open(), an mmap(), a malloc() failure. The columns show\n     both counts side by side precisely so that judgement is possible;\n     nothing here proves the errno belongs to the io_uring call.\n", qQ84},
	{"uring-sqpoll-free-race", "SQPOLL ring with a free in the same function: freed before completion", "ANSWERS a use-after-free that the kernel dereferences, not this process.\nACT io_uring(7): without SQPOLL a submitted pointer only has to stay valid\n     until io_uring_submit() returns; WITH IORING_SETUP_SQPOLL it \"must\n     remain valid until completion\". Hold the buffer until its CQE arrives,\n     or drop SQPOLL.\nMISLEADS the free may be of something entirely unrelated to the submission,\n     and a function that both submits and frees a scratch buffer is listed\n     regardless of whether the two are connected. The flag is captured from\n     the body, so a ring configured in another function and passed in is\n     invisible here. There is no fan_in column: SQPOLL setup with a free\n     in the same body is nearly always a test's main(), which has no\n     callers, so the number measured 0 on every row of every corpus.\n", qQ85},
	{"uring-ring-without-barrier", "Shared ring head or tail touched with no acquire/release barrier", "ANSWERS a data race on memory the kernel is writing at the same time.\nACT the head and tail are shared with the kernel and both sides write them,\n     so io_uring(7) requires an acquire on the way in and a release on the\n     way out. Use the io_uring_smp_load_acquire / io_uring_smp_store_release\n     helpers or their C11 equivalents; a plain read is not sufficient.\nMISLEADS the ring reference is matched by NAME SHAPE -- an identifier\n     containing `ring` followed within a short distance by `head` or `tail`\n     -- so a project's own ring that does not use the word, or a raw\n     integer index into the ring, is missed. liburing wraps all of this and\n     a ring accessed only through it is correctly absent.\n", qQ86},
	{"event-loop-alloc-per-event", "Allocation on the event path: one malloc per event", "ANSWERS where the allocator is on the hot path of the whole process.\nACT per-event allocations dominate a saturated loop. Use a pool, a slab, or\n     an object cache keyed by the descriptor; the allocation should not scale\n     with the event rate.\nMISLEADS the allocation may be a one-time setup that merely shares a body\n     with the wait, and the count is of SITES, not of events. A loop that\n     allocates only on connection establishment -- not per event -- looks\n     identical here.\n", qQ87},
	{"event-wait-no-timeout", "A wait primitive called with an indefinite or zero timeout", "ANSWERS which loops can never do periodic work, and which never sleep.\nACT two opposite bugs, both from the same argument. NULL or -1 blocks until\n     an event arrives, so the loop can never run a timer, check a shutdown\n     flag, or re-arm -- the usual fix is a bounded timeout plus a periodic\n     tick. 0 turns the wait into a poll, and a loop polling at 0 burns a\n     core to save microseconds of latency it usually does not need.\nMISLEADS an indefinite wait is CORRECT wherever the loop has nothing to do\n     between events -- postgres's WaitEventSetWaitBlock blocks forever\n     precisely because no timeout was requested. This ranks the property,\n     not the defect; read the fan_in column before acting. Only epoll_wait\n     and kevent are analysed: io_uring's wait calls end in an out-parameter\n     or a count, so \"the last argument is the timeout\" is not true there.\n     A kevent with nevents of 0 is a registration, not a wait, and is\n     skipped -- counting it was 2 of redis's first 3 rows. There is no\n     fan_in column: an event-loop poll function is almost always reached\n     through a dispatch-table pointer, so it has no callers and the number\n     measured 0 on every row of every corpus.\n", qQ88},
	{"et-without-nonblocking", "Edge-triggered registration on a descriptor never made non-blocking", "ANSWERS the other half of the edge-triggered contract.\nACT epoll(7): an EPOLLET application \"should use nonblocking file\n     descriptors to avoid having a blocking read or write starve a task that\n     is handling multiple file descriptors\". Set O_NONBLOCK (or accept with\n     SOCK_NONBLOCK) before registering.\nMISLEADS the flag is usually set somewhere ELSE -- at accept time, in the\n     listen socket, or by a shared helper -- so a correct loop is listed\n     whenever the descriptor was made non-blocking in a different function.\n     EAGAIN handling and a non-blocking descriptor are two separate halves:\n     et-without-drain ranks the first, this ranks the second, and a loop can\n     fail either one.\n", qQ89},
	{"event-fd-leak", "Creates a poller instance and never releases anything in the function", "ANSWERS where an epoll descriptor, a kqueue, or a ring is dropped.\nACT these are descriptors (and for io_uring, two shared mappings). close()\n     the epoll fd and the kqueue, and call io_uring_queue_exit() with any\n     registered buffers or files unregistered first -- on every error path,\n     not only the happy one.\nMISLEADS the release test is any close()/queue_exit()/unregister() call in\n     the same function, so a function that closes an unrelated descriptor\n     reads clean, and a constructor that hands the instance to its caller\n     -- which is the correct design -- is listed as a leak. Ownership\n     transfer is invisible here.\n", qQ90},
	{"uring-sqe-null", "io_uring_get_sqe() whose result is never checked for NULL", "ANSWERS the most common io_uring bug there is.\nACT io_uring_get_sqe() returns NULL when the submission queue is full. Keep\n     the queue drained, size it for the intended depth, and test the pointer\n     before filling it in -- otherwise a busy ring is a NULL dereference.\nMISLEADS the test is the function-wide count of `== NULL` and `if (!x)`, so\n     an unrelated NULL check anywhere in the function makes it read clean,\n     and a check inside a shared helper does not. Ranked by how many SQEs\n     the function takes, because that is how often it can be NULL.\n", qQ91},
	{"uring-cqe-never-seen", "Completions waited for but never returned to the kernel", "ANSWERS where the completion ring can fill and stall the program silently.\nACT every CQE has to be marked seen -- io_uring_cqe_seen(), or\n     io_uring_cq_advance() for a batch. Until it is, the slot stays occupied;\n     when the ring is full the kernel cannot post another completion and the\n     ring stops making progress with no error anywhere.\nMISLEADS a function that peeks rather than waits, or one that hands the CQE\n     to a helper that advances it, is listed. liburing's *_nr and\n     for_each_cqe helpers all advance in the caller, so this is usually a\n     true positive; the gap is a project's own wrapper.\n", qQ92},
	{"uring-user-data-unset", "More SQEs taken than user_data values set: completions cannot be correlated", "ANSWERS where a completion cannot be matched to the request it belongs to.\nACT io_uring(7) is explicit that completions arrive in ANY order and that\n     \"the most common method\" of matching them is the user_data field. Set\n     it on every SQE that shares a ring with another in-flight request, or\n     use liburing's io_uring_sqe_set_data().\nMISLEADS this only matters when more than one request is in flight at a\n     time, so the query requires at least 2 SQEs; a submit-one-wait-one loop\n     legitimately needs none. The comparison is of COUNTS, not of identity:\n     a function that sets user_data on one SQE and not on the other reads as\n     half-clean. liburing tests that reuse a single request are listed too.\n", qQ93},
	{"uring-link-ordering", "Stream sends or receives with no IOSQE_IO_LINK anywhere", "ANSWERS where two stream operations on one socket can be reordered.\nACT io_uring(7): \"it is generally unsafe to have more than one outstanding\n     send, or more than one outstanding receive ... on a given socket at a\n     time, as the kernel may reorder their execution.\" Link them with\n     IOSQE_IO_LINK, submit them in one batch, or wait between them.\nMISLEADS two sends of INDEPENDENT data -- a header and a separate response,\n     on different sockets -- are fine unlinked, and this cannot tell them\n     apart: it sees prep_send twice and no link flag. Datagram sockets are\n     unaffected by the ordering rule and are still listed.\n", qQ94},
	{"uring-teardown-incomplete", "A ring initialised in this function with no io_uring_queue_exit", "ANSWERS where a ring's descriptor and its two shared mappings are dropped.\nACT io_uring_queue_exit() releases the ring, the mappings and the\n     descriptor. Registered buffers and files must be unregistered first.\n     Put it on every exit path.\nMISLEADS a function that builds a ring and hands it to its caller is\n     correct and is listed -- ownership transfer is invisible to a\n     per-function count. Short-lived test and example code frequently lets\n     process exit do the cleanup, which is why this ranks high on trees\n     that are mostly tests.\n", qQ95},
	{"kevent-timer-zero", "EVFILT_TIMER registered with a data value of zero", "ANSWERS a timer that does not mean what it says.\nACT kqueue(2): \"Periodic timers with a specified timeout of 0 will be\n     silently adjusted to timeout after 1 of the time units specified by\n     the requested precision in fflags.\" At NOTE_NSECONDS that is a\n     one-nanosecond periodic timer -- the loop never sleeps and the process\n     pins a core. Pass the interval you meant, or use EV_ONESHOT.\nMISLEADS only EV_SET's data argument is inspected, so a timer whose value\n     is assigned afterwards (`kev.data = 0`) is missed, and a data of 0 that\n     is genuinely intended as \"one unit\" reads as a bug. NOTE_ABSTIME\n     timers are exempt in spirit but not in this test.\n", qQ96},
	{"kevent-batch-errors-ignored", "EV_RECEIPT bulk registration whose EV_ERROR events are never read", "ANSWERS where a failed registration is silent.\nACT kqueue(2): with EV_RECEIPT, \"EV_ERROR [is] always returned. When a\n     filter is successfully added the data field will be zero.\" The failures\n     arrive as EVENTS, not as a return value -- so iterate the eventlist and\n     test EV_ERROR on each, or drop EV_RECEIPT and check kevent()'s return.\nMISLEADS the EV_ERROR test may live in a shared dispatcher. This is the\n     rarest of the kqueue shapes and neither test corpus uses EV_RECEIPT at\n     all, so its only evidence is the fixture.\n", qQ97},
	{"blocking-call-in-event-loop", "A loop that waits, and also blocks on something else inside the loop", "ANSWERS where one slow descriptor stalls all the others.\nACT an event loop must block on exactly one thing: the wait primitive. A\n     synchronous read inside the body stalls every other descriptor for its\n     duration, and a lock taken in the body serialises every thread that\n     wants the same lock. Move the work off the loop, or make it\n     non-blocking and continue on the next event.\nMISLEADS the per-loop counters cover EVERY loop in the function, not only\n     the event loop, so a helper called from the loop that happens to lock\n     makes the whole function look blocking. Locking to update a shared\n     statistic is usually correct and cheap; the ranking cannot tell a\n     contended lock from an uncontended one.\n", qQ98},
	{"snprintf-truncation-ignored", "snprintf's return is never compared to anything", "ANSWERS where truncated output is silently accepted.\nACT snprintf returns the length it WOULD have written. Compare it against\n     the buffer size and treat `>= sizeof buf` as truncation. Otherwise\n     \"fit exactly\" and \"lost 400 bytes\" are the same result.\nMISLEADS the guard test is function-wide (`< 0`, `== -1`, `>= size`), so a\n     check for an unrelated call makes a row read clean. This is the single\n     largest rule in the pack by measured volume: 296 rows across the four\n     corpora, so triage by fan_in rather than reading top to bottom. GCC's\n     `-Wformat-truncation` catches some of these at compile time, but only\n     when the buffer size is known there.\n", qQ99},
	{"stdio-result-unchecked", "fread/fwrite/fscanf return values discarded", "ANSWERS where a short read, a failed write, or an unconverted field passes.\nACT fread returns a COUNT, not a success flag -- compare it to the count you\n     asked for. fscanf returns the number of conversions; check it equals the\n     number of specifiers, or the untouched variables hold garbage.\nMISLEADS function-wide guard test. A loop that checks nothing but writes\n     into a buffer it then validates another way is a false positive; there\n     is no way to see that from here.\n", qQ100},
	{"string-conversion-unchecked", "strtol-family result used without errno and end pointer", "ANSWERS where \"0\", \"not a number\" and \"overflow\" are indistinguishable.\nACT strtol fails three ways. Set `errno = 0` first, pass a real end pointer,\n     then check endptr != input AND errno != ERANGE. One without the other\n     is not a check.\nMISLEADS requires BOTH guards, so a function that clears errno but ignores\n     the end pointer is listed. `atoi` is worse still and is a separate\n     smell this query does not cover.\n", qQ101},
	{"ctype-signed-char-ub", "isalpha/toupper called on a plain char", "ANSWERS a latent undefined behaviour that is invisible until a byte >= 0x80.\nACT ctype functions are defined only for values representable as an unsigned\n     char, or EOF. Pass `(unsigned char)c`, or hold the byte in an `int`.\n     With a negative `char` the result is undefined -- typically a read\n     outside the table.\nMISLEADS the guard is `(unsigned char)` anywhere in the function, so a cast\n     used for an unrelated conversion makes a row read clean; and a char\n     that provably never goes negative (ASCII literals) is still listed.\n     ctype had ZERO coverage before this rule.\n", qQ102},
	{"sleep-eintr-unhandled", "nanosleep/usleep/sleep with no EINTR handling", "ANSWERS where a signal shortens a sleep and nobody notices.\nACT on -1/EINTR the remainder is undone. Retry with the remainder\n     (nanosleep writes it into the second argument) or the sleep is short.\nMISLEADS the test is the token EINTR anywhere in the function. Some sleeps\n     genuinely should be interruptible -- an alarm-driven timeout loop is\n     correct and is listed.\n", qQ103},
	{"socket-partial-write", "send/write return never compared: a short write truncates", "ANSWERS where a message can be silently truncated at the receiver.\nACT send/write return the number of bytes accepted, which is often less than\n     the length. Loop until everything is sent, or treat a short count as an\n     error, depending on the protocol.\nMISLEADS fires on any send/write in an unguarded function, including writes\n     to a blocking file where a short write genuinely cannot happen. It\n     cannot tell a socket from a file descriptor -- the two share write().\n", qQ104},
	{"mman-never-unmapped", "mmap with no munmap anywhere in the function", "ANSWERS where address space is acquired and never released.\nACT pair every mmap with munmap on every path. On a long-running daemon this\n     is a slow exhaustion rather than a crash.\nMISLEADS ownership transfer is invisible: a helper that returns the mapping\n     to its caller is correct and is listed. Short-lived processes that let\n     exit() clean up are listed too -- which is why liburing's tests rank\n     high here.\n", qQ105},
	{"wait-status-unchecked", "waitpid's status used without WIFEXITED/WIFSIGNALED", "ANSWERS where a signal death is reported as an exit code.\nACT the status word must be decoded first. WIFEXITED gates WEXITSTATUS;\n     WIFSIGNALED gates WTERMSIG. Reading it raw conflates the two.\nMISLEADS function-wide guard test; the macros may appear in a helper.\n", qQ106},
	{"scanf-unchecked", "scanf-family conversions never counted", "ANSWERS where malformed input silently yields uninitialised variables.\nACT scanf returns the number of successful conversions. Check it equals the\n     number of specifiers. Prefer fgets + strtol, which is checkable.\nMISLEADS function-wide guard test.\n", qQ107},
	{"math-domain-error", "sqrt/log/asin with no domain guard", "ANSWERS where a NaN is born and then propagates silently.\nACT sqrt of a negative, log of zero, asin outside [-1,1] are domain errors:\n     they return NaN and set errno. Clamp the argument or check the result\n     with isnan().\nMISLEADS the guard is any comparison or `if` in the function, which is a weak\n     proxy -- it cannot tell a clamp on THIS argument from an unrelated\n     branch. Only 7 rows across the corpora; gcc's -Wfloat-equal and clang's\n     -Wtautological-compare-ish checks do not cover this.\n", qQ108},
	{"time-not-threadsafe", "localtime/gmtime/ctime/asctime -- one static buffer", "ANSWERS where two threads can read each other's formatted time.\nACT these return a pointer to a single static object. Use the _r variants\n     (localtime_r, gmtime_r) or ctime_r, which fill a caller buffer.\nMISLEADS single-threaded programs are entirely unaffected and are listed.\n     The `conc_in_fn` column is the only evidence offered: whether the same\n     function touches a concurrency primitive at all.\n", qQ109},
	{"signal-deprecated-api", "signal() where sigaction() is available", "ANSWERS where handler semantics depend on the platform.\nACT signal() has implementation-defined behaviour on delivery -- SysV resets\n     the handler, BSD does not. sigaction() makes it explicit and is\n     portable.\nMISLEADS signal() is not wrong everywhere, only where the reset semantics\n     matter. A program that reinstalls its own handler is fine and listed.\n", qQ110},
	{"mman-result-unchecked", "mmap compared to NULL instead of MAP_FAILED", "ANSWERS a check that passes on failure.\nACT mmap fails with MAP_FAILED, which is (void *)-1 on most systems and NOT\n     NULL. A NULL check succeeds, and the caller then dereferences -1.\nMISLEADS the guard test looks for MAP_FAILED or a NULL check; a codebase\n     that correctly checks MAP_FAILED only via a macro is missed. Only 2\n     rows across the corpora -- rare, and worth finding precisely because\n     it is rare.\n", qQ111},
	{"socket-not-nonblocking", "accept() with no O_NONBLOCK anywhere", "ANSWERS where one slow peer can stall the loop that accepted it.\nACT an accepted socket starts blocking. Set O_NONBLOCK (or accept with\n     SOCK_NONBLOCK) before handing it to an event loop, or a read can block\n     every other descriptor.\nMISLEADS the guard is O_NONBLOCK/SOCK_NONBLOCK/F_SETFL anywhere in the\n     function, so a non-blocking set performed in a helper is missed and the\n     accepting function is listed.\n", qQ112},
	{"varargs-lifetime", "va_start with no va_end in the same function", "ANSWERS where a va_list is left in an undefined state.\nACT every va_start needs a matching va_end on every path -- including the\n     early returns. On some ABIs the frame is not cleaned up without it.\nMISLEADS only 2 rows across the corpora; this is a rare bug, kept because it\n     is cheap and unambiguous. A va_end reached through a macro is missed.\n", qQ113},
	{"signal-stack-size-constant", "Array sized with SIGSTKSZ / MINSIGSTKSZ / PTHREAD_STACK_MIN", "ANSWERS a mis-sized buffer that appeared when glibc changed underneath it.\nACT glibc 2.34 made these non-constant -- they are sysconf() calls now.\n     Query them at runtime (sysconf(_SC_SIGSTKSZ)) and allocate, or use a\n     constant of your own. A compile-time array gets a size that is no\n     longer tied to what the kernel wants.\nMISLEADS fires on NO corpus here. Only a problem on glibc >= 2.34; on musl\n     and older glibc these are still constants and the code is fine.\n", qQ114},
	{"pointer-overflow-check-broken", "`p + n < p` -- an overflow check the compiler deletes", "ANSWERS a check that looks defensive and is not one.\nACT pointer addition that overflows is undefined behaviour, so the compiler\n     folds `p + n < p` to false. Compare on integers:\n     `(uintptr_t)p + n < (uintptr_t)p`. Detect with\n     -fsanitize=pointer-overflow, which is opt-in -- hence this rule.\nMISLEADS the pattern is matched textually, so a genuine comparison of two\n     unrelated pointers that happen to share a name is listed. Fires on no\n     corpus here, and is kept precisely because the compiler silently\n     removes the check rather than warning about it.\n", qQ115},
	{"calloc-transposed-args", "calloc's arguments are the wrong way round", "ANSWERS an allocation that is the wrong size and looks deliberate.\nACT calloc(count, size). A sizeof in the FIRST argument means the two are\n     transposed: you asked for size*count elements of size bytes. GCC 14\n     catches this with -Wcalloc-transposed-args, which is opt-in.\nMISLEADS only the `calloc(sizeof(...)` spelling is detected; a named\n     constant first argument that is really a size is invisible. Harmless\n     when size == 1, which is common.\n", qQ116},
	{"varargs-array-type", "va_arg with an array type: undefined behaviour", "ANSWERS a construct that cannot work at all.\nACT va_arg's type must be a promoted type the ABI can pass. An array type\n     undergoes no promotion, so it never matches. Pass a pointer instead.\n     Clang 20 diagnoses this as -Wvarargs; GCC does not by default.\nMISLEADS fires on NO corpus in this tree -- it exists because the construct\n     is unambiguous UB and the compiler only catches it with an opt-in flag.\n", qQ117},
	{"time-elapsed-not-monotonic", "A wall clock read where an elapsed time may be measured", "ANSWERS where a clock adjustment, an NTP step or a DST change moves a timing.\nACT gettimeofday() and time() read the WALL clock, which steps backwards and\n     forwards. For elapsed time use clock_gettime(CLOCK_MONOTONIC), which\n     cannot go backwards. Keep the wall clock only for timestamps.\nMISLEADS this cannot tell measuring from timestamping -- and most wall-clock\n     reads are legitimate timestamps that are correctly excluded by the\n     `clock_gettime` test. A function that reads the clock to print a log\n     line is listed. The filter is any gettimeofday/time/ftime/clock call\n     with no CLOCK_MONOTONIC anywhere in the function, so the volume is high\n     (143 rows across the corpora) by design: triage by fan_in.\n", qQ118},
	{"event-wait-batch-one", "An event wait that can return exactly one event per syscall", "ANSWERS where the loop is syscall-bound instead of event-bound.\nACT pass a real array size. epoll_wait's maxevents of 1 -- or kevent's nevents\n     of 1 -- means one syscall per event, which throws away the entire reason\n     for using a poller instead of poll().\nMISLEADS a maxevents of 1 is sometimes deliberate: nginx uses it in a probe\n     that exists precisely to test one descriptor. Detected from the argument\n     list only, so a variable that happens to hold 1 is invisible.\n", qQ119},
}

var metricsCat = []qentry{
	{"graph-blindspots", "Read this first: where the call graph cannot see", "ANSWERS how much of every other answer below is a lower bound.\nACT fnptr is the real blindness -- a callback table, a vtable-by-hand, a\n     dispatch array. Reachability results for a module high on this list\n     are floors, not facts. `via_macro` and `external` are NOT blindness:\n     one is the preprocessor, the other is libc, and both are known.\nMISLEADS this matches on NAME and body size only -- it never checks that\n     the definitions sit behind a preprocessor conditional. Two\n     same-named static functions in unrelated translation units, or one\n     entry point implemented once per example module, look identical to\n     a real backend pair. Confirm with grep -n '#if' on both files.\n     fnptr also counts ordinary struct-member calls, and `unresolved`\n     includes names from headers of libraries this run never walked into.", mM1},
	{"hot-multipliers", "Where one fix multiplies: highest fan-in, ranked with complexity", "ANSWERS which functions the rest of the tree leans on hardest.\nACT a win in a high-fan-in leaf pays back once per caller.\nMISLEADS fan_in counts STATIC call sites, not dynamic frequency, and it\n     cannot see a caller that reaches this through a function pointer.", mM2},
	{"risk-ranked", "Security review order: complexity x hazard x recursion", "ANSWERS if you can only review N functions this week, which N.\nACT risk = 2*cyclo + cognitive + 5*nest + 10*memory + 8*io + 15*exec\n     + integer + 2*alloc + 3*concurrency + 25 if recursive\n     + 10 if it allocates and never frees.\nMISLEADS it is a heuristic for ORDERING, not a list of findings.", mM3},
	{"alloc-cost", "Allocations per call, TRANSITIVELY", "ANSWERS what one call to this function really costs the allocator.\nACT `direct` counts allocation written in the body; `xalloc` walks the\n     call graph and multiplies by static call sites.\nMISLEADS the multiplier is STATIC call sites, not trip count, and the\n     walk stops at depth 3 -- deeper allocation is not counted at all.\n     Cycles are double-counted (a self-recursive wrapper's own mass is\n     pushed back into it), so treat xalloc as an upper bound.", mM4},
	{"module-coupling", "Cross-module call edges: where a seam would actually cut", "ANSWERS how entangled the subsystems are.\nACT a heavy one-way edge is a real seam; a heavy pair is a cycle and the\n     two modules are one module that has not admitted it.\nMISLEADS counts DISTINCT caller/callee pairs, not runtime frequency, and\n     misses every edge that goes through a callback.", mM5},
	{"header-fanout", "Headers whose change rebuilds the most of the tree", "ANSWERS which header is the build's bottleneck, transitively.\nACT split the widely-included header, or move the hot declarations into a\n     narrow one. This is the cheapest build-time win in a C repo.\nMISLEADS include depth is not compile cost, and a header included by\n     everything but changed once a year costs nothing at all.", mM6},
	{"nested-loops", "Loop depth >= 2: the O(n^k) candidates, with their per-iteration cost", "ANSWERS where cost grows super-linearly in the input.\nACT check what bounds the INNER trip count. `divs` and `calls` are the\n     two per-iteration costs people forget; a divide is ~20-40 cycles and\n     a loop-invariant one should be a reciprocal multiply.\nMISLEADS depth counts LEXICAL nesting, not asymptotics: an inner loop\n     bounded by a constant is O(1).", mM7},
	{"vectorisation-blocked", "Loops that CANNOT vectorise: a libm call in the body", "ANSWERS which loops are categorically unvectorizable as written.\nACT a libm call in a loop body is a hard stop for the auto-vectoriser --\n     it cannot prove the call is pure or replace it with a vector form.\n     Use a vectorised math library, or a polynomial approximation.\nMISLEADS it does not know the loop is hot, and -ffast-math plus a vector\n     libm changes the answer entirely.", mM8},
	{"explicit-simd", "Hand-written intrinsics and branch hints", "ANSWERS where the code already commits to a specific ISA.\nACT every intrinsic site needs a scalar fallback that is BUILT in CI, not\n     merely present -- an unbuilt fallback is an unrun fallback.\nMISLEADS a high intrinsic count is not a fast function, and `likely` hints\n     are frequently wrong and never re-measured.", mM9},
	{"struct-padding", "Byte-accurate layout: what reordering the fields would save", "ANSWERS which structs waste memory on padding and exactly what the fix\n     buys: `packed_sz` is the size with members declared widest-alignment\n     first, `saves` the bytes back PER INSTANCE, and lines64_packed whether\n     it then spans fewer cache lines.\nACT reorder to get packed_sz. No algorithm changes, and it compounds across\n     every instance in every array. A row where lines64 drops is also one\n     fewer cache miss per access.\nMISLEADS LP64 model, exact=1 rows only: a struct holding a type this file\n     does not define cannot be sized and is absent. A struct whose layout is\n     an ABI -- written to disk, sent on the wire, shared with assembly --\n     cannot be reordered at all, and one grouped for cache locality (hot\n     fields first) may be right as it is: packed_sz is the floor, not a\n     recommendation to ignore both.", mM10},
	{"cache-line-crossers", "Structs just over a 64-byte cache line", "ANSWERS which hot objects need two cache lines where one would do.\nACT 65-128 bytes is the painful band: shrink below 64 and every access\n     halves its memory traffic. Removing the padding is often enough.\nMISLEADS a struct that is never hot does not care, and an object always\n     touched in full needs both lines anyway.", mM11},
	{"cache-hostile-layout", "Pointer-dense structs: each pointer field defeats the prefetcher", "ANSWERS which structures drag a whole cache line to reach one field, and\n     then send you somewhere else in memory to read it.\nACT candidates for splitting the hot fields into a parallel array (SoA).\nMISLEADS TOP-LEVEL fields only -- union arms are alternatives, not extra\n     fields -- and a node in a linked structure is pointer-dense by nature.", mM12},
	{"stack-pressure", "Stack bytes per function, and down the deepest call chain under it", "ANSWERS which frames are large, and which entry points sit on top of the\n     most stack: `chain_bytes` is this function's frame plus the heaviest\n     acyclic path of callees below it, `chain_depth` how many frames that is.\nACT compare chain_bytes for every thread entry point against that thread's\n     stack size. `biggest_local` names the usual culprit -- one buffer that\n     belongs on the heap or in a static.\nMISLEADS frame_bytes sums DECLARATIONS: sibling scopes share slots, so it\n     overstates; `unsized` locals (a type this file does not define, a VLA)\n     and saved registers are left out, so it understates. chain_bytes follows\n     only calls the graph resolved -- nothing through a pointer -- and where\n     `cyclic` is 1 a call back into the path was cut, so the true figure is\n     unbounded and this one is a floor.", mM13},
	{"cast-density", "Pointer casts: where the type system was overruled", "ANSWERS the places a wrong assumption becomes a memory bug.\nACT each cast is a claim the compiler cannot check. A cast next to I/O\n     and shifting is where attacker-controlled bytes become a pointer.\nMISLEADS the pattern counts some compound literals and some macro\n     parameter lists as casts.", mM14},
	{"macro-machinery", "Function-like macros, by how much work they do", "ANSWERS the code the call graph cannot see into at all.\nACT a heavily-used multi-line macro is a function that skipped review: no\n     type checking, no breakpoint, no stack frame, and its cost is\n     multiplied by every use site rather than shared.\nMISLEADS `uses` counts sites where the name was called and no function of\n     that name exists in the tree, so a macro shadowed by a real function\n     somewhere reads as unused.", mM15},
	{"config-gated", "Code behind a CONFIG_/HAVE_/USE_ flag", "ANSWERS which regions a plain build silently omits.\nACT build each flag in CI, or the code inside it is unrun and unreviewed\n     and will not compile the day someone needs it.\nMISLEADS lists DIRECTIVES, not region sizes: one #ifdef can gate a\n     thousand lines or a single semicolon.", mM16},
	{"backend-parity", "One name, two definitions: which #if-selected backend is the STUB", "ANSWERS where a compile-time alternative silently drops a feature.\nACT a body a fraction of its sibling's size is usually the stub, and the\n     platform that selects it is the platform the feature does not work on.\nMISLEADS a small body can be complete -- a one-line platform wrapper is\n     the whole implementation on that platform.", mM17},
	{"profiler-invisible", "static inline with real fan-in: zero self-time is not zero cost", "ANSWERS which functions a sampling profiler CANNOT attribute cost to.\nACT never conclude one of these is cold from a flat profile -- its time\n     is charged to whoever inlined it. `hidden` is fan_in * sloc, a rough\n     measure of how much code the inliner is duplicating.\nMISLEADS `static inline` is a request, not a guarantee, and a big one is\n     often refused.", mM18},
	{"undocumented-complexity", "Complex functions with almost no comments", "ANSWERS where the next reader has to reconstruct intent from the code,\n     and where that reconstruction costs the most.\nACT a useful comment carries a CONSTRAINT or a non-obvious fact -- why\n     the bound is 4096, which caller guarantees the pointer is non-NULL.\nMISLEADS comment COUNT is not comment quality, and a genuinely obvious\n     500-line switch needs no prose at all.", mM19},
	{"hand-linked-objects", "Build rules and object lists that enumerate their inputs by hand", "ANSWERS which link targets name their objects individually -- both in a\n     rule's prerequisites and in the OBJ= variable that feeds it, which is\n     where a C project usually keeps the list.\nACT these are the lists a NEW CALL from a shared source silently breaks:\n     the compile succeeds and the link fails naming a symbol nobody edited.\nMISLEADS a hand-written list is perfectly correct until a dependency\n     changes, so this is a fragility ranking, not a defect list. A probe\n     snippet in a configure check looks the same and matters not at all.", mM20},
	{"parse-coverage", "How much of the tree this run actually read", "ANSWERS whether any answer above is missing a chunk of the codebase.\nACT `errors` means braces or parentheses did not balance, so functions in\n     that file may be merged, truncated or missed entirely. `unclosed` is\n     an #if without an #endif. Both are worth reading before trusting a\n     ranking that says a module is small.\nMISLEADS a file with zero errors can still be mis-read: a macro that\n     opens a brace and another that closes it balances perfectly and\n     produces nonsense spans.", mM21},
	{"goto-spaghetti", "Functions with excessive goto usage (MISRA-C 15.1)", "ANSWERS where a function has more than 3 goto statements, making control\n     flow hard to follow and verify.\nACT restructure with structured control flow (if/else, while, break).\nMISLEADS goto for cleanup-on-error (the only acceptable use in Linux kernel\n     style) is correct but should be the only pattern.", mM22},
	{"deep-nesting", "Functions with excessive nesting depth (MISRA-C)", "ANSWERS where a function has max_nesting > 4, making it hard to verify.\nACT extract nested blocks into helper functions; use early returns.\nMISLEADS C's max_nesting is +1 vs tree-sitter languages because it counts\n     the function's own brace. Adjust thresholds accordingly.", mM23},
	{"too-many-params", "Functions with too many parameters (MISRA-C)", "ANSWERS where a function has more than 6 parameters.\nACT use a struct parameter.\nMISLEADS a variadic function has n_params that does not count the ellipsis.", mM24},
	{"scattered-concerns", "A function called from many different modules (shotgun surgery)", "ANSWERS which functions are called from many distinct modules.\nACT consider splitting or stabilizing the contract.\nMISLEADS a utility like memcpy or printf is called from everywhere.", mM25},
	{"magic-number", "Functions with many bare numeric literals (MISRA-C 7.4)", "ANSWERS where a function has many magic numbers — bare numeric literals\n     that are not 0 or 1 and have no named constant. Each is a\n     maintenance burden.\nACT extract magic numbers into named constants or enums.\nMISLEADS 0 and 1 are excluded by convention. Array indices and bit flags\n     are sometimes clearer as literals.", mM26},
	{"call-chain-depth", "Deepest resolved call chains: how far a change propagates downward", "ANSWERS the functions with the widest downward exposure -- the count\n     of DISTINCT functions each one can reach through resolved edges\n     at any depth (precomputed in `reach.n_transitive_out`). Where\n     blast-radius counts who can reach YOU (upward risk), this counts\n     what you can REACH: the failure modes, allocation behaviour and\n     locking you inherit every time you call down the stack.\nACT a wide fan under a request handler is where latency and error\n     handling get lost; flatten, or make each level's contract\n     explicit.\nMISLEADS breadth, not depth: a function calling 30 helpers outranks a\n     strict 12-level chain of 1-callee hops; calls through function\n     pointers end the walk and undercount; a cycle member inherits its\n     component's full reach.", mM27},
	{"global-state-map", "Which modules own mutable file-scope state, and who writes it", "ANSWERS the mutation map `race-surface` cannot draw: per module, the\n    count of mutable file-scope objects against the number of\n    functions that WRITE one of them in place. A module high on both\n    columns is stateful by design and is where 'just add a feature'\n    turns into 'understand six globals first'.\nACT the fix is architectural: group the globals into one context\n    struct passed explicitly, so ownership shows up in signatures.\nMISLEADS same-file references only (extern-through-header use is\n    invisible), and a write counted here can be an initializer run\n    once at startup -- volume of writes is not volume of races.", mM28},
	{"error-handling-density", "Where failure handling dominates the code", "ANSWERS the functions whose shape IS error handling: return paths vs\n    distinct failure shapes (NULL / negative / zero), null checks, and\n    gotos-to-cleanup. C has no stack unwinding, so this work is real\n    code with real branch cost -- and its absence is a defect while\n    its excess is unreadability. This metric measures the balance.\nACT a high ratio with FEW null checks is the dangerous corner:\n    failure-shaped but not checking allocations. Start there.\nMISLEADS ret_* counts are textual (`return NULL;` exactly); a returned\n    error ENUM or errno expression counts as ret_val, so a\n    consistently-error-coded module looks value-returning; goto here\n    includes non-cleanup uses.", mM29},
	{"alloc-site-inventory", "Every allocation site, grouped by size-expression shape", "ANSWERS the tree's actual allocation habits: raw byte counts vs\n    sizeof-multiplied element counts vs bare constants, per allocator.\n    The sizeof share is the single best proxy for allocation-safety\n    discipline, and it is comparable across projects.\nACT read the shape mix before trusting any single leak query: a tree\n    that allocates mostly through wrappers needs wrapper-level review,\n    not site-level.\nMISLEADS project allocator wrappers are invisible unless they call\n    malloc/calloc/realloc directly IN THE BODY (they usually do, which\n    is why the fn column matters); alloca rows are stack allocations\n    and do not leak -- they exhaust the stack instead.", mM30},
	{"copy-hotspots", "The heaviest memcpy-family users, with their copy-site inventory", "ANSWERS which functions move the most bytes-by-call-count: the\n    memcpy/memmove/strcpy/strcat/sprintf load ranked per function.\n    Copy count is the cheapest available proxy for memory-bandwidth\n    pressure, and these functions are also where a wrong length\n    argument does the most damage.\nACT for the top rows ask the only two questions that matter: could\n    this copy be a pointer swap, and is the length derived from input?\nMISLEADS call COUNT, not bytes moved -- one memcpy of 4 KB outranks\n    nothing here if the rest are 8-byte struct copies; copies inside\n    loops multiply beyond what the count shows (join nested-loops for\n    that).", mM31},
	{"macro-reach", "Macro definitions ranked by transitive use weight", "ANSWERS the preprocessor's real API surface: function-like macros\n    weighted by use sites AND by body size -- n_uses * body_len is the\n    amount of source the reader must simulate to understand one macro\n    call. macro-machinery ranks by uses alone; this adds the cost side.\nACT the top rows are conversion candidates: a static inline function\n    gives type checking and debuggability at identical runtime cost.\nMISLEADS n_uses counts CALL-SHAPED uses only (name followed by '(');\n    object-like macros used as constants show zero uses here, and a\n    macro shadowed by a same-named function anywhere in the tree\n    loses all its uses to that function.", mM32},
	{"struct-abi-surface", "Structs most exposed to layout change, weighted by field volatility", "ANSWERS the structs whose byte layout carries the most cross-file\n    weight: exact-sized, pointer-bearing, and defined in headers other\n    files include. Reordering ONE field of these changes offsets in\n    every consumer -- the ABI-compat question struct-padding answers\n    for waste, answered here for RISK.\nACT pin the hot ones with _Static_assert(sizeof(struct x) == N) so a\n    layout change fails the build instead of the customer.\nMISLEADS header-defined structs only (a .c-local struct cannot leak\n    its layout); usage is inferred from include edges, so a widely\n    included header whose struct nobody instantiates still tops the\n    list; exact=1 rows only.", mM33},
	{"marker-debt", "TODO/FIXME/XXX/HACK/BUG/UNSAFE comments, by file", "ANSWERS where the code says out loud that it is not finished.\nACT read the SAFETY and UNSAFE markers first: those are usually a real\n     invariant stated in prose because C cannot state it in types.\nMISLEADS a marker is a comment, not a defect, and NOTE dominates every real\n     codebase. The `markers` table has been populated since the base schema and\n     nothing read it until now, so no convention about it has been established.\n", mM34},
	{"clone-candidates", "Functions with an identical size-and-shape fingerprint", "ANSWERS where the same shape of code appears more than once.\nACT a fingerprint match is a place to LOOK, not proof of a copy. Read two rows\n     before extracting anything.\nMISLEADS the fingerprint is (sloc, cyclomatic, n_params, n_calls). Two entirely\n     different functions can share it, and a genuine copy that gained one\n     statement will not. `duplicated_sloc` is an upper bound on what could be\n     removed and is usually far larger than what should be.\n", mM35},
	{"aliasing-blocked", "Same-typed pointer parameters with no restrict anywhere", "ANSWERS where the compiler must assume two parameters overlap.\nACT add `restrict` where they genuinely cannot overlap and the reloads go away.\n     Verify first: CERT EXP43-C -- an incorrect restrict is undefined behaviour.\nMISLEADS \"same type\" is textual on the parameter type string, so two pointers\n     to different structs that happen to share a spelling get paired, while a\n     `void *` next to anything does not. Loop depth is the proxy for whether it\n     matters: without a loop there is nothing to reload.\n", mM36},
	{"file-hotspots", "Files ranked by total risk", "ANSWERS where to start when the answer has to be a file, not a function.\nACT `risk_per_100_sloc` normalises for size: a big file is not automatically a\n     bad one, but a small file with a high density is.\nMISLEADS risk is the C-weighted formula (memory, I/O, exec, recursion), so a\n     file full of careful string handling ranks high without being unsafe.\n", mM37},
	{"api-surface", "The tree's real public interface, ranked by dependents", "ANSWERS which functions are actually an API: defined in a .c, prototyped in a .h.\nACT these are the ones a signature change breaks across translation units, and\n     the ones that need a written contract. `documented=0` on a high fan_in row\n     is the shortest path to a better codebase.\nMISLEADS `declared_in` counts prototype rows anywhere in the tree, so a name\n     declared in several headers counts all of them. Inline functions in headers\n     are excluded by construction, which is where much of a modern C API lives.\n", mM38},
	{"macro-hygiene", "Function-like macros ranked by how much work they do", "ANSWERS the macros whose expansion is doing real work in this tree.\nACT a macro used a thousand times is a function that cannot be stepped into,\n     type-checked, or profiled. `do_while=0` on a multi-statement macro is the\n     one to fix first.\nMISLEADS `n_uses` counts calls resolved to this macro by NAME, so a macro\n     invoked from inside another macro is attributed to the outer one. do_while\n     is a substring test on the body, not a parse.\n", mM39},
	{"init-discipline", "Locals declared without an initialiser, by function", "ANSWERS the surface for EXP33-C: an uninitialised read.\nACT a local declared without an init is only a bug if it is READ before it is\n     written, which this cannot prove. Ranked by count so the largest surfaces\n     come first.\nMISLEADS the dominant false positive is a variable assigned on all paths\n     immediately after its declaration -- extremely common and entirely correct.\n", mM40},
	{"lock-scope", "Each lock, by how much code runs while it is held", "ANSWERS which lock is the actual contention point.\nACT a lock's cost is not how often it is acquired, it is how much work sits\n     underneath: cyclo_under_lock and io_under_lock are the columns that predict\n     stalls. Splitting a hot lock beats optimising what is under it.\nMISLEADS attributed per FUNCTION that acquires the lock, not per critical\n     section -- so everything in that function counts as \"under the lock\",\n     including code that runs before the acquire. Only lock-shaped identifier\n     names are recorded, so a lock held in a variable is invisible.\n", mM41},
	{"typedef-pointer-opacity", "Typedefs whose underlying type is a pointer or an array", "ANSWERS where the `*` is invisible at every use site.\nACT the reader cannot tell whether `Foo x` is one word or a thousand, whether\n     `x = y` aliases or copies, or whether `sizeof x` is the pointer or the\n     buffer. Naming it `FooPtr` is the cheap fix.\nMISLEADS matched on the presence of `*` or `[` in the declared type text, so a\n     typedef of a function pointer and a typedef of a struct that CONTAINS a\n     pointer are both listed; only the first is the readability problem.\n", mM42},
	{"static-inline-in-header", "static inline bodies living in a header", "ANSWERS code that is compiled once per including translation unit.\nACT `times_included` is the multiplier. These functions never appear in a\n     profile under their own name and their cost is paid once per includer.\nMISLEADS a header with none is absent from this list, not clean -- this is a\n     presence ranking. `total_fan_in` sums the fan_in of the inlines, so a\n     header with one heavily-used inline outranks one with thirty unused ones,\n     which is the right order for build time and the wrong one for review.\n", mM43},
	{"error-code-convention", "Modules that cannot agree how to report failure", "ANSWERS where a caller has to know the callee before it can check the result.\nACT pick one convention per module and write it down. `pct_mixed` is the share\n     of that module's functions that use more than one shape.\nMISLEADS computed over functions with at least one return, and the three shapes\n     are NULL, a negative literal and a zero literal. Returning 0 for success AND\n     0 as a legitimate value is the worst case of all and is invisible. Modules\n     with fewer than 8 functions are excluded.\n", mM44},
	{"extern-in-source", "extern declarations sitting in a .c instead of a header", "ANSWERS where a symbol's declaration is repeated per translation unit.\nACT move it to the header that owns the module, so the compiler can check every\n     declaration against every other one.\nMISLEADS `also_in_a_header` counts how many of this file's extern names ALSO\n     appear in some header anywhere -- not necessarily one this file includes. A\n     high number usually means the declaration is simply duplicated, which is\n     still the problem.\n", mM45},
	{"event-api-inventory", "Every function that touches epoll, io_uring or kqueue, with its flag profile", "ANSWERS which functions own the event loop, and what contract each one has\n     taken on -- edge or level, oneshot or not, errors watched or not.\nACT read this before any of the event-loop queries: it is the same columns\n     unfiltered, so a row here that no query flags is a loop that is probably\n     correct, and a row that several queries flag is where to start.\nMISLEADS all counts are per FUNCTION. A project with one central dispatcher\n     shows one row and looks simple; a project that inlines its loop shows\n     dozens and does not necessarily have more problems.\n", mM46},
	{"event-loop-hotspots", "The blocking wait loops, ranked by complexity", "ANSWERS which event loop is the one worth understanding.\nACT a loop's cost is not the wait, it is everything under it. The per-loop\n     columns -- calls, I/O and branches inside the loop body -- say whether\n     the loop is a dispatcher or accidentally a worker.\nMISLEADS 'loop' here means the function that calls the wait primitive, and\n     the loop-body metrics cover every loop in that function, not only the\n     event loop. Cyclomatic complexity ranks big dispatch switches highly,\n     which is often the correct shape for one.\n", mM47},
	{"event-api-coverage", "Per module: which of the three APIs, and how much of each contract is met", "ANSWERS where the event-loop code lives and how disciplined it is.\nACT the six right-hand columns are the same predicates the event-loop\n     queries use, counted instead of listed: eintr_blind, et_unprepared and\n     leaks_instance are the rows those queries would return, so this is the\n     denominator they lack.\nMISLEADS every count is per FUNCTION, so a project with one central\n     dispatcher shows a single dense row while a project that inlines its\n     loop shows many sparse ones with the same total defects. Modules are\n     two path components, so cross-directory loops are split.\n", mM48},
	{"api-namespace-inventory", "Which toolchain namespaces each file uses, and how heavily", "ANSWERS where the API surface of this codebase actually is.\nACT read this before any other rule in the pack: it is the denominator the\n     per-rule queries lack. A file leaning hard on string.h deserves the\n     string rules first.\nMISLEADS grouped per FILE, so a project with one central wrapper file shows\n     a single dense row while a project that calls libc everywhere shows many\n     sparse ones with the same total. Namespaces are assigned by first match\n     in sorted order, so a function declared by two headers is counted once.\n", mM49},
	{"api-discipline-score", "Per file: how much of the API surface is used without its guard", "ANSWERS which files are careless, not which individual calls are wrong.\nACT the five right-hand columns are the same predicates the per-rule queries\n     use, counted instead of listed. Rank by the column that matters for the\n     file's namespace mix.\nMISLEADS every count is per FUNCTION and every guard test is function-wide,\n     so a file with one careful helper and twenty careless callers scores\n     badly for the right reason and a file with one careful helper and no\n     callers scores perfectly for the wrong one. This is a triage ranking,\n     not a defect count.\n", mM50},
}

func report(g *Graph) {
	w := bufio.NewWriter(os.Stdout)
	defer w.Flush()
	var bar strings.Builder
	for range 78 {
		bar.WriteString("=")
	}
	var dash strings.Builder
	for range 78 {
		dash.WriteString("-")
	}
	fmt.Fprintf(w, "\n%s\nOVERVIEW\n%s\n", bar.String(), dash.String())
	for _, k := range []string{"lang", "target", "parser", "root"} {
		if v, ok := g.Meta[k]; ok && v != "" {
			fmt.Fprintf(w, " %-14s %s\n", k, v)
		}
	}
	files := int32(len(g.Files))
	parsed, sloc := int32(0), int32(0)
	for i := range g.Files {
		if g.Files[i].Parsed == 1 {
			parsed++
			sloc += g.Files[i].Sloc
		}
	}
	fmt.Fprintf(w, " %-14s %d catalogued, %d parsed, %d sloc\n", "files",
		files, parsed, sloc)
	kindN := map[string]int{}
	for i := range g.Symbols {
		kindN[g.Symbols[i].Kind()]++
	}
	type kv struct {
		k string
		v int
	}
	var ks []kv
	for k, v := range kindN {
		ks = append(ks, kv{k, v})
	}
	sort.Slice(ks, func(i, j int) bool {
		if ks[i].v != ks[j].v {
			return ks[i].v > ks[j].v
		}
		return ks[i].k < ks[j].k
	})
	if len(ks) > 12 {
		ks = ks[:12]
	}
	parts := make([]string, 0, len(ks))
	for _, x := range ks {
		parts = append(parts, x.k+"="+strconv.Itoa(x.v))
	}
	fmt.Fprintf(w, " %-14s %s\n", "symbols", joinComma(parts))
	var unres int32
	for _, u := range g.Unres {
		unres += u.N
	}
	fmt.Fprintf(w, " %-14s %d edges, %d call sites, %d unresolved\n",
		"call graph", len(g.Edges), len(g.Callsites), unres)

	fmt.Fprintf(w, "\n%s\nHOW MUCH OF THIS TO TRUST\n%s\n", bar.String(), dash.String())

	errFiles, errTotal := int32(0), int32(0)
	for i := range g.Files {
		if g.Files[i].NParseErrors > 0 {
			errFiles++
			errTotal += g.Files[i].NParseErrors
		}
	}
	var direct, viaMacro, external, viaPtr int64
	for i := range g.Symbols {
		s := g.Symbols[i]
		r := g.rv(s)
		viaMacro += int64(r.NMacroCalls)
		external += int64(r.NExternalCalls)
		viaPtr += int64(r.NFnptrCalls)
	}
	for _, ed := range g.Edges {
		direct += int64(ed.NCalls)
	}
	named := direct + viaMacro + external + int64(unres)
	pctOf := func(n, d int64) string {
		if d == 0 {
			return "-"
		}
		return strconv.FormatInt((n*1000/d+5)/10, 10) + "%"
	}
	if parsed == 0 {
		fmt.Fprintln(w, " NOTHING WAS PARSED. Every number below is zero because no file")
		fmt.Fprintln(w, " was read, not because this repository is empty or clean.")
	}
	fmt.Fprintf(w, " %-30s %d of %d parsed file(s), %d diagnostic(s)\n",
		"files with parse diagnostics", errFiles, parsed, errTotal)
	if errFiles > 0 {
		fmt.Fprintln(w, "   functions near a diagnostic may be missing or merged: --parse-errors")
	}
	if named+viaPtr == 0 {
		fmt.Fprintf(w, " %-30s none recorded -- this is the absence of data, not a clean result\n",
			"calls")
	} else {
		fmt.Fprintf(w, " %-30s %d\n", "call sites that name a callee", named)
		fmt.Fprintf(w, "   %-28s %-8d %s\n", "followed to a definition", direct, pctOf(direct, named))
		fmt.Fprintf(w, "   %-28s %-8d %s  (libc and other code outside this tree)\n",
			"external by design", external, pctOf(external, named))
		fmt.Fprintf(w, "   %-28s %-8d %s  (a macro the preprocessor left unexpanded)\n",
			"macro, not a function", viaMacro, pctOf(viaMacro, named))
		fmt.Fprintf(w, "   %-28s %-8d %s  <- the blind share\n",
			"NOT followed", unres, pctOf(int64(unres), named))
		fmt.Fprintf(w, " %-30s %d  (no callee name at all; never in the call graph)\n",
			"calls through a pointer", viaPtr)
		fmt.Fprintln(w, " Fan-in, reachability and dead-code are computed from the first line only.")
		fmt.Fprintln(w, " `--metrics graph-blindspots` breaks the blind share down per module, and")
		fmt.Fprintln(w, " the `calls_resolved` row of the meta table says how much of it is names")
		fmt.Fprintln(w, " defined more than once, where neither linkage nor a shared prototype")
		fmt.Fprintln(w, " picks the target.")
	}

	fmt.Fprintf(w, "\n%s\nBIGGEST MODULES\n%s\n", bar.String(), dash.String())
	mods := make([]Module, 0, len(g.Modules))
	for _, m := range g.Modules {
		if m.NFiles > 0 {
			mods = append(mods, m)
		}
	}
	sort.Slice(mods, func(i, j int) bool {
		if mods[i].Sloc != mods[j].Sloc {
			return mods[i].Sloc > mods[j].Sloc
		}
		return mods[i].Name() < mods[j].Name()
	})
	if len(mods) > 12 {
		mods = mods[:12]
	}
	var mr [][]fval
	for _, m := range mods {
		mr = append(mr, []fval{fStr(m.Name()), fI32(m.NFiles), fI32(m.Sloc),
			fI32(m.NSymbols), fF(round2(m.Instability))})
	}
	render(w, mr, []string{"name", "files", "sloc", "syms", "instab"})

	fmt.Fprintf(w, "\n%s\nHEAVIEST FUNCTIONS\n%s\n", bar.String(), dash.String())
	render(w, heaviest(g), []string{"name", "sloc", "cyclo", "cog", "nest",
		"fan_in", "at"})

	fmt.Fprintf(w, "\n%s\nMOST DEPENDED ON\n%s\n", bar.String(), dash.String())
	render(w, mostDepended(g), []string{"name", "fan_in", "fan_out", "cyclo",
		"sloc", "at"})

	fmt.Fprintf(w, "\n%s\nMARKERS LEFT IN THE CODE\n%s\n", bar.String(), dash.String())
	mk := map[string]int{}
	for _, m := range g.Markers {
		mk[m.Kind()]++
	}
	var mks []kv
	for k, v := range mk {
		mks = append(mks, kv{k, v})
	}
	sort.Slice(mks, func(i, j int) bool {
		if mks[i].v != mks[j].v {
			return mks[i].v > mks[j].v
		}
		return mks[i].k < mks[j].k
	})
	var mkr [][]fval
	for _, x := range mks {
		mkr = append(mkr, []fval{fStr(x.k), fInt(x.v)})
	}
	render(w, mkr, []string{"kind", "n"})
}

func round2(f float64) float64 {
	return float64(int64(f*100+copysign(0.5, f))) / 100
}

func copysign(v, s float64) float64 {
	if s < 0 {
		return -v
	}
	return v
}

func joinComma(p []string) string {
	var out strings.Builder
	for i, s := range p {
		if i > 0 {
			out.WriteString(", ")
		}
		out.WriteString(s)
	}
	return out.String()
}

func fnAt(g *Graph, s *Symbol) string {
	return g.Files[s.FileID-1].Path() + ":" + strconv.Itoa(int(s.LineStart))
}

func heaviest(g *Graph) [][]fval {
	var xs []*Symbol
	for i := range g.Symbols {
		s := g.Symbols[i]
		if s.Kind() == "function" || s.Kind() == "method" ||
			s.Kind() == "constructor" || s.Kind() == "closure" {
			xs = append(xs, s)
		}
	}
	sort.SliceStable(xs, func(i, j int) bool {
		return xs[i].Cyclomatic > xs[j].Cyclomatic
	})
	if len(xs) > 12 {
		xs = xs[:12]
	}
	var out [][]fval
	for _, s := range xs {
		out = append(out, []fval{fStr(s.Name()), fI32(s.Sloc), fI32(s.Cyclomatic),
			fI32(s.Cognitive), fI32(s.MaxNesting), fI32(s.FanIn), fStr(fnAt(g, s))})
	}
	return out
}

func mostDepended(g *Graph) [][]fval {
	var xs []*Symbol
	for i := range g.Symbols {
		s := g.Symbols[i]
		if s.Kind() == "function" || s.Kind() == "method" ||
			s.Kind() == "constructor" || s.Kind() == "closure" {
			xs = append(xs, s)
		}
	}
	sort.SliceStable(xs, func(i, j int) bool { return xs[i].FanIn > xs[j].FanIn })
	if len(xs) > 12 {
		xs = xs[:12]
	}
	var out [][]fval
	for _, s := range xs {
		out = append(out, []fval{fStr(s.Name()), fI32(s.FanIn), fI32(s.FanOut),
			fI32(s.Cyclomatic), fI32(s.Sloc), fStr(fnAt(g, s))})
	}
	return out
}

type fkind int

const (
	fkNull fkind = iota
	fkInt
	fkFloat
	fkStr
	fkBlob
)

type fval struct {
	kind fkind
	i    int64
	f    float64
	s    string
}

func fNull() fval        { return fval{kind: fkNull} }
func fInt(v int) fval    { return fval{kind: fkInt, i: int64(v)} }
func fI32(v int32) fval  { return fval{kind: fkInt, i: int64(v)} }
func fF(v float64) fval  { return fval{kind: fkFloat, f: v} }
func fStr(s string) fval { return fval{kind: fkStr, s: s} }
func fOptInt(v int32, ok bool) fval {
	if !ok {
		return fNull()
	}
	return fI32(v)
}
func fOptStr(v string, ok bool) fval {
	if !ok {
		return fNull()
	}
	return fStr(v)
}

func escapeText(s string) string {
	var sb strings.Builder
	sb.Grow(len(s) + 2)
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch c {
		case '\\':
			sb.WriteString(`\\`)
		case '\n':
			sb.WriteString(`\n`)
		case '\t':
			sb.WriteString(`\t`)
		case '\r':
			sb.WriteString(`\r`)
		default:
			if c < 0x20 || c == 0x7F {
				fmt.Fprintf(&sb, `\x%02X`, c)
			} else {
				sb.WriteByte(c)
			}
		}
	}
	return sb.String()
}

func cgFloat(f float64) string {
	if math.IsInf(f, 1) {
		return "inf"
	}
	if math.IsInf(f, -1) {
		return "-inf"
	}
	if math.IsNaN(f) {
		return "nan"
	}
	return cgRepr(f)
}

type dumper struct {
	w    *bufio.Writer
	buf  []byte
	rows [][2]int32
}

// appendField is encodeField written into a buffer instead of a new string.
func appendField(b []byte, v fval) []byte {
	switch v.kind {
	case fkNull:
		return append(b, '\\', 'N')
	case fkInt:
		return strconv.AppendInt(append(b, 'i', ':'), v.i, 10)
	case fkFloat:
		return append(append(b, 'f', ':'), cgFloat(v.f)...)
	}
	b = append(b, 's', ':')
	const hex = "0123456789ABCDEF"
	for i := 0; i < len(v.s); i++ {
		switch c := v.s[i]; c {
		case '\\':
			b = append(b, '\\', '\\')
		case '\n':
			b = append(b, '\\', 'n')
		case '\t':
			b = append(b, '\\', 't')
		case '\r':
			b = append(b, '\\', 'r')
		default:
			if c < 0x20 || c == 0x7F {
				b = append(b, '\\', 'x', hex[c>>4], hex[c&15])
			} else {
				b = append(b, c)
			}
		}
	}
	return b
}

// table writes one table with its rows in byte order. Every row is encoded
// into one buffer and the sort moves (offset, end) pairs: a string per field,
// a slice per row and a joined line per row was 1.3 million rows' worth of
// garbage on PostgreSQL, and most of the time --dump took.
func (d *dumper) table(name string, ncols int, rows [][]fval) {
	buf, sp := d.buf[:0], d.rows[:0]
	for _, r := range rows {
		lo := len(buf)
		buf = append(buf, 'R')
		if len(r) == 0 {
			buf = append(buf, ' ')
		}
		for _, v := range r {
			buf = append(buf, ' ')
			buf = appendField(buf, v)
		}
		if len(buf) > math.MaxInt32 {
			panic("dump: table " + name + " exceeds 2 GB of text")
		}
		sp = append(sp, [2]int32{int32(lo), int32(len(buf))})
	}
	slices.SortFunc(sp, func(a, b [2]int32) int {
		return bytes.Compare(buf[a[0]:a[1]], buf[b[0]:b[1]])
	})
	fmt.Fprintf(d.w, "T %s %d %d\n", name, ncols, len(sp))
	for _, r := range sp {
		d.w.Write(buf[r[0]:r[1]])
		d.w.WriteByte('\n')
	}
	fmt.Fprintf(d.w, "E %s\n", name)
	d.buf, d.rows = buf, sp
}

func (g *Graph) dump(w io.Writer) {
	d := &dumper{w: bufio.NewWriterSize(w, 1<<20)}
	g.dumpW = d.w

	d.table("addr_taken", 6, g.tAddrTaken())
	d.table("allocsites", 6, g.tAllocSites())
	d.table("apiuse", 6, g.tAPIUse())
	d.table("attributes", 6, g.tAttributes())
	d.table("callsites", 3, g.tCallsites())
	d.table("config_blocks", 6, g.tConfigBlocks())
	d.table("declarations", 4, g.tDeclarations())
	d.table("edges", 6, g.tEdges())
	d.table("enum_members", 5, g.tEnumMembers())
	d.table("eventops", 7, g.tEventOps())
	d.table("fields", 14, g.tFields())
	d.table("files", 29, g.tFiles())
	d.table("globals", 13, g.tGlobals())
	d.table("hazards", 5, g.tHazards())
	d.table("imports", 13, g.tImports())
	d.table("include_cycles", 5, g.tCycles())
	d.table("layout", 11, g.tLayout())
	d.table("literals", 7, g.tLiterals())
	d.table("locals", 11, g.tLocals())
	d.table("locks", 5, g.tLocks())
	d.table("macros", 7, g.tMacros())
	d.table("makefile_rules", 7, g.tMakefileRules())
	d.table("markers", 6, g.tMarkers())
	d.table("memops", 10, g.tMemops())
	d.table("meta", 2, g.tMeta())
	d.table("modules", 10, g.tModules())
	d.table("params", 13, g.tParams())
	d.table("reach", 3, g.tReach())
	d.table("secret_candidates", 5, g.tSecrets())
	d.table("struct_size", 7, g.tStructSize())
	g.tFTS()
	d.table("symbols", 174, g.tSymbols())
	d.table("unresolved_calls", 4, g.tUnresolved())
	d.w.Flush()
}

func (g *Graph) tAddrTaken() [][]fval {
	out := make([][]fval, 0, len(g.Addr))
	for _, a := range g.Addr {
		out = append(out, []fval{fI32(a.ID), fOptInt(a.SymbolID, a.HasSym),
			fI32(a.FileID), fStr(a.Name()), fI32(a.Line), fStr(a.Kind())})
	}
	return out
}

func (g *Graph) tAllocSites() [][]fval {
	out := make([][]fval, 0, len(g.Allocs))
	for i, a := range g.Allocs {
		out = append(out, []fval{fInt(i + 1), fI32(a.SymbolID), fI32(a.FileID),
			fStr(a.Fn()), fStr(a.SizeExpr()), fI32(a.Line)})
	}
	return out
}

func (g *Graph) tAPIUse() [][]fval {
	out := make([][]fval, 0, len(g.APIUses))
	for i, a := range g.APIUses {
		out = append(out, []fval{fInt(i + 1), fI32(a.SymbolID), fI32(a.FileID),
			fStr(a.NS()), fStr(a.Fn()), fI32(a.Line)})
	}
	return out
}

func (g *Graph) tAttributes() [][]fval {
	out := make([][]fval, 0, len(g.Attrs))
	for i, a := range g.Attrs {
		out = append(out, []fval{fInt(i + 1), fOptInt(a.SymbolID, a.HasSymbolID),
			fI32(a.FileID), fStr(a.Name()), fOptStr(a.Args(), a.HasArgs), fI32(a.Line)})
	}
	return out
}

func (g *Graph) tCallsites() [][]fval {
	out := make([][]fval, 0, len(g.Callsites))
	for _, c := range g.Callsites {
		out = append(out, []fval{fI32(c.CallerID), fI32(c.CalleeID), fI32(c.Line)})
	}
	return out
}

func (g *Graph) tConfigBlocks() [][]fval {
	out := make([][]fval, 0, len(g.Cfgs))
	for i, c := range g.Cfgs {
		out = append(out, []fval{fInt(i + 1), fI32(c.FileID), fStr(c.Directive()),
			fStr(c.Expr()), fI32(c.Line), fI32(c.IsConfig)})
	}
	return out
}

func (g *Graph) tDeclarations() [][]fval {
	out := make([][]fval, 0, len(g.Decls))
	for i, c := range g.Decls {
		out = append(out, []fval{fInt(i + 1), fI32(c.FileID), fStr(c.Name()), fI32(c.Line)})
	}
	return out
}

func (g *Graph) tEdges() [][]fval {
	out := make([][]fval, 0, len(g.Edges))
	for _, e := range g.Edges {
		out = append(out, []fval{fI32(e.CallerID), fI32(e.CalleeID), fI32(e.NCalls),
			fI32(e.SameFile), fI32(e.SameModule), fI32(e.IsSelf)})
	}
	return out
}

func (g *Graph) tEnumMembers() [][]fval {
	out := make([][]fval, 0, len(g.EnumMem))
	for _, e := range g.EnumMem {
		out = append(out, []fval{fI32(e.SymbolID), fI32(e.Ordinal), fStr(e.Name()),
			fOptStr(e.Value(), e.HasValue), fI32(e.NFields)})
	}
	return out
}

func (g *Graph) tEventOps() [][]fval {
	out := make([][]fval, 0, len(g.EvOps))
	for i, e := range g.EvOps {
		out = append(out, []fval{fInt(i + 1), fI32(e.SymbolID), fI32(e.FileID),
			fStr(e.Family()), fStr(e.Fn()), fStr(e.Args()), fI32(e.Line)})
	}
	return out
}

func (g *Graph) tFields() [][]fval {
	out := make([][]fval, 0, len(g.Fields))
	for _, f := range g.Fields {
		out = append(out, []fval{fI32(f.SymbolID), fI32(f.Ordinal), fStr(f.Name()),
			fStr(f.Type()), fStr(f.Vis()), fI32(f.Line), fI32(f.IsStatic),
			fI32(f.IsConst), fI32(f.IsMutable), fI32(f.IsNullable),
			fI32(f.IsCollection), fI32(f.IsUntyped), fI32(f.HasDefault),
			fI32(f.TypeDepth)})
	}
	return out
}

func (g *Graph) tFiles() [][]fval {
	out := make([][]fval, 0, len(g.Files))
	for _, f := range g.Files {
		out = append(out, []fval{fI32(f.ID), fStr(f.Path()), fStr(f.Dir()),
			fStr(f.Basename()), fStr(f.Ext()), fStr(f.Lang()), fI32(f.ModuleID),
			fI32(f.Bytes), fI32(f.Lines), fI32(f.Sloc), fI32(f.BlankLines),
			fI32(f.CommentLines), fI32(f.DocLines), fI32(f.MaxLineLen),
			fStr(f.Sha1()), fI32(f.Parsed), fI32(f.IsTest), fI32(f.IsGenerated),
			fI32(f.IsVendored), fI32(f.NParseErrors), fI32(f.NMissingNodes),
			fF(f.ParseMs), fI32(f.NSymbols), fI32(f.NFunctions), fI32(f.NTypes),
			fI32(f.NImports), fI32(f.TotalCyclo), fI32(f.MaxCyclo),
			fI32(f.TotalRisk)})
	}
	return out
}

func (g *Graph) tGlobals() [][]fval {
	out := make([][]fval, 0, len(g.Globals))
	for i, m := range g.Globals {
		out = append(out, []fval{fInt(i + 1), fI32(m.FileID), fI32(m.ModuleID),
			fStr(m.Name()), fStr(m.Type()), fI32(m.Line), fI32(m.IsStatic),
			fI32(m.IsConst), fI32(m.IsVolatile), fI32(m.IsAtomic),
			fI32(m.IsArray), fI32(m.PtrDepth), fI32(m.HasInit)})
	}
	return out
}

func (g *Graph) tHazards() [][]fval {
	out := make([][]fval, 0, len(g.Hazards))
	for _, h := range g.Hazards {
		out = append(out, []fval{fI32(h.SymbolID), fStr(h.Pattern()),
			fStr(h.Category()), fI32(h.N), fI32(h.FirstLine)})
	}
	return out
}

func (g *Graph) tImports() [][]fval {
	out := make([][]fval, 0, len(g.Imports))
	for i, im := range g.Imports {
		out = append(out, []fval{fInt(i + 1), fI32(im.FileID), fStr(im.Target()),
			fOptInt(im.TargetID, im.HasTargetID),
			fOptStr(im.Alias(), im.HasAlias), fStr(im.Kind()), fI32(im.Line),
			fI32(im.IsExternal), fI32(im.IsRelative), fI32(im.IsWildcard),
			fI32(im.IsTypeOnly), fI32(im.IsDynamic), fI32(im.NNames)})
	}
	return out
}

func (g *Graph) tCycles() [][]fval {
	out := make([][]fval, 0, len(g.Cycles))
	for i, c := range g.Cycles {
		out = append(out, []fval{fInt(i + 1), fStr(c.APath()), fStr(c.BPath()),
			fI32(c.Length), fStr(c.Members())})
	}
	return out
}

func (g *Graph) tLayout() [][]fval {
	out := make([][]fval, 0, len(g.Layout))
	for _, l := range g.Layout {
		out = append(out, []fval{fI32(l.SymbolID), fI32(l.Ordinal),
			fI32(l.ByteOff), fI32(l.ByteSize), fI32(l.PadBefore), fI32(l.Exact),
			fI32(l.PtrDepth), fI32(l.ArrayLen), fI32(l.IsFnptr), fI32(l.Depth),
			fI32(l.InUnion)})
	}
	return out
}

func (g *Graph) tLiterals() [][]fval {
	out := make([][]fval, 0, len(g.Literals))
	for i, l := range g.Literals {
		out = append(out, []fval{fInt(i + 1), fOptInt(l.SymbolID, l.HasSym),
			fI32(l.FileID), fStr(l.Kind()), fStr(l.Value()), fI32(l.Line),
			fI32(l.IsMagic)})
	}
	return out
}

func (g *Graph) tLocals() [][]fval {
	out := make([][]fval, 0, len(g.Locals))
	for _, l := range g.Locals {
		out = append(out, []fval{fI32(l.SymbolID), fI32(l.Ordinal), fStr(l.Name()),
			fStr(l.Type()), fI32(l.Line), fI32(l.IsConst), fI32(l.IsMutable),
			fI32(l.IsUntyped), fI32(l.HasInit), fI32(l.InLoop),
			fI32(l.ScopeDepth)})
	}
	return out
}

func (g *Graph) tLocks() [][]fval {
	out := make([][]fval, 0, len(g.Locks))
	for i, l := range g.Locks {
		out = append(out, []fval{fInt(i + 1), fI32(l.SymbolID), fI32(l.FileID),
			fStr(l.Name()), fI32(l.Line)})
	}
	return out
}

func (g *Graph) tMacros() [][]fval {
	out := make([][]fval, 0, len(g.Macros))
	for _, m := range g.Macros {
		out = append(out, []fval{fI32(m.SymbolID), fI32(m.IsFunctionlike),
			fI32(m.NParams), fOptStr(m.Body(), m.HasBody), fI32(m.BodyLen),
			fI32(m.IsMultiline), fI32(m.NUses)})
	}
	return out
}

func (g *Graph) tMakefileRules() [][]fval {
	out := make([][]fval, 0, len(g.MkRules))
	for i, m := range g.MkRules {
		out = append(out, []fval{fInt(i + 1), fStr(m.Path()), fStr(m.Rule()),
			fI32(m.Line), fI32(m.NObjs), fI32(m.NSrcs), fI32(m.UsesAr)})
	}
	return out
}

func (g *Graph) tMarkers() [][]fval {
	out := make([][]fval, 0, len(g.Markers))
	for i, m := range g.Markers {
		out = append(out, []fval{fInt(i + 1), fI32(m.FileID),
			fOptInt(m.SymbolID, m.HasSym), fStr(m.Kind()), fI32(m.Line), fStr(m.Text())})
	}
	return out
}

func (g *Graph) tMemops() [][]fval {
	out := make([][]fval, 0, len(g.Memops))
	for i, m := range g.Memops {
		out = append(out, []fval{fInt(i + 1), fI32(m.SymbolID), fI32(m.FileID),
			fStr(m.Fn()), fStr(m.Dst()), fStr(m.Src()), fStr(m.SizeArg()),
			fStr(m.SizeBuf()), fStr(m.DstTail()), fI32(m.Line)})
	}
	return out
}

var metaOrder = []string{"addr_taken_pruned", "calls_resolved", "files_failed",
	"files_parsed", "files_skipped", "grammar_note", "imports_resolved", "lang",
	"layout_model", "makefiles_read", "parse_mode", "parser", "root",
	"schema_version", "sqlite", "target"}

func (g *Graph) tMeta() [][]fval {
	out := make([][]fval, 0, len(metaOrder))
	for _, k := range metaOrder {
		v, ok := g.Meta[k]
		if !ok {
			continue
		}
		out = append(out, []fval{fStr(k), fStr(v)})
	}
	return out
}

func (g *Graph) tModules() [][]fval {
	out := make([][]fval, 0, len(g.Modules))
	for _, m := range g.Modules {
		out = append(out, []fval{fI32(m.ID), fStr(m.Name()), fStr(m.Kind()),
			fI32(m.NFiles), fI32(m.NSymbols), fI32(m.NPublic), fI32(m.Sloc),
			fI32(m.FanIn), fI32(m.FanOut), fF(m.Instability)})
	}
	return out
}

func (g *Graph) tParams() [][]fval {
	out := make([][]fval, 0, len(g.Params))
	for _, p := range g.Params {
		out = append(out, []fval{fI32(p.SymbolID), fI32(p.Pos),
			fOptStr(p.Name(), p.HasName), fStr(p.Type()),
			fOptStr(p.Default(), p.HasDefaultValue), fI32(p.IsOptional),
			fI32(p.IsVariadic), fI32(p.IsRef), fI32(p.IsMutable),
			fI32(p.IsNullable), fI32(p.IsGeneric), fI32(p.IsUntyped),
			fI32(p.TypeDepth)})
	}
	return out
}

func (g *Graph) tReach() [][]fval {
	out := make([][]fval, 0, len(g.Reach))
	for _, r := range g.Reach {
		out = append(out, []fval{fI32(r.SymbolID), fI32(r.NTransitive),
			fI32(r.NTransitiveOut)})
	}
	return out
}

func (g *Graph) tSecrets() [][]fval {
	out := make([][]fval, 0, len(g.Secrets))
	for i, s := range g.Secrets {
		out = append(out, []fval{fInt(i + 1), fOptInt(s.SymbolID, s.HasSym),
			fI32(s.FileID), fStr(s.Value()), fI32(s.Line)})
	}
	return out
}

func (g *Graph) tStructSize() [][]fval {
	out := make([][]fval, 0, len(g.SSize))
	for _, s := range g.SSize {
		out = append(out, []fval{fI32(s.SymbolID), fI32(s.TotalSize),
			fI32(s.TailPad), fI32(s.TotalPad), fI32(s.MaxAlign), fI32(s.Exact),
			fI32(s.NLines64)})
	}
	return out
}

func (g *Graph) tUnresolved() [][]fval {
	out := make([][]fval, 0, len(g.Unres))
	for _, u := range g.Unres {
		out = append(out, []fval{fI32(u.CallerID), fStr(u.Name()), fI32(u.N),
			fI32(u.FirstLine)})
	}
	return out
}

const target = "C23 (ISO/IEC 9899:2024) + GNU extensions tolerated (real preprocessor)"

type qentry struct {
	name  string
	title string
	notes string
	run   func(g *Graph, p params) (cols []string, rows [][]fval)
}

func main() {
	if v := os.Getenv("CGORD"); v != "" {
		scanOrder = v
	}

	args := os.Args[1:]
	for _, a := range args {
		if a == "-h" || a == "--help" {

			usage()
			return
		}
	}
	var positional []string
	flags := map[string]string{}
	boolFlags := map[string]bool{}
	for i := 0; i < len(args); i++ {
		a := args[i]
		if isNegNumber(a) || !strings.HasPrefix(a, "-") {
			positional = append(positional, a)
			continue
		}
		name := strings.TrimLeft(a, "-")
		val := ""
		if j := strings.IndexByte(name, '='); j >= 0 {
			val, name = name[j+1:], name[:j]
		} else if i+1 < len(args) && (!strings.HasPrefix(args[i+1], "-") ||
			isNegNumber(args[i+1])) && takesValue(name) {
			val = args[i+1]
			i++
		}
		if !knownFlag(name) {
			usage()
			fmt.Fprintf(os.Stderr, "codegraph_c: error: unrecognized arguments: %s\n", a)
			os.Exit(2)
		}
		if takesValue(name) {
			flags[name] = val
		} else {
			boolFlags[name] = true
		}
	}

	qv := map[string]bool{}
	for _, k := range []string{"list", "metrics", "report", "quiet", "no-tests",
		"include-generated", "include-vendored", "version", "deps", "force",
		"install-deps", "schema", "summary", "parse-errors", "selfcheck"} {
		if boolFlags[k] {
			qv[k] = true
		}
	}
	lim := -1
	if v, ok := flags["limit"]; ok {
		n, err := strconv.Atoi(v)
		if err != nil {
			invalidArg("--limit", v)
		}
		lim = n
	}
	modPat := "%"
	if v, ok := flags["module"]; ok {
		modPat = v
	}
	csvN, jsonN := -1, -1
	_, csvSet := flags["csv"]
	_, jsonSet := flags["json"]
	dumpPath := flags["dump"]
	savePath := flags["save"]
	saveAST := flags["save-ast"]
	loadASTPath := flags["load-ast"]
	if saveAST != "" && loadASTPath != "" {
		fmt.Fprintln(os.Stderr, "--save-ast and --load-ast cannot be used together")
		os.Exit(2)
	}

	if qv["version"] {
		fmt.Printf("codegraph_c  target=%s  schema=v2  go=%s\n", target, goVersion())
		return
	}
	if qv["deps"] || qv["install-deps"] {
		fmt.Println("dependencies for codegraph-c:")
		fmt.Println("  (none -- pure standard library)")
		return
	}
	if qv["schema"] {
		fmt.Print(schemaNative())
		return
	}

	cat := queries
	if qv["metrics"] {
		cat = metricsCat
	}
	if qv["list"] {
		nameW := 0
		for _, q := range cat {
			nameW = max(nameW, len(q.name))
		}
		for i, q := range cat {
			fmt.Printf("%3d. %-*s %s\n", i+1, nameW, q.name, q.title)
		}
		if qv["metrics"] {
			fmt.Printf("\n%d metrics. Without --metrics, --list shows the %d defect queries.\n",
				len(cat), len(queries))
		} else {
			fmt.Printf("\n%d defect queries. --metrics --list shows the %d structural metrics.\n",
				len(cat), len(metricsCat))
		}
		return
	}
	if csvSet {
		csvN = pickQuery(cat, flags["csv"], qv["metrics"])
	}
	if jsonSet {
		jsonN = pickQuery(cat, flags["json"], qv["metrics"])
	}

	// A saved state has no root to name, so with --load-ast every positional
	// is a selector. Taking the first as a root anyway turned
	// `--load-ast s.ast dead-code` into "run all 119 queries".
	dir := "."
	if loadASTPath != "" {
		positional = append([]string{dir}, positional...)
	} else if len(positional) > 0 {
		dir = positional[0]
	}
	// A selector that names nothing is an error, not a silent no-op: dropping
	// it used to fall through to "run every query", which reads as the answer
	// to a question that was never asked.
	var sel []int
	if len(positional) > 1 {
		for _, p := range positional[1:] {
			sel = append(sel, pickQuery(cat, p, qv["metrics"]))
		}
	}
	if loadASTPath == "" {
		if st, err := os.Stat(dir); err != nil || !st.IsDir() {
			fmt.Fprintf(os.Stderr, "not a directory: %s\n", dir)
			os.Exit(2)
		}
	}

	if csvSet || jsonSet {
		qv["quiet"] = true
	}
	quiet := qv["quiet"]

	var g *Graph
	t0 := time.Now()
	var n int
	if loadASTPath != "" {
		var err error
		g, err = loadAST(loadASTPath)
		if err != nil {
			fmt.Fprintf(os.Stderr, "load-ast: %v\n", err)
			os.Exit(2)
		}
		n = len(g.Files)
	} else {
		g = &Graph{}
		opts := runOpts{includeTests: !qv["no-tests"],
			includeGenerated: qv["include-generated"],
			includeVendored:  qv["include-vendored"], quiet: quiet}
		n = build(g, dir, opts, quiet)
	}
	took := time.Since(t0)

	if saveAST != "" {
		if _, serr := os.Lstat(saveAST); serr == nil && !qv["force"] {
			fmt.Fprintf(os.Stderr, "refusing to overwrite %s (pass --force)\n", saveAST)
			os.Exit(2)
		}
		ts := time.Now()
		nb, werr := saveASTFile(g, saveAST)
		if werr != nil {
			fmt.Fprintf(os.Stderr, "save-ast: %v\n", werr)
			os.Exit(2)
		}
		if !quiet {
			fmt.Fprintf(os.Stderr, "ast state written to %s: %d bytes in %.1fs\n",
				saveAST, nb, time.Since(ts).Seconds())
		}
	}

	if dumpPath != "" {
		f, err := os.Create(dumpPath)
		if err != nil {
			fmt.Fprintf(os.Stderr, "could not write %s: %v\n", dumpPath, err)
			os.Exit(2)
		}
		bw := bufio.NewWriterSize(f, 1<<20)
		g.dump(bw)
		bw.Flush()
		f.Close()
	}

	p := params{mod: modPat, lim: lim}
	if csvSet || jsonSet {
		n2 := csvN
		if n2 < 0 {
			n2 = jsonN
		}
		warnCoverage(g)
		cols, rows := cat[n2-1].run(g, p)
		if csvN >= 0 {
			w := csv.NewWriter(os.Stdout)

			w.UseCRLF = true
			w.Write(cols)
			for _, r := range rows {
				w.Write(fieldStrings(r))
			}
			w.Flush()
		} else {
			enc := json.NewEncoder(os.Stdout)
			enc.SetIndent("", "  ")
			enc.SetEscapeHTML(false)
			out := make([]orderedObj, 0, len(rows))
			for _, r := range rows {
				m := orderedObj{}
				for i, c := range cols {
					m = append(m, orderedKV{Key: c, Val: jsonValue(r[i])})
				}
				out = append(out, m)
			}
			enc.Encode(out)
		}
		return
	}

	if !quiet {
		limStr := "all"
		if lim >= 0 {
			limStr = itoa(lim)
		}
		if loadASTPath != "" {
			fmt.Fprintf(os.Stderr, "codegraph-c: %d files loaded from AST in %.1fs module=%s limit=%s\n",
				n, took.Seconds(), modPat, limStr)
		} else {
			fmt.Fprintf(os.Stderr, "codegraph-c: %d files parsed into memory in %.1fs module=%s limit=%s\n",
				n, took.Seconds(), modPat, limStr)
		}
	}
	warnCoverage(g)
	alone := false
	if qv["report"] {
		report(g)
		alone = true
	}
	if qv["parse-errors"] {
		printParseErrors(g, lim, loadASTPath != "")
		alone = true
	}
	if qv["selfcheck"] {
		sw := bufio.NewWriter(os.Stdout)
		bad := selfCheck(g, sw)
		sw.Flush()
		if bad > 0 {
			saveGraph(g, savePath, qv["force"])
			os.Exit(1)
		}
		alone = true
	}
	if qv["summary"] {
		printSummary(g, cat, sel, p, qv["metrics"])
	}
	// --report, --parse-errors and --summary are answers in themselves: asked
	// for without a selector they do not also print every query.
	if qv["summary"] || (alone && len(sel) == 0) {
		saveGraph(g, savePath, qv["force"])
		return
	}
	if len(sel) == 0 {
		for i := 1; i <= len(cat); i++ {
			sel = append(sel, i)
		}
	}
	w := bufio.NewWriterSize(os.Stdout, 1<<16)
	for _, k := range sel {
		q := cat[k-1]
		fmt.Fprintf(w, "\n%s\n", strings.Repeat("=", 78))
		fmt.Fprintf(w, "Q%d. %s -- %s\n", k, q.name, q.title)
		fmt.Fprintf(w, "%s\n", strings.Repeat("-", 78))
		for _, line := range splitNotes(q.notes) {
			fmt.Fprintf(w, " %s\n", line)
		}
		fmt.Fprintln(w)
		cols, rows := q.run(g, p)
		render(w, rows, cols)
	}
	w.Flush()
	saveGraph(g, savePath, qv["force"])
}

// pickQuery resolves a selector -- a 1-based number or a query name -- against
// the active catalogue, and exits with the near misses when it names nothing.
func pickQuery(cat []qentry, sel string, metrics bool) int {
	which := "query"
	other := metricsCat
	if metrics {
		which = "metric"
		other = queries
	}
	if n, err := strconv.Atoi(sel); err == nil {
		if n >= 1 && n <= len(cat) {
			return n
		}
		fmt.Fprintf(os.Stderr, "codegraph_c: no %s %d (there are %d; see --list)\n",
			which, n, len(cat))
		os.Exit(2)
	}
	for i, q := range cat {
		if q.name == sel {
			return i + 1
		}
	}
	var near []string
	for _, q := range cat {
		if strings.Contains(q.name, sel) {
			near = append(near, q.name)
		}
	}
	if len(near) == 1 {
		for i, q := range cat {
			if q.name == near[0] {
				return i + 1
			}
		}
	}
	fmt.Fprintf(os.Stderr, "codegraph_c: no %s named %q (see --list)\n", which, sel)
	if len(near) > 0 {
		fmt.Fprintf(os.Stderr, "  did you mean: %s\n", joinComma(near))
	}
	for _, q := range other {
		if q.name == sel {
			if metrics {
				fmt.Fprintf(os.Stderr, "  %q is a defect query: drop --metrics\n", sel)
			} else {
				fmt.Fprintf(os.Stderr, "  %q is a metric: add --metrics\n", sel)
			}
		}
	}
	os.Exit(2)
	return 0
}

// warnCoverage says on stderr, even under --quiet, when more than 1% of the
// files did not parse cleanly. Every ranking is computed from what parsed.
func warnCoverage(g *Graph) {
	parsed, bad, errs := 0, 0, 0
	for i := range g.Files {
		f := &g.Files[i]
		if f.Parsed != 1 {
			continue
		}
		parsed++
		if f.NParseErrors > 0 {
			bad++
			errs += int(f.NParseErrors)
		}
	}
	if parsed > 0 && bad*100 > parsed {
		fmt.Fprintf(os.Stderr, "codegraph_c: %d of %d files have parse diagnostics (%d in all); "+
			"functions near them may be missing or merged. --parse-errors lists them.\n",
			bad, parsed, errs)
	}
}

func printParseErrors(g *Graph, lim int, fromAST bool) {
	w := bufio.NewWriter(os.Stdout)
	defer w.Flush()
	fmt.Fprintf(w, "\n%s\nPARSE ERRORS\n%s\n", strings.Repeat("=", 78), strings.Repeat("-", 78))
	total := 0
	for i := range g.Files {
		total += int(g.Files[i].NParseErrors)
	}
	nErrDir := 0
	for _, m := range g.ParseMsgs {
		if strings.HasPrefix(m.msg, "#error") {
			nErrDir++
		}
	}
	if total == 0 && nErrDir == 0 {
		fmt.Fprintln(w, " none: every file parsed without a diagnostic")
		return
	}
	if fromAST {
		fmt.Fprintf(w, " %d diagnostics were counted when this state was saved; the messages\n"+
			" are not stored in it. Re-run against the source tree to list them.\n", total)
		return
	}
	kinds := map[string]int{}
	for _, m := range g.ParseMsgs {
		kinds[parseMsgKind(m.msg)]++
	}
	var kr [][]fval
	for k, v := range kinds {
		kr = append(kr, []fval{fInt(v), fStr(k)})
	}
	orderBy(kr, dI(0), cmpS(1))
	fmt.Fprintf(w, " %d diagnostics. One unreadable construct is counted once per token it\n"+
		" derails, so the FIRST message in a file is the cause and the rest are\n"+
		" usually its echo.\n", total)
	if nErrDir > 0 {
		fmt.Fprintf(w, " Plus %d `#error` directive(s) reached. Those are not parse failures: the\n"+
			" code refused a configuration this run could not determine (a generated\n"+
			" config.h, a compiler it does not recognise), and what was read on the\n"+
			" way there is intact. They are not in the count above.\n", nErrDir)
	}
	fmt.Fprintln(w)
	render(w, kr, []string{"n", "kind"})
	fmt.Fprintln(w)
	msgs := append([]parseMsg(nil), g.ParseMsgs...)
	sort.SliceStable(msgs, func(i, j int) bool {
		if msgs[i].fileID != msgs[j].fileID {
			return g.Files[msgs[i].fileID-1].Path() < g.Files[msgs[j].fileID-1].Path()
		}
		return msgs[i].line < msgs[j].line
	})
	var rows [][]fval
	lastFile, lastLine := int32(0), int32(-1)
	for _, m := range msgs {
		if m.fileID == lastFile && m.line == lastLine {
			continue
		}
		lastFile, lastLine = m.fileID, m.line
		rows = append(rows, []fval{fStr(g.Files[m.fileID-1].Path() + ":" +
			strconv.Itoa(int(m.line))), fStr(jtrunc(m.msg, 90))})
	}
	if lim >= 0 && len(rows) > lim {
		rows = rows[:lim]
	}
	render(w, rows, []string{"at", "first diagnostic on this line"})
}

// parseMsgKind strips the quoted token out of a diagnostic so that messages
// differing only in what they tripped over group together.
func parseMsgKind(m string) string {
	if i := strings.Index(m, " near '"); i >= 0 {
		if j := strings.LastIndexByte(m, '\''); j > i+6 {
			return m[:i+6] + "..." + m[j+1:]
		}
	}
	if i := strings.Index(m, " token '"); i >= 0 {
		if j := strings.LastIndexByte(m, '\''); j > i+7 {
			return m[:i+7] + "..." + m[j+1:]
		}
	}
	if strings.HasPrefix(m, "#error") {
		return "#error reached"
	}
	return m
}

// printSummary runs every selected query and prints one line each: the name,
// how many rows it found and the top row. It is the index to a report that is
// otherwise twenty thousand lines long.
func printSummary(g *Graph, cat []qentry, sel []int, p params, metrics bool) {
	w := bufio.NewWriter(os.Stdout)
	defer w.Flush()
	if len(sel) == 0 {
		for i := 1; i <= len(cat); i++ {
			sel = append(sel, i)
		}
	}
	all := p
	all.lim = -1
	var rows [][]fval
	hit := 0
	for _, k := range sel {
		q := cat[k-1]
		cols, rs := q.run(g, all)
		top := ""
		if len(rs) > 0 {
			hit++
			top = summaryTop(cols, rs[0])
		}
		rows = append(rows, []fval{fInt(k), fStr(q.name), fInt(len(rs)), fStr(top)})
	}
	what := "QUERIES"
	if metrics {
		what = "METRICS"
	}
	fmt.Fprintf(w, "\n%s\nSUMMARY OF %s  module=%s\n%s\n", strings.Repeat("=", 78),
		what, p.mod, strings.Repeat("-", 78))
	fmt.Fprintf(w, " %d of %d returned rows. `rows` is how many the query found, not how\n"+
		" many are defects: read a query's MISLEADS note before acting on it.\n\n",
		hit, len(sel))
	render(w, rows, []string{"#", "name", "rows", "first row"})
}

// summaryTop is the first row's identifying cell and where it is.
func summaryTop(cols []string, r []fval) string {
	name, at := "", ""
	for i, c := range cols {
		if i >= len(r) || r[i].kind != fkStr {
			continue
		}
		switch {
		case c == "at" || c == "first_at":
			if at == "" {
				at = r[i].s
			}
		case name == "":
			name = r[i].s
		}
	}
	if at != "" && name != "" {
		return jtrunc(name, 44) + "  " + at
	}
	return jtrunc(name+at, 70)
}

// selfCheck tests the graph against things its numbers must obey whatever
// the code under analysis looks like. A parser or walker bug rarely crashes:
// it produces a count that is plausible on its own and impossible beside
// another count. Each rule below pairs two numbers that were computed by
// different code, so a fault in either shows up as a violation.
//
// It prints one row per rule and returns the number of rules violated.
func selfCheck(g *Graph, w *bufio.Writer) int {
	type rule struct {
		name    string
		checked int
		bad     int
		first   string
	}
	var rules []*rule
	add := func(name string) *rule {
		r := &rule{name: name}
		rules = append(rules, r)
		return r
	}
	fail := func(r *rule, ok bool, where func() string) {
		r.checked++
		if !ok {
			r.bad++
			if r.first == "" {
				r.first = where()
			}
		}
	}
	isFn := func(s *Symbol) bool { return s != nil && s.Kind() == "function" }
	at := func(s *Symbol) func() string {
		return func() string { return s.Name() + "  " + g.at(s) }
	}

	rLoopDepth := add("a function with loops has loop depth >= 1 and <= its loop count")
	rInLoop := add("nothing is counted `in a loop` in a function that has no loop")
	rRet := add("the five return shapes add up to the return count")
	rCyclo := add("cyclomatic complexity is at least 1 + the number of loops")
	rCmp := add("null checks and signed/size compares are subsets of all compares")
	rLocals := add("n_locals equals the function's rows in locals")
	rParams := add("n_params equals the function's rows in params, `...` aside")
	rVariadic := add("a function is variadic exactly when it has one `...` row, and that row is last")
	rLine := add("every symbol starts on a line its file has")

	nLoc := map[int32]int32{}
	for _, l := range g.Locals {
		nLoc[l.SymbolID]++
	}
	nPar, nVar, lastPos := map[int32]int32{}, map[int32]int32{}, map[int32]int32{}
	for _, p := range g.Params {
		if p.IsVariadic == 1 {
			nVar[p.SymbolID]++
		} else {
			nPar[p.SymbolID]++
		}
		lastPos[p.SymbolID] = max(lastPos[p.SymbolID], p.Pos)
	}
	varLast := map[int32]bool{}
	for _, p := range g.Params {
		if p.IsVariadic == 1 && p.Pos == lastPos[p.SymbolID] {
			varLast[p.SymbolID] = true
		}
	}
	for _, s := range g.syms() {
		f := g.fileOf(s.ID)
		fail(rLine, f != nil && s.LineStart >= 1 && (f.Lines == 0 || s.LineStart <= f.Lines+1), at(s))
		if !isFn(s) {
			continue
		}
		r := g.rv(s)
		fail(rLoopDepth, (s.NLoops == 0 && s.MaxLoopDepth == 0) ||
			(s.NLoops > 0 && s.MaxLoopDepth >= 1 && s.MaxLoopDepth <= s.NLoops), at(s))
		inLoop := s.CallInLoop + s.AllocInLoop + s.IOInLoop + s.LockInLoop +
			s.BranchInLoop + r.SwitchInLoop + r.LibmInLoop + r.DivInLoop + r.StrlenInLoop
		fail(rInLoop, s.NLoops > 0 || inLoop == 0, at(s))
		fail(rRet, r.RetNull+r.RetNeg+r.RetZero+r.RetVal+r.RetVoid == s.NReturns, at(s))
		fail(rCyclo, s.Cyclomatic >= 1+s.NLoops, at(s))
		fail(rCmp, s.NNullCheck <= s.NCmp && r.NSignCmp <= s.NCmp, at(s))
		fail(rLocals, s.NLocals == nLoc[s.ID], at(s))
		fail(rParams, s.NParams == nPar[s.ID], at(s))
		fail(rVariadic, (s.IsVariadic == 0 && nVar[s.ID] == 0) ||
			(s.IsVariadic == 1 && nVar[s.ID] == 1 && varLast[s.ID]), at(s))
	}

	rEdgeEnds := add("both ends of every call edge are functions that exist")
	rEdgeFlags := add("an edge's is_self/same_file/same_module agree with its two ends")
	rFan := add("fan_in and fan_out equal the distinct callers/callees in edges")
	rSites := add("an edge has >= 1 call and no more distinct call lines than calls")
	rEdgeOne := add("no two edges join the same caller and callee")
	{
		// module-coupling counts edge rows as distinct caller/callee pairs
		out := g.edgeCSR(false)
		var seen []int32
		for id := int32(1); int(id) <= len(g.Symbols); id++ {
			es := out.of(id)
			if len(es) == 0 {
				continue
			}
			seen = seen[:0]
			for _, j := range es {
				seen = append(seen, g.Edges[j].CalleeID)
			}
			slices.Sort(seen)
			s := g.Symbols[id-1]
			fail(rEdgeOne, len(slices.Compact(seen)) == len(es), at(s))
		}
	}
	fin, fout := map[int32]int32{}, map[int32]int32{}
	sites := map[[2]int32]int32{}
	for _, c := range g.Callsites {
		sites[[2]int32{c.CallerID, c.CalleeID}]++
	}
	for i := range g.Edges {
		ed := &g.Edges[i]
		a, b := g.sym(ed.CallerID), g.sym(ed.CalleeID)
		where := func() string {
			an, bn := "?", "?"
			if a != nil {
				an = a.Name()
			}
			if b != nil {
				bn = b.Name()
			}
			return an + " -> " + bn
		}
		fail(rEdgeEnds, isFn(a) && isFn(b), where)
		if a == nil || b == nil {
			continue
		}
		fail(rEdgeFlags, (ed.IsSelf == 1) == (a.ID == b.ID) &&
			(ed.SameFile == 1) == (a.FileID == b.FileID) &&
			(ed.SameModule == 1) == (a.ModuleID == b.ModuleID), where)
		fail(rSites, ed.NCalls >= 1 && sites[[2]int32{a.ID, b.ID}] <= ed.NCalls, where)
		if ed.IsSelf == 0 {
			fout[a.ID]++
			fin[b.ID]++
		}
	}
	for _, s := range g.syms() {
		if isFn(s) {
			fail(rFan, s.FanIn == fin[s.ID] && s.FanOut == fout[s.ID], at(s))
		}
	}
	rReach := add("transitive callers are at least as many as direct callers")
	for _, r := range g.Reach {
		if s := g.sym(r.SymbolID); isFn(s) {
			fail(rReach, r.NTransitive >= s.FanIn, at(s))
		}
	}

	rFileLines := add("code + comment + blank lines do not exceed a file's line count")
	rFileFns := add("a file's n_functions equals its function symbols")
	rFileDoc := add("doc lines are comment lines: doc_lines <= comment_lines")
	fileFns := map[int32]int32{}
	for _, s := range g.syms() {
		if isFn(s) {
			fileFns[s.FileID]++
		}
	}
	for i := range g.Files {
		f := &g.Files[i]
		where := func() string { return f.Path() }
		fail(rFileLines, f.Sloc+f.CommentLines+f.BlankLines <= f.Lines+1 && f.Sloc >= 0, where)
		fail(rFileFns, f.NFunctions == fileFns[f.ID], where)
		fail(rFileDoc, f.DocLines >= 0 && f.DocLines <= f.CommentLines, where)
	}

	rSize := add("struct: pad < size, size % align == 0, lines64 == ceil(size/64)")
	rLayFit := add("every laid-out member lies inside its struct")
	rLayOrder := add("struct members do not overlap; union members all start at 0")
	rLayPad := add("member sizes + member padding + tail padding == struct size")
	rPacked := add("packed size == member bytes rounded up to the alignment")
	size := map[int32]*StructSize{}
	for i := range g.SSize {
		size[g.SSize[i].SymbolID] = &g.SSize[i]
	}
	packed := g.shapeCounts("packed_size")
	type acc struct {
		end, pad, sum int32
		ok, overlap   bool
	}
	lay := map[int32]*acc{}
	for i := range g.Layout {
		l := &g.Layout[i]
		if l.Depth != 0 || l.InUnion != 0 || l.ByteOff < 0 {
			continue
		}
		ss, s := size[l.SymbolID], g.sym(l.SymbolID)
		if ss == nil || s == nil || ss.Exact != 1 {
			continue
		}
		fail(rLayFit, l.ByteOff+l.ByteSize <= ss.TotalSize, at(s))
		a := lay[l.SymbolID]
		if a == nil {
			a = &acc{ok: true}
			lay[l.SymbolID] = a
		}
		if s.Kind() == "union" {
			if l.ByteOff != 0 {
				a.overlap = true
			}
			continue
		}
		if l.ByteOff < a.end {
			a.overlap = true
		}
		a.end = l.ByteOff + l.ByteSize
		a.pad += l.PadBefore
		a.sum += l.ByteSize
	}
	for i := range g.SSize {
		ss := &g.SSize[i]
		s := g.sym(ss.SymbolID)
		if s == nil || ss.Exact != 1 {
			continue
		}
		fail(rSize, ss.TotalPad >= 0 && (ss.TotalSize == 0 || ss.TotalPad < ss.TotalSize) &&
			ss.MaxAlign >= 1 && ss.TotalSize%ss.MaxAlign == 0 &&
			ss.NLines64 == (ss.TotalSize+63)/64, at(s))
		a := lay[ss.SymbolID]
		if a == nil {
			continue
		}
		fail(rLayOrder, !a.overlap, at(s))
		if s.Kind() == "struct" {
			fail(rLayPad, a.pad+ss.TailPad == ss.TotalPad && a.sum+ss.TotalPad == ss.TotalSize, at(s))
			// Widest-alignment-first leaves no interior hole, so the packed
			// size is exactly the member bytes rounded up to the alignment --
			// and it must be reported whenever that beats the declared size.
			want := a.sum
			if r := want % ss.MaxAlign; r != 0 {
				want += ss.MaxAlign - r
			}
			pk, has := packed[ss.SymbolID]
			if !has {
				pk = ss.TotalSize
			}
			fail(rPacked, pk == want, at(s))
		}
	}

	// The parser stores a struct body as a flat range that also covers the
	// members of any aggregate declared inside it. Hoisting those is right for
	// an anonymous member and wrong for a named one, and the sizes above
	// cannot tell: a struct that swallowed its nested member's fields is still
	// self-consistent. The fields table can -- there the leak shows as the
	// nested aggregate's whole member list, same lines and all, sitting
	// directly in front of the named member that should have hidden it.
	rNest := add("a named nested struct/union member keeps its fields to itself")
	{
		byTag := map[int32][]*Field{}
		for i := range g.Fields {
			f := &g.Fields[i]
			byTag[f.SymbolID] = append(byTag[f.SymbolID], f)
		}
		type fkey struct {
			file, line int32
			name       string
		}
		first := map[fkey][]int32{}
		for id, fs := range byTag {
			sort.Slice(fs, func(a, b int) bool { return fs[a].Ordinal < fs[b].Ordinal })
			if t := g.sym(id); t != nil && fs[0].Name() != "" {
				k := fkey{t.FileID, fs[0].Line, fs[0].Name()}
				first[k] = append(first[k], id)
			}
		}
		for id, fs := range byTag {
			o := g.sym(id)
			if o == nil {
				continue
			}
			leak := false
			for i := 0; i < len(fs) && !leak; i++ {
				for _, tid := range first[fkey{o.FileID, fs[i].Line, fs[i].Name()}] {
					ts := byTag[tid]
					if tid == id || i+len(ts) >= len(fs) {
						continue
					}
					same := true
					for k := range ts {
						if ts[k].Name() != fs[i+k].Name() || ts[k].Line != fs[i+k].Line ||
							ts[k].Type() != fs[i+k].Type() {
							same = false
							break
						}
					}
					t, m := g.sym(tid), fs[i+len(ts)]
					if same && m.Name() != "" && (m.Type() == t.Kind() ||
						m.Type() == t.Kind()+" "+t.Name()) {
						leak = true
						break
					}
				}
			}
			fail(rNest, !leak, at(o))
		}
	}

	rHaz := add("every hazard row counts >= 1 occurrence in a symbol that exists")
	for _, h := range g.Hazards {
		hh := h
		fail(rHaz, hh.N >= 1 && g.sym(hh.SymbolID) != nil, func() string {
			return hh.Pattern() + " on symbol " + strconv.Itoa(int(hh.SymbolID))
		})
	}
	rFrame := add("a function's largest local is no larger than its whole frame")
	frame, big := g.shapeCounts("frame_bytes"), g.shapeCounts("frame_max_local")
	for id, b := range big {
		if s := g.sym(id); s != nil {
			fail(rFrame, b <= frame[id], at(s))
		}
	}
	rDisc := add("a result is not ignored at more call sites than there are calls")
	_, rows := qQ25(g, params{mod: "%", lim: -1})
	for _, r := range rows {
		rr := r
		fail(rDisc, rr[1].i <= rr[2].i, func() string { return rr[0].s })
	}

	fmt.Fprintf(w, "\n%s\nSELF-CHECK\n%s\n", strings.Repeat("=", 78), strings.Repeat("-", 78))
	fmt.Fprintln(w, " Each rule ties two numbers computed by different code. A violation is a")
	fmt.Fprintln(w, " bug in this tool, never a property of the code it read.")
	fmt.Fprintln(w)
	var out [][]fval
	nBad := 0
	for _, r := range rules {
		verdict := "ok"
		if r.checked == 0 {
			verdict = "n/a"
		}
		if r.bad > 0 {
			verdict = "VIOLATED"
			nBad++
		}
		out = append(out, []fval{fStr(verdict), fInt(r.bad), fInt(r.checked),
			fStr(r.name), fStr(r.first)})
	}
	render(w, out, []string{"verdict", "bad", "checked", "rule", "first violation"})
	fmt.Fprintf(w, "\n %d of %d rules violated\n", nBad, len(rules))
	return nBad
}

func symlinkReal(link string) string {
	tgt, err := os.Readlink(link)
	if err != nil {
		return link
	}
	if !filepath.IsAbs(tgt) {
		tgt = filepath.Join(filepath.Dir(link), tgt)
	}
	if r, err := filepath.EvalSymlinks(tgt); err == nil {
		return r
	}
	if r, err := filepath.EvalSymlinks(filepath.Dir(tgt)); err == nil {
		return filepath.Join(r, filepath.Base(tgt))
	}
	return tgt
}

func saveGraph(g *Graph, path string, force bool) {
	if path == "" {
		return
	}
	if fi, err := os.Lstat(path); err == nil {
		if fi.Mode()&os.ModeSymlink != 0 {
			if !force {
				fmt.Fprintf(os.Stderr,
					"refusing to overwrite %s (pass --force) -- it is a symlink to %s\n",
					path, symlinkReal(path))
				return
			}
			os.Remove(path)
		} else if !force {
			fmt.Fprintf(os.Stderr, "refusing to overwrite %s (pass --force)\n", path)
			return
		}
	}
	f, err := os.Create(path)
	if err != nil {
		fmt.Fprintf(os.Stderr, "could not write %s: %v\n", path, err)
		os.Exit(2)
	}
	bw := bufio.NewWriterSize(f, 1<<20)
	g.dump(bw)
	bw.Flush()
	f.Close()
	fmt.Printf("(graph also written to %s)\n", path)
}

type cgStr struct{ Off, Ln uint64 }

var (
	cgArena  []byte
	cgIntern map[string]cgStr
	cgMu     sync.Mutex
)

func cgPut(s string) cgStr {
	if s == "" {
		return cgStr{}
	}
	cgMu.Lock()
	if v, ok := cgIntern[s]; ok {
		cgMu.Unlock()
		return v
	}
	off := uint64(len(cgArena))
	cgArena = append(cgArena, s...)
	v := cgStr{off, uint64(len(s))}
	if cgIntern == nil {
		cgIntern = make(map[string]cgStr, 1<<16)
	}
	cgIntern[s] = v
	cgMu.Unlock()
	return v
}

type cgMetaRow struct{ K, V cgStr }

func (m *cgMetaRow) Key() string { return m.K.Str() }
func (m *cgMetaRow) Val() string { return m.V.Str() }

func (s cgStr) Str() string {
	if s.Ln == 0 {
		return ""
	}
	return unsafe.String(&cgArena[s.Off], int(s.Ln))
}

var cgZero [4096]byte

func cgZeroRow(p unsafe.Pointer, n uintptr) {
	copy(unsafe.Slice((*byte)(p), n)[:n], cgZero[:n])
}

func cgasBytes[T any](rows []T) []byte {
	if len(rows) == 0 {
		return nil
	}
	return unsafe.Slice((*byte)(unsafe.Pointer(&rows[0])), len(rows)*int(unsafe.Sizeof(rows[0])))
}

const cgasMagic = "CGAS"

const (
	cgasVersion  = 5
	cgasHeaderSz = 32
	cgasSecSz    = 24
	cgasNSec     = 35
	cgasArenaLo  = 1 << 20
)

const (
	cgasSecStrings uint32 = 1 + iota
	cgasSecFiles
	cgasSecMods
	cgasSecSyms
	cgasSecRare
	cgasSecParams
	cgasSecFields
	cgasSecLocals
	cgasSecLits
	cgasSecMark
	cgasSecAttrs
	cgasSecImps
	cgasSecHaz
	cgasSecEnums
	cgasSecLayout
	cgasSecSSize
	cgasSecDecls
	cgasSecAddr
	cgasSecSecrets
	cgasSecAllocs
	cgasSecMemops
	cgasSecMacros
	cgasSecGlobals
	cgasSecCfgs
	cgasSecReach
	cgasSecMkRules
	cgasSecLocks
	cgasSecEvOps
	cgasSecAPIUses
	cgasSecCycles
	cgasSecEdges
	cgasSecSites
	cgasSecUnres
	cgasSecMeta
	cgasSecCounters
)

type cgasHeader struct {
	Magic    [4]byte
	Version  uint32
	SecCount uint32
	Pad      uint32
	Total    uint64
	ArenaLen uint64
}

type cgasSec struct {
	ID  uint32
	Pad uint32
	Off uint64
	Len uint64
}

type cgasCounters struct {
	FilesParsed, FilesFailed               int32
	EdgesN, MacroN, ExternN, DeclN, UnresN int32
	CallsTotal, AddrPruned, MakefilesRead  int32
	FilesSkippedBig, FilesSkippedSpecial   int32
	FilesSkippedEscape, FilesSkippedDenied int32
	WalkErrors, Pad                        int32
}

func cgasI16(v int32, what string) int16 {
	if v < -32768 || v > 32767 {
		panic("cgas: " + what + " = " + itoa(int(v)) + " exceeds int16 state width")
	}
	return int16(v)
}

func cgasFlag(v int32, what string) uint8 {
	if v < 0 || v > 1 {
		panic("cgas: " + what + " = " + itoa(int(v)) + " is not a 0/1 flag")
	}
	return uint8(v)
}

func cgasU8(v int32, what string) uint8 {
	if v < 0 || v > 255 {
		panic("cgas: " + what + " = " + itoa(int(v)) + " exceeds uint8 state width")
	}
	return uint8(v)
}

type cgasSymN struct {
	Name                                                                              cgStr
	QualName                                                                          cgStr
	Kind                                                                              cgStr
	Signature                                                                         cgStr
	ReturnType                                                                        cgStr
	ID, FileID, ModuleID, LineStart                                                   int32
	Sloc, NCommentLines, NTokens, NOperators                                          int32
	NOperands, Rare                                                                   int32
	NParams, Cyclomatic, Cognitive, MaxNesting, NDistinctOperators, NDistinctOperands int16
	NLoops, NBranches, NReturns, NSwitch, NCases, NLabels                             int16
	NGotos, MaxLoopDepth, CallInLoop, AllocInLoop, IOInLoop, LockInLoop               int16
	BranchInLoop, NLocals, NCmp, NArith, NShift, NFloatLit                            int16
	NMagic, NNullCheck, NCalls, NDynamicCalls, NUnresolvedCalls, FanIn                int16
	FanOut, NCallsites, RiskScore, NMemory, NAlloc, NIO                               int16
	NStdio, NExec, NLibm, NInteger, NConcurrency, NReentrancy                         int16
	NPtrLocals, NDeref, NCast, NSizeof, NIntrinsic, NAtomic                           int16
	NRestrict, NLikely, NBuiltin                                                      int16
	HasSignature, HasReturnType                                                       bool
	IsPublic, IsStatic, IsAbstract, IsOverride                                        uint8
	IsTest, IsEntrypoint, IsGenerated, HasDoc                                         uint8
	IsRecursive, IsInline, IsVariadic                                                 uint8
}

type cgasRareN struct {
	SwitchInLoop, LibmInLoop, DivInLoop, StrlenInLoop, RetNull, RetNeg                           int16
	RetZero, RetVal, RetVoid, NFnptrCalls, NMacroCalls, NExternalCalls                           int16
	NFree, NConstCast, NToctou, NLockAcquire, NLockRelease, NNarrowCast                          int16
	NSignCmp, NVariadicFmt, NMemcpy, NAllocsite, NGlobalWrite, NErrno                            int16
	NWeakRandom, NShiftVar, NReallocSelf, NVla, NGetenv, NAssertSide                             int16
	NFreeThenUse, NEpoll, NUring, NKqueue, NEventWait, NEtReg                                    int16
	NOneshotReg, NWriteReady, NErrFlag, NRearm, NDereg, NEagain                                  int16
	NEintr, NUringRes, NUringRing, NUringBarrier, NUringSqpoll, NNonblockSet                     int16
	NEventCreate, NEventDestroy, NUringSqe, NUringSeen, NUringUdata, NUringLink                  int16
	NUringStreamOps, NUringTeardown, NEvTimeoutIndefinite, NEvTimeoutZero, NEvBatchOne, NKqTimer int16
	NKqTimerZeroData, NKqReceipt, NRetNegCheck, NDomainGuard, NUcharCast, NErrnoZero             int16
	NEndptr, NVaEnd, NMapFailed, NMonotonicClock, NStackszArray, NPtrOvfCheck                    int16
	NCallocTransposed, NVaArgArr                                                                 int16
}

type cgasParamN struct {
	Name                                     cgStr
	Type                                     cgStr
	DefaultValue                             cgStr
	SymbolID                                 int32
	Pos, TypeDepth                           int16
	HasName, HasDefaultValue                 bool
	IsOptional, IsVariadic, IsRef, IsMutable uint8
	IsNullable, IsGeneric, IsUntyped         uint8
}

type cgasFieldN struct {
	Name                                     cgStr
	Type                                     cgStr
	Visibility                               cgStr
	SymbolID, Line                           int32
	Ordinal, TypeDepth                       int16
	IsStatic, IsConst, IsMutable, IsNullable uint8
	IsCollection, IsUntyped, HasDefault      uint8
}

type cgasLocalN struct {
	Name                                   cgStr
	Type                                   cgStr
	SymbolID, Line                         int32
	Ordinal                                int16
	IsConst, IsMutable, IsUntyped, HasInit uint8
	InLoop, ScopeDepth                     uint8
}

func cgasBoolOffs[T any]() []uintptr {
	t := reflect.TypeFor[T]()
	var out []uintptr
	for i := 0; i < t.NumField(); i++ {
		if t.Field(i).Type.Kind() == reflect.Bool {
			out = append(out, t.Field(i).Offset)
		}
	}
	return out
}

var (
	symNBoolOffs    = cgasBoolOffs[cgasSymN]()
	paramNBoolOffs  = cgasBoolOffs[cgasParamN]()
	importBoolOffs  = cgasBoolOffs[Import]()
	attrBoolOffs    = cgasBoolOffs[Attribute]()
	literalBoolOffs = cgasBoolOffs[Literal]()
	enumBoolOffs    = cgasBoolOffs[EnumMember]()
	addrBoolOffs    = cgasBoolOffs[AddrTaken]()
	secretBoolOffs  = cgasBoolOffs[SecretCandidate]()
	macroBoolOffs   = cgasBoolOffs[Macro]()
)

var cgasGuardDone bool

func cgasGuard() {
	if cgasGuardDone {
		return
	}
	cgasGuardDone = true
	fail := func(ok bool, what string) {
		if !ok {
			panic("cgas: unsupported platform layout: " + what)
		}
	}
	fail(unsafe.Sizeof(uintptr(0)) == 8, "64-bit required")
	x := uint32(1)
	fail(*(*byte)(unsafe.Pointer(&x)) == 1, "little-endian required")
	fail(unsafe.Sizeof("") == 16, "string must be 16 bytes")
	g := "cgas-layout-guard"
	h := *(*[2]uintptr)(unsafe.Pointer(&g))
	fail(h[1] == uintptr(len(g)) &&
		h[0] == uintptr(unsafe.Pointer(unsafe.StringData(g))), "string header word order")
	fail(unsafe.Sizeof(cgasHeader{}) == cgasHeaderSz, "cgasHeader must be 32 bytes")
	fail(unsafe.Sizeof(cgasSec{}) == cgasSecSz, "cgasSec must be 24 bytes")
	fail(unsafe.Sizeof(cgStr{}) == 16, "cgStr must be the 16-byte {offset,length} pair")
	fail(unsafe.Sizeof(cgMetaRow{}) == 32, "cgMetaRow must be 32 bytes")
	fail(unsafe.Sizeof(cgasCounters{}) == 64, "cgasCounters must be 64 bytes")
	fail(unsafe.Sizeof(cgasSymN{}) == 240, "cgasSymN must be 240 bytes")
	fail(unsafe.Sizeof(cgasRareN{}) == 148, "cgasRareN must be 148 bytes")
	fail(unsafe.Sizeof(cgasParamN{}) == 72, "cgasParamN must be 72 bytes")
	fail(unsafe.Sizeof(cgasFieldN{}) == 72, "cgasFieldN must be 72 bytes")
	fail(unsafe.Sizeof(cgasLocalN{}) == 48, "cgasLocalN must be 48 bytes")
	fail(unsafe.Sizeof(cgasSymN{}) < unsafe.Sizeof(Symbol{}), "cgasSymN must narrow Symbol")
	fail(unsafe.Sizeof(cgasRareN{}) < unsafe.Sizeof(symRare{}), "cgasRareN must narrow symRare")
	fail(unsafe.Sizeof(cgasParamN{}) < unsafe.Sizeof(Param{}), "cgasParamN must narrow Param")
	fail(unsafe.Sizeof(cgasFieldN{}) < unsafe.Sizeof(Field{}), "cgasFieldN must narrow Field")
	fail(unsafe.Sizeof(cgasLocalN{}) < unsafe.Sizeof(Local{}), "cgasLocalN must narrow Local")
	for _, t := range []reflect.Type{
		reflect.TypeOf(File{}),
		reflect.TypeOf(Module{}),
		reflect.TypeOf(Symbol{}),
		reflect.TypeOf(symRare{}),
		reflect.TypeOf(Param{}),
		reflect.TypeOf(Field{}),
		reflect.TypeOf(Local{}),
		reflect.TypeOf(Literal{}),
		reflect.TypeOf(Marker{}),
		reflect.TypeOf(Attribute{}),
		reflect.TypeOf(Import{}),
		reflect.TypeOf(Hazard{}),
		reflect.TypeOf(EnumMember{}),
		reflect.TypeOf(LayoutRow{}),
		reflect.TypeOf(StructSize{}),
		reflect.TypeOf(Declaration{}),
		reflect.TypeOf(AddrTaken{}),
		reflect.TypeOf(SecretCandidate{}),
		reflect.TypeOf(AllocSite{}),
		reflect.TypeOf(Memop{}),
		reflect.TypeOf(Macro{}),
		reflect.TypeOf(Global{}),
		reflect.TypeOf(ConfigBlock{}),
		reflect.TypeOf(Reach{}),
		reflect.TypeOf(MakefileRule{}),
		reflect.TypeOf(LockRow{}),
		reflect.TypeOf(EventOp{}),
		reflect.TypeOf(APIUse{}),
		reflect.TypeOf(IncludeCycle{}),
		reflect.TypeOf(Edge{}),
		reflect.TypeOf(Callsite{}),
		reflect.TypeOf(Unresolved{}),
		reflect.TypeOf(cgasCounters{}),
		reflect.TypeOf(cgasSymN{}),
		reflect.TypeOf(cgasRareN{}),
		reflect.TypeOf(cgasParamN{}),
		reflect.TypeOf(cgasFieldN{}),
		reflect.TypeOf(cgasLocalN{}),
		reflect.TypeOf(int32(0)),
		reflect.TypeOf(uint32(0)),
	} {
		fail(!cgasHasGoRef(t), t.String()+" must be raw-castable")
	}
}

func cgasHasGoRef(t reflect.Type) bool {
	switch t.Kind() {
	case reflect.Pointer, reflect.Slice, reflect.Map,
		reflect.Chan, reflect.Func, reflect.Interface, reflect.UnsafePointer,
		reflect.Complex64, reflect.Complex128:
		return true
	case reflect.Array:
		return cgasHasGoRef(t.Elem())
	case reflect.Struct:
		for i := 0; i < t.NumField(); i++ {
			if cgasHasGoRef(t.Field(i).Type) {
				return true
			}
		}
	}
	return false
}

func cgPutU32(b []byte, o int, v uint32) { *(*uint32)(unsafe.Pointer(&b[o])) = v }
func cgPutU64(b []byte, o int, v uint64) { *(*uint64)(unsafe.Pointer(&b[o])) = v }

func cgGetU32(b []byte, o int) uint32 { return *(*uint32)(unsafe.Pointer(&b[o])) }
func cgGetU64(b []byte, o int) uint64 { return *(*uint64)(unsafe.Pointer(&b[o])) }

func knownFlag(n string) bool {
	switch n {
	case "module", "limit", "csv", "json", "save", "dump", "save-ast", "load-ast":
		return true
	case "list", "metrics", "report", "quiet", "no-tests", "include-generated",
		"include-vendored", "version", "deps", "force", "install-deps",
		"schema", "help", "h", "summary", "parse-errors", "selfcheck":
		return true
	}
	return false
}

func invalidArg(flag, val string) {
	usage()
	fmt.Fprintf(os.Stderr, "codegraph_c: error: argument %s: invalid int value: '%s'\n", flag, val)
	os.Exit(2)
}

func takesValue(n string) bool {
	switch n {
	case "module", "limit", "csv", "json", "save", "dump", "save-ast", "load-ast":
		return true
	}
	return false
}

func isNegNumber(a string) bool {
	if len(a) < 2 || a[0] != '-' {
		return false
	}
	digits, dot := 0, false
	for _, c := range a[1:] {
		switch {
		case c >= '0' && c <= '9':
			digits++
		case c == '.' && !dot:
			dot = true
		default:
			return false
		}
	}

	if !dot {
		return digits >= 1
	}
	last := a[len(a)-1]
	return last >= '0' && last <= '9'
}

func usage() {
	fmt.Printf(`usage: codegraph_c [options] [root] [which ...]

Parse a C tree into an in-memory graph and query it in one shot.
Target: %s

  root            directory to analyse (default: .)
  which           queries to run, by number or by name (default: all of them)

what to print
  --list          list the %d defect queries and exit
  --metrics       switch to the %d structural metrics (combine with --list,
                  --summary, --json, --csv or a selector)
  --summary       one line per query: rows found and the top row. Start here.
  --report        overview: size, call graph, how much of it to trust, the
                  biggest modules and the heaviest functions
  --parse-errors  what the parser could not read, and where
  --selfcheck     test the graph against invariants its own numbers must obey
                  (loop depth vs loop count, fan-in vs the edge table, member
                  offsets vs struct size ...). Exits 1 if any is violated: that
                  is a bug in this tool, not in the code it read.
  --schema        print the tables and columns of the graph and exit

narrowing
  --module PAT    only rows whose module matches PAT (%% is a wildcard)
  --limit N       at most N rows per query (default: all)
  --no-tests      leave test files out of the graph
  --include-generated, --include-vendored
                  keep files that are skipped by default

machine output
  --json Q        one query as a JSON array (Q is a number or a name)
  --csv Q         one query as CSV
  --dump PATH     write every table as text
  --save PATH     same as --dump; refuses to overwrite without --force
  --save-ast PATH write the parsed state; --load-ast PATH reads it back and
                  skips parsing. It takes no root: --load-ast s.ast dead-code

other
  --quiet         no progress on stderr
  --force         allow --save / --save-ast to overwrite
  --deps, --install-deps, --version, -h

Progress and warnings go to stderr; results go to stdout.

examples
  codegraph_c src --summary
  codegraph_c src --report
  codegraph_c src dead-code untrusted-frontier --limit 20
  codegraph_c src --metrics risk-ranked --limit 25 --module 'src%%'
  codegraph_c src --json lock-order-inversion
`, target, len(queries), len(metricsCat))
}

func fieldStrings(r []fval) []string {
	out := make([]string, len(r))
	for i, v := range r {
		switch v.kind {
		case fkNull:
			out[i] = ""
		case fkInt:
			out[i] = strconv.FormatInt(v.i, 10)
		case fkFloat:
			out[i] = strconv.FormatFloat(v.f, 'g', -1, 64)
		default:
			out[i] = v.s
		}
	}
	return out
}

func jsonValue(v fval) any {
	switch v.kind {
	case fkNull:
		return nil
	case fkInt:
		return v.i
	case fkFloat:
		return v.f
	default:
		return cgJSONString(v.s)
	}
}

type orderedKV struct {
	Key string
	Val any
}

type orderedObj []orderedKV

func (o orderedObj) MarshalJSON() ([]byte, error) {
	var b []byte
	b = append(b, '{')
	for i, kv := range o {
		if i > 0 {
			b = append(b, ',')
		}
		k, err := json.Marshal(kv.Key)
		if err != nil {
			return nil, err
		}
		b = append(b, k...)
		b = append(b, ':')
		v, err := json.Marshal(kv.Val)
		if err != nil {
			return nil, err
		}
		b = append(b, v...)
	}
	b = append(b, '}')
	return b, nil
}

type cgJSONString string

func (s cgJSONString) MarshalJSON() ([]byte, error) {
	return []byte(cgJSONQuote(string(s))), nil
}

func cgJSONQuote(s string) string {
	var b strings.Builder
	b.Grow(len(s) + 2)
	b.WriteByte('"')
	for _, r := range s {
		switch r {
		case '"':
			b.WriteString(`\"`)
		case '\\':
			b.WriteString(`\\`)
		case '\b':
			b.WriteString(`\b`)
		case '\f':
			b.WriteString(`\f`)
		case '\n':
			b.WriteString(`\n`)
		case '\r':
			b.WriteString(`\r`)
		case '\t':
			b.WriteString(`\t`)
		default:
			if r < 0x20 {
				fmt.Fprintf(&b, `\u%04x`, r)
			} else if r < 0x80 {
				b.WriteByte(byte(r))
			} else if r <= 0xFFFF {
				fmt.Fprintf(&b, `\u%04x`, r)
			} else {
				r -= 0x10000
				fmt.Fprintf(&b, `\u%04x\u%04x`, 0xD800+(r>>10), 0xDC00+(r&0x3FF))
			}
		}
	}
	b.WriteByte('"')
	return b.String()
}

const maxCell = 72

func cell(v fval) string {
	var t string
	switch v.kind {
	case fkNull:
		return "-"
	case fkFloat:
		t = fmt.Sprintf("%.2f", v.f)
	case fkInt:
		t = strconv.FormatInt(v.i, 10)
	default:
		t = v.s
	}
	if len(t) <= maxCell || utf8.RuneCountInString(t) <= maxCell {
		return t
	}
	return string([]rune(t)[:maxCell-3]) + "..."
}

func render(w *bufio.Writer, rows [][]fval, cols []string) {
	if len(rows) == 0 {
		fmt.Fprintln(w, " (no rows)")
		return
	}
	// one backing array for every cell, and the padding written straight to
	// the output: a slice, a padded copy and a joined line per row was most
	// of what printing a long result cost
	nc := len(cols)
	body := make([]string, 0, len(rows)*nc)
	widths := make([]int, nc)
	for i, c := range cols {
		widths[i] = utf8.RuneCountInString(c)
	}
	for _, r := range rows {
		for j, v := range r {
			c := cell(v)
			if n := utf8.RuneCountInString(c); j < nc && n > widths[j] {
				widths[j] = n
			}
			body = append(body, c)
		}
	}
	line := func(get func(i int) string, n int) {
		for i := 0; i < n; i++ {
			c := get(i)
			w.WriteByte(' ')
			w.WriteString(c)
			for k := widths[i] - utf8.RuneCountInString(c); k > 0; k-- {
				w.WriteByte(' ')
			}
		}
		w.WriteByte('\n')
	}
	line(func(i int) string { return cols[i] }, nc)
	line(func(i int) string { return strings.Repeat("-", widths[i]) }, nc)
	at := 0
	for _, r := range rows {
		row := body[at : at+len(r)]
		at += len(r)
		line(func(i int) string { return row[i] }, len(r))
	}
}

func pad(s string, w int) string {
	n := w - utf8.RuneCountInString(s)
	if n <= 0 {
		return s
	}
	return s + strings.Repeat(" ", n)
}

func goVersion() string { return "go1.27" }

var _ = sort.Ints

func splitNotes(s string) []string {
	if s == "" {
		return nil
	}
	out := strings.Split(s, "\n")
	for len(out) > 0 && out[len(out)-1] == "" {
		out = out[:len(out)-1]
	}
	return out
}
func cgasCastRows[T any](mem []byte, off, ln uint64, what string) ([]T, error) {
	if ln == 0 {
		return nil, nil
	}
	size := uint64(unsafe.Sizeof(*new(T)))
	if ln%size != 0 {
		return nil, fmt.Errorf("%s: section length %d is not a multiple of record size %d", what, ln, size)
	}
	if off < uint64(cgasHeaderSz) || off > uint64(len(mem)) || ln > uint64(len(mem))-off {
		return nil, fmt.Errorf("%s: section lies outside the file", what)
	}
	base := unsafe.Pointer(uintptr(unsafe.Pointer(&mem[0])) + uintptr(off))
	if a := uintptr(unsafe.Alignof(*new(T))); a > 1 && uintptr(base)%a != 0 {
		return nil, fmt.Errorf("%s: section is not %d-byte aligned", what, a)
	}
	return unsafe.Slice((*T)(base), int(ln/size)), nil
}

func cgasStrFields[T any]() []uintptr {
	var p *T
	switch any(p).(type) {
	case *File:
		return []uintptr{unsafe.Offsetof(File{}.path), unsafe.Offsetof(File{}.dir),
			unsafe.Offsetof(File{}.basename), unsafe.Offsetof(File{}.ext),
			unsafe.Offsetof(File{}.lang), unsafe.Offsetof(File{}.sha1)}
	case *Module:
		return []uintptr{unsafe.Offsetof(Module{}.name), unsafe.Offsetof(Module{}.kind)}
	case *Symbol:
		return []uintptr{unsafe.Offsetof(Symbol{}.name), unsafe.Offsetof(Symbol{}.qualName),
			unsafe.Offsetof(Symbol{}.kind), unsafe.Offsetof(Symbol{}.signature),
			unsafe.Offsetof(Symbol{}.returnType)}
	case *Param:
		return []uintptr{unsafe.Offsetof(Param{}.name), unsafe.Offsetof(Param{}.typ),
			unsafe.Offsetof(Param{}.def)}
	case *Field:
		return []uintptr{unsafe.Offsetof(Field{}.name), unsafe.Offsetof(Field{}.typ),
			unsafe.Offsetof(Field{}.vis)}
	case *Local:
		return []uintptr{unsafe.Offsetof(Local{}.name), unsafe.Offsetof(Local{}.typ)}
	case *Unresolved:
		return []uintptr{unsafe.Offsetof(Unresolved{}.name)}
	case *Import:
		return []uintptr{unsafe.Offsetof(Import{}.target), unsafe.Offsetof(Import{}.alias),
			unsafe.Offsetof(Import{}.kind)}
	case *Hazard:
		return []uintptr{unsafe.Offsetof(Hazard{}.pattern), unsafe.Offsetof(Hazard{}.category)}
	case *Attribute:
		return []uintptr{unsafe.Offsetof(Attribute{}.name), unsafe.Offsetof(Attribute{}.args)}
	case *Literal:
		return []uintptr{unsafe.Offsetof(Literal{}.kind), unsafe.Offsetof(Literal{}.value)}
	case *EnumMember:
		return []uintptr{unsafe.Offsetof(EnumMember{}.name), unsafe.Offsetof(EnumMember{}.value)}
	case *Marker:
		return []uintptr{unsafe.Offsetof(Marker{}.kind), unsafe.Offsetof(Marker{}.text)}
	case *Declaration:
		return []uintptr{unsafe.Offsetof(Declaration{}.name)}
	case *AddrTaken:
		return []uintptr{unsafe.Offsetof(AddrTaken{}.name), unsafe.Offsetof(AddrTaken{}.kind)}
	case *SecretCandidate:
		return []uintptr{unsafe.Offsetof(SecretCandidate{}.value)}
	case *AllocSite:
		return []uintptr{unsafe.Offsetof(AllocSite{}.fn), unsafe.Offsetof(AllocSite{}.sizeExpr)}
	case *Memop:
		return []uintptr{unsafe.Offsetof(Memop{}.fn), unsafe.Offsetof(Memop{}.dst),
			unsafe.Offsetof(Memop{}.src), unsafe.Offsetof(Memop{}.sizeArg),
			unsafe.Offsetof(Memop{}.sizeBuf), unsafe.Offsetof(Memop{}.dstTail)}
	case *Macro:
		return []uintptr{unsafe.Offsetof(Macro{}.body)}
	case *Global:
		return []uintptr{unsafe.Offsetof(Global{}.name), unsafe.Offsetof(Global{}.typ)}
	case *ConfigBlock:
		return []uintptr{unsafe.Offsetof(ConfigBlock{}.directive), unsafe.Offsetof(ConfigBlock{}.expr)}
	case *MakefileRule:
		return []uintptr{unsafe.Offsetof(MakefileRule{}.path), unsafe.Offsetof(MakefileRule{}.rule)}
	case *LockRow:
		return []uintptr{unsafe.Offsetof(LockRow{}.name)}
	case *EventOp:
		return []uintptr{unsafe.Offsetof(EventOp{}.family), unsafe.Offsetof(EventOp{}.fn),
			unsafe.Offsetof(EventOp{}.args)}
	case *APIUse:
		return []uintptr{unsafe.Offsetof(APIUse{}.ns), unsafe.Offsetof(APIUse{}.fn)}
	case *IncludeCycle:
		return []uintptr{unsafe.Offsetof(IncludeCycle{}.aPath), unsafe.Offsetof(IncludeCycle{}.bPath),
			unsafe.Offsetof(IncludeCycle{}.members)}
	case *cgMetaRow:
		return []uintptr{unsafe.Offsetof(cgMetaRow{}.K), unsafe.Offsetof(cgMetaRow{}.V)}
	case *cgasSymN:
		return []uintptr{unsafe.Offsetof(cgasSymN{}.Name), unsafe.Offsetof(cgasSymN{}.QualName),
			unsafe.Offsetof(cgasSymN{}.Kind), unsafe.Offsetof(cgasSymN{}.Signature),
			unsafe.Offsetof(cgasSymN{}.ReturnType)}
	case *cgasParamN:
		return []uintptr{unsafe.Offsetof(cgasParamN{}.Name), unsafe.Offsetof(cgasParamN{}.Type),
			unsafe.Offsetof(cgasParamN{}.DefaultValue)}
	case *cgasFieldN:
		return []uintptr{unsafe.Offsetof(cgasFieldN{}.Name), unsafe.Offsetof(cgasFieldN{}.Type),
			unsafe.Offsetof(cgasFieldN{}.Visibility)}
	case *cgasLocalN:
		return []uintptr{unsafe.Offsetof(cgasLocalN{}.Name), unsafe.Offsetof(cgasLocalN{}.Type)}
	}
	return nil
}

func cgasCheckStrRefs[T any](rows []T, arenaLen uint64, what string) error {
	if len(rows) == 0 {
		return nil
	}
	n := int(unsafe.Sizeof(rows[0]))
	base := unsafe.Pointer(&rows[0])
	for _, f := range cgasStrFields[T]() {
		for i := range rows {
			q := (*cgStr)(unsafe.Pointer(uintptr(base) + uintptr(i*n) + f))
			if q.Off > arenaLen || q.Ln > arenaLen-q.Off {
				return fmt.Errorf("%s: string reference (%d,%d) escapes the string arena (%d bytes)",
					what, q.Off, q.Ln, arenaLen)
			}
		}
	}
	return nil
}

func cgasCheckBools[T any](rows []T, offs []uintptr, what string) error {
	if len(rows) == 0 || len(offs) == 0 {
		return nil
	}
	n := int(unsafe.Sizeof(rows[0]))
	base := unsafe.Pointer(&rows[0])
	for i := range rows {
		for _, o := range offs {
			if b := *(*byte)(unsafe.Pointer(uintptr(base) + uintptr(i*n) + o)); b > 1 {
				return fmt.Errorf("%s: row %d carries boolean byte %d", what, i, b)
			}
		}
	}
	return nil
}

func cgasWriteProj[S any, N any](w *bufio.Writer, rows []S, fill func(d *N, s *S)) error {
	if len(rows) == 0 {
		return nil
	}
	chunk := make([]N, min(len(rows), 512))
	for start := 0; start < len(rows); start += len(chunk) {
		n := min(len(chunk), len(rows)-start)
		for i := 0; i < n; i++ {
			cgZeroRow(unsafe.Pointer(&chunk[i]), unsafe.Sizeof(chunk[i]))
			fill(&chunk[i], &rows[start+i])
		}
		if _, err := w.Write(cgasBytes(chunk[:n])); err != nil {
			return err
		}
	}
	return nil
}

func cgasWriteFiles(w *bufio.Writer, rows []File) error {
	if len(rows) == 0 {
		return nil
	}
	chunk := make([]File, min(len(rows), 512))
	po := unsafe.Offsetof(File{}.ParseMs)
	for start := 0; start < len(rows); start += len(chunk) {
		end := min(start+len(chunk), len(rows))
		n := copy(chunk, rows[start:end])
		for i := 0; i < n; i++ {
			*(*float64)(unsafe.Pointer(uintptr(unsafe.Pointer(&chunk[i])) + po)) = 0
		}
		if _, err := w.Write(cgasBytes(chunk[:n])); err != nil {
			return err
		}
	}
	return nil
}

func cgasFillSymN(d *cgasSymN, s *Symbol) {
	d.ID = s.ID
	d.FileID = s.FileID
	d.ModuleID = s.ModuleID
	d.Name = s.name
	d.QualName = s.qualName
	d.Kind = s.kind
	d.LineStart = s.LineStart
	d.Signature = s.signature
	d.ReturnType = s.returnType
	d.HasSignature = s.HasSignature
	d.HasReturnType = s.HasReturnType
	d.NParams = cgasI16(s.NParams, "symbol NParams")
	d.IsPublic = cgasFlag(s.IsPublic, "symbol IsPublic")
	d.IsStatic = cgasFlag(s.IsStatic, "symbol IsStatic")
	d.IsAbstract = cgasFlag(s.IsAbstract, "symbol IsAbstract")
	d.IsOverride = cgasFlag(s.IsOverride, "symbol IsOverride")
	d.IsTest = cgasFlag(s.IsTest, "symbol IsTest")
	d.IsEntrypoint = cgasFlag(s.IsEntrypoint, "symbol IsEntrypoint")
	d.IsGenerated = cgasFlag(s.IsGenerated, "symbol IsGenerated")
	d.Sloc = s.Sloc
	d.NCommentLines = s.NCommentLines
	d.HasDoc = cgasFlag(s.HasDoc, "symbol HasDoc")
	d.Cyclomatic = cgasI16(s.Cyclomatic, "symbol Cyclomatic")
	d.Cognitive = cgasI16(s.Cognitive, "symbol Cognitive")
	d.MaxNesting = cgasI16(s.MaxNesting, "symbol MaxNesting")
	d.NTokens = s.NTokens
	d.NOperators = s.NOperators
	d.NOperands = s.NOperands
	d.NDistinctOperators = cgasI16(s.NDistinctOperators, "symbol NDistinctOperators")
	d.NDistinctOperands = cgasI16(s.NDistinctOperands, "symbol NDistinctOperands")
	d.NLoops = cgasI16(s.NLoops, "symbol NLoops")
	d.NBranches = cgasI16(s.NBranches, "symbol NBranches")
	d.NReturns = cgasI16(s.NReturns, "symbol NReturns")
	d.NSwitch = cgasI16(s.NSwitch, "symbol NSwitch")
	d.NCases = cgasI16(s.NCases, "symbol NCases")
	d.NLabels = cgasI16(s.NLabels, "symbol NLabels")
	d.NGotos = cgasI16(s.NGotos, "symbol NGotos")
	d.MaxLoopDepth = cgasI16(s.MaxLoopDepth, "symbol MaxLoopDepth")
	d.CallInLoop = cgasI16(s.CallInLoop, "symbol CallInLoop")
	d.AllocInLoop = cgasI16(s.AllocInLoop, "symbol AllocInLoop")
	d.IOInLoop = cgasI16(s.IOInLoop, "symbol IOInLoop")
	d.LockInLoop = cgasI16(s.LockInLoop, "symbol LockInLoop")
	d.BranchInLoop = cgasI16(s.BranchInLoop, "symbol BranchInLoop")
	d.NLocals = cgasI16(s.NLocals, "symbol NLocals")
	d.NCmp = cgasI16(s.NCmp, "symbol NCmp")
	d.NArith = cgasI16(s.NArith, "symbol NArith")
	d.NShift = cgasI16(s.NShift, "symbol NShift")
	d.NFloatLit = cgasI16(s.NFloatLit, "symbol NFloatLit")
	d.NMagic = cgasI16(s.NMagic, "symbol NMagic")
	d.NNullCheck = cgasI16(s.NNullCheck, "symbol NNullCheck")
	d.NCalls = cgasI16(s.NCalls, "symbol NCalls")
	d.NDynamicCalls = cgasI16(s.NDynamicCalls, "symbol NDynamicCalls")
	d.NUnresolvedCalls = cgasI16(s.NUnresolvedCalls, "symbol NUnresolvedCalls")
	d.FanIn = cgasI16(s.FanIn, "symbol FanIn")
	d.FanOut = cgasI16(s.FanOut, "symbol FanOut")
	d.NCallsites = cgasI16(s.NCallsites, "symbol NCallsites")
	d.IsRecursive = cgasFlag(s.IsRecursive, "symbol IsRecursive")
	d.RiskScore = cgasI16(s.RiskScore, "symbol RiskScore")
	d.NMemory = cgasI16(s.NMemory, "symbol NMemory")
	d.NAlloc = cgasI16(s.NAlloc, "symbol NAlloc")
	d.NIO = cgasI16(s.NIO, "symbol NIO")
	d.NStdio = cgasI16(s.NStdio, "symbol NStdio")
	d.NExec = cgasI16(s.NExec, "symbol NExec")
	d.NLibm = cgasI16(s.NLibm, "symbol NLibm")
	d.NInteger = cgasI16(s.NInteger, "symbol NInteger")
	d.NConcurrency = cgasI16(s.NConcurrency, "symbol NConcurrency")
	d.NReentrancy = cgasI16(s.NReentrancy, "symbol NReentrancy")
	d.IsInline = cgasFlag(s.IsInline, "symbol IsInline")
	d.IsVariadic = cgasFlag(s.IsVariadic, "symbol IsVariadic")
	d.NPtrLocals = cgasI16(s.NPtrLocals, "symbol NPtrLocals")
	d.NDeref = cgasI16(s.NDeref, "symbol NDeref")
	d.NCast = cgasI16(s.NCast, "symbol NCast")
	d.NSizeof = cgasI16(s.NSizeof, "symbol NSizeof")
	d.NIntrinsic = cgasI16(s.NIntrinsic, "symbol NIntrinsic")
	d.NAtomic = cgasI16(s.NAtomic, "symbol NAtomic")
	d.NRestrict = cgasI16(s.NRestrict, "symbol NRestrict")
	d.NLikely = cgasI16(s.NLikely, "symbol NLikely")
	d.NBuiltin = cgasI16(s.NBuiltin, "symbol NBuiltin")
	d.Rare = s.rare
}

func cgasFillRareN(d *cgasRareN, s *symRare) {
	d.SwitchInLoop = cgasI16(s.SwitchInLoop, "rare row SwitchInLoop")
	d.LibmInLoop = cgasI16(s.LibmInLoop, "rare row LibmInLoop")
	d.DivInLoop = cgasI16(s.DivInLoop, "rare row DivInLoop")
	d.StrlenInLoop = cgasI16(s.StrlenInLoop, "rare row StrlenInLoop")
	d.RetNull = cgasI16(s.RetNull, "rare row RetNull")
	d.RetNeg = cgasI16(s.RetNeg, "rare row RetNeg")
	d.RetZero = cgasI16(s.RetZero, "rare row RetZero")
	d.RetVal = cgasI16(s.RetVal, "rare row RetVal")
	d.RetVoid = cgasI16(s.RetVoid, "rare row RetVoid")
	d.NFnptrCalls = cgasI16(s.NFnptrCalls, "rare row NFnptrCalls")
	d.NMacroCalls = cgasI16(s.NMacroCalls, "rare row NMacroCalls")
	d.NExternalCalls = cgasI16(s.NExternalCalls, "rare row NExternalCalls")
	d.NFree = cgasI16(s.NFree, "rare row NFree")
	d.NConstCast = cgasI16(s.NConstCast, "rare row NConstCast")
	d.NToctou = cgasI16(s.NToctou, "rare row NToctou")
	d.NLockAcquire = cgasI16(s.NLockAcquire, "rare row NLockAcquire")
	d.NLockRelease = cgasI16(s.NLockRelease, "rare row NLockRelease")
	d.NNarrowCast = cgasI16(s.NNarrowCast, "rare row NNarrowCast")
	d.NSignCmp = cgasI16(s.NSignCmp, "rare row NSignCmp")
	d.NVariadicFmt = cgasI16(s.NVariadicFmt, "rare row NVariadicFmt")
	d.NMemcpy = cgasI16(s.NMemcpy, "rare row NMemcpy")
	d.NAllocsite = cgasI16(s.NAllocsite, "rare row NAllocsite")
	d.NGlobalWrite = cgasI16(s.NGlobalWrite, "rare row NGlobalWrite")
	d.NErrno = cgasI16(s.NErrno, "rare row NErrno")
	d.NWeakRandom = cgasI16(s.NWeakRandom, "rare row NWeakRandom")
	d.NShiftVar = cgasI16(s.NShiftVar, "rare row NShiftVar")
	d.NReallocSelf = cgasI16(s.NReallocSelf, "rare row NReallocSelf")
	d.NVla = cgasI16(s.NVla, "rare row NVla")
	d.NGetenv = cgasI16(s.NGetenv, "rare row NGetenv")
	d.NAssertSide = cgasI16(s.NAssertSide, "rare row NAssertSide")
	d.NFreeThenUse = cgasI16(s.NFreeThenUse, "rare row NFreeThenUse")
	d.NEpoll = cgasI16(s.NEpoll, "rare row NEpoll")
	d.NUring = cgasI16(s.NUring, "rare row NUring")
	d.NKqueue = cgasI16(s.NKqueue, "rare row NKqueue")
	d.NEventWait = cgasI16(s.NEventWait, "rare row NEventWait")
	d.NEtReg = cgasI16(s.NEtReg, "rare row NEtReg")
	d.NOneshotReg = cgasI16(s.NOneshotReg, "rare row NOneshotReg")
	d.NWriteReady = cgasI16(s.NWriteReady, "rare row NWriteReady")
	d.NErrFlag = cgasI16(s.NErrFlag, "rare row NErrFlag")
	d.NRearm = cgasI16(s.NRearm, "rare row NRearm")
	d.NDereg = cgasI16(s.NDereg, "rare row NDereg")
	d.NEagain = cgasI16(s.NEagain, "rare row NEagain")
	d.NEintr = cgasI16(s.NEintr, "rare row NEintr")
	d.NUringRes = cgasI16(s.NUringRes, "rare row NUringRes")
	d.NUringRing = cgasI16(s.NUringRing, "rare row NUringRing")
	d.NUringBarrier = cgasI16(s.NUringBarrier, "rare row NUringBarrier")
	d.NUringSqpoll = cgasI16(s.NUringSqpoll, "rare row NUringSqpoll")
	d.NNonblockSet = cgasI16(s.NNonblockSet, "rare row NNonblockSet")
	d.NEventCreate = cgasI16(s.NEventCreate, "rare row NEventCreate")
	d.NEventDestroy = cgasI16(s.NEventDestroy, "rare row NEventDestroy")
	d.NUringSqe = cgasI16(s.NUringSqe, "rare row NUringSqe")
	d.NUringSeen = cgasI16(s.NUringSeen, "rare row NUringSeen")
	d.NUringUdata = cgasI16(s.NUringUdata, "rare row NUringUdata")
	d.NUringLink = cgasI16(s.NUringLink, "rare row NUringLink")
	d.NUringStreamOps = cgasI16(s.NUringStreamOps, "rare row NUringStreamOps")
	d.NUringTeardown = cgasI16(s.NUringTeardown, "rare row NUringTeardown")
	d.NEvTimeoutIndefinite = cgasI16(s.NEvTimeoutIndefinite, "rare row NEvTimeoutIndefinite")
	d.NEvTimeoutZero = cgasI16(s.NEvTimeoutZero, "rare row NEvTimeoutZero")
	d.NEvBatchOne = cgasI16(s.NEvBatchOne, "rare row NEvBatchOne")
	d.NKqTimer = cgasI16(s.NKqTimer, "rare row NKqTimer")
	d.NKqTimerZeroData = cgasI16(s.NKqTimerZeroData, "rare row NKqTimerZeroData")
	d.NKqReceipt = cgasI16(s.NKqReceipt, "rare row NKqReceipt")
	d.NRetNegCheck = cgasI16(s.NRetNegCheck, "rare row NRetNegCheck")
	d.NDomainGuard = cgasI16(s.NDomainGuard, "rare row NDomainGuard")
	d.NUcharCast = cgasI16(s.NUcharCast, "rare row NUcharCast")
	d.NErrnoZero = cgasI16(s.NErrnoZero, "rare row NErrnoZero")
	d.NEndptr = cgasI16(s.NEndptr, "rare row NEndptr")
	d.NVaEnd = cgasI16(s.NVaEnd, "rare row NVaEnd")
	d.NMapFailed = cgasI16(s.NMapFailed, "rare row NMapFailed")
	d.NMonotonicClock = cgasI16(s.NMonotonicClock, "rare row NMonotonicClock")
	d.NStackszArray = cgasI16(s.NStackszArray, "rare row NStackszArray")
	d.NPtrOvfCheck = cgasI16(s.NPtrOvfCheck, "rare row NPtrOvfCheck")
	d.NCallocTransposed = cgasI16(s.NCallocTransposed, "rare row NCallocTransposed")
	d.NVaArgArr = cgasI16(s.NVaArgArr, "rare row NVaArgArr")
}

func cgasFillParamN(d *cgasParamN, s *Param) {
	d.SymbolID = s.SymbolID
	d.Pos = cgasI16(s.Pos, "param Pos")
	d.Name = s.name
	d.HasName = s.HasName
	d.Type = s.typ
	d.DefaultValue = s.def
	d.HasDefaultValue = s.HasDefaultValue
	d.IsOptional = cgasFlag(s.IsOptional, "param IsOptional")
	d.IsVariadic = cgasFlag(s.IsVariadic, "param IsVariadic")
	d.IsRef = cgasFlag(s.IsRef, "param IsRef")
	d.IsMutable = cgasFlag(s.IsMutable, "param IsMutable")
	d.IsNullable = cgasFlag(s.IsNullable, "param IsNullable")
	d.IsGeneric = cgasFlag(s.IsGeneric, "param IsGeneric")
	d.IsUntyped = cgasFlag(s.IsUntyped, "param IsUntyped")
	d.TypeDepth = cgasI16(s.TypeDepth, "param TypeDepth")
}

func cgasFillFieldN(d *cgasFieldN, s *Field) {
	d.SymbolID = s.SymbolID
	d.Ordinal = cgasI16(s.Ordinal, "field Ordinal")
	d.Name = s.name
	d.Type = s.typ
	d.Visibility = s.vis
	d.Line = s.Line
	d.IsStatic = cgasFlag(s.IsStatic, "field IsStatic")
	d.IsConst = cgasFlag(s.IsConst, "field IsConst")
	d.IsMutable = cgasFlag(s.IsMutable, "field IsMutable")
	d.IsNullable = cgasFlag(s.IsNullable, "field IsNullable")
	d.IsCollection = cgasFlag(s.IsCollection, "field IsCollection")
	d.IsUntyped = cgasFlag(s.IsUntyped, "field IsUntyped")
	d.HasDefault = cgasFlag(s.HasDefault, "field HasDefault")
	d.TypeDepth = cgasI16(s.TypeDepth, "field TypeDepth")
}

func cgasFillLocalN(d *cgasLocalN, s *Local) {
	d.SymbolID = s.SymbolID
	d.Ordinal = cgasI16(s.Ordinal, "local Ordinal")
	d.Name = s.name
	d.Type = s.typ
	d.Line = s.Line
	d.IsConst = cgasFlag(s.IsConst, "local IsConst")
	d.IsMutable = cgasFlag(s.IsMutable, "local IsMutable")
	d.IsUntyped = cgasFlag(s.IsUntyped, "local IsUntyped")
	d.HasInit = cgasFlag(s.HasInit, "local HasInit")
	d.InLoop = cgasFlag(s.InLoop, "local InLoop")
	d.ScopeDepth = cgasU8(s.ScopeDepth, "local ScopeDepth")
}

func saveASTFile(g *Graph, path string) (int64, error) {
	cgasGuard()
	mods := g.Modules
	metaKeys := make([]string, 0, len(g.Meta))
	for k := range g.Meta {
		metaKeys = append(metaKeys, k)
	}
	sort.Strings(metaKeys)
	metaRows := make([]cgMetaRow, len(metaKeys))
	for i, k := range metaKeys {
		metaRows[i].K = cgPut(k)
		metaRows[i].V = cgPut(g.Meta[k])
	}
	var ctr cgasCounters
	ctr.FilesParsed = g.FilesParsed
	ctr.FilesFailed = g.FilesFailed
	ctr.EdgesN = g.EdgesN
	ctr.MacroN = g.MacroN
	ctr.ExternN = g.ExternN
	ctr.DeclN = g.DeclN
	ctr.UnresN = g.UnresN
	ctr.CallsTotal = g.CallsTotal
	ctr.AddrPruned = g.AddrPruned
	ctr.MakefilesRead = g.MakefilesRead
	ctr.FilesSkippedBig = g.FilesSkippedBig
	ctr.FilesSkippedSpecial = g.FilesSkippedSpecial
	ctr.FilesSkippedEscape = g.FilesSkippedEscape
	ctr.FilesSkippedDenied = g.FilesSkippedDenied
	ctr.WalkErrors = g.WalkErrors
	body := make([][]byte, cgasNSec)
	body[cgasSecMods-1] = cgasBytes(mods)
	body[cgasSecLits-1] = cgasBytes(g.Literals)
	body[cgasSecMark-1] = cgasBytes(g.Markers)
	body[cgasSecAttrs-1] = cgasBytes(g.Attrs)
	body[cgasSecImps-1] = cgasBytes(g.Imports)
	body[cgasSecHaz-1] = cgasBytes(g.Hazards)
	body[cgasSecEnums-1] = cgasBytes(g.EnumMem)
	body[cgasSecLayout-1] = cgasBytes(g.Layout)
	body[cgasSecSSize-1] = cgasBytes(g.SSize)
	body[cgasSecDecls-1] = cgasBytes(g.Decls)
	body[cgasSecAddr-1] = cgasBytes(g.Addr)
	body[cgasSecSecrets-1] = cgasBytes(g.Secrets)
	body[cgasSecAllocs-1] = cgasBytes(g.Allocs)
	body[cgasSecMemops-1] = cgasBytes(g.Memops)
	body[cgasSecMacros-1] = cgasBytes(g.Macros)
	body[cgasSecGlobals-1] = cgasBytes(g.Globals)
	body[cgasSecCfgs-1] = cgasBytes(g.Cfgs)
	body[cgasSecReach-1] = cgasBytes(g.Reach)
	body[cgasSecMkRules-1] = cgasBytes(g.MkRules)
	body[cgasSecLocks-1] = cgasBytes(g.Locks)
	body[cgasSecEvOps-1] = cgasBytes(g.EvOps)
	body[cgasSecAPIUses-1] = cgasBytes(g.APIUses)
	body[cgasSecCycles-1] = cgasBytes(g.Cycles)
	body[cgasSecEdges-1] = cgasBytes(g.Edges)
	body[cgasSecSites-1] = cgasBytes(g.Callsites)
	body[cgasSecUnres-1] = cgasBytes(g.Unres)
	body[cgasSecMeta-1] = cgasBytes(metaRows)
	body[cgasSecCounters-1] = cgasBytes([]cgasCounters{ctr})
	body[cgasSecStrings-1] = cgArena

	secLn := map[uint32]uint64{
		cgasSecFiles:  uint64(len(g.Files)) * uint64(unsafe.Sizeof(File{})),
		cgasSecSyms:   uint64(len(g.symStore)) * uint64(unsafe.Sizeof(cgasSymN{})),
		cgasSecRare:   uint64(len(g.rare)) * uint64(unsafe.Sizeof(cgasRareN{})),
		cgasSecParams: uint64(len(g.Params)) * uint64(unsafe.Sizeof(cgasParamN{})),
		cgasSecFields: uint64(len(g.Fields)) * uint64(unsafe.Sizeof(cgasFieldN{})),
		cgasSecLocals: uint64(len(g.Locals)) * uint64(unsafe.Sizeof(cgasLocalN{})),
	}
	dir := make([]cgasSec, cgasNSec)
	cur := uint64(cgasHeaderSz) + cgasNSec*cgasSecSz
	for i := range dir {
		cur = (cur + 7) &^ 7
		ln, ok := secLn[uint32(i+1)]
		if !ok {
			ln = uint64(len(body[i]))
		}
		dir[i] = cgasSec{uint32(i + 1), 0, cur, ln}
		cur += ln
	}
	total := cur

	f, err := os.Create(path)
	if err != nil {
		return 0, err
	}
	w := bufio.NewWriterSize(f, 1<<20)
	failW := func(err error) (int64, error) {
		f.Close()
		return 0, fmt.Errorf("write %s: %v", path, err)
	}
	hdr := make([]byte, cgasHeaderSz)
	copy(hdr[0:4], cgasMagic)
	cgPutU32(hdr, 4, cgasVersion)
	cgPutU32(hdr, 8, cgasNSec)
	cgPutU64(hdr, 16, total)
	cgPutU64(hdr, 24, uint64(len(cgArena)))
	if _, err := w.Write(hdr); err != nil {
		return failW(err)
	}
	dirBuf := make([]byte, cgasNSec*cgasSecSz)
	for i := range dir {
		o := i * int(cgasSecSz)
		cgPutU32(dirBuf, o, dir[i].ID)
		cgPutU64(dirBuf, o+8, dir[i].Off)
		cgPutU64(dirBuf, o+16, dir[i].Len)
	}
	if _, err := w.Write(dirBuf); err != nil {
		return failW(err)
	}
	var zero [8]byte
	pos := uint64(cgasHeaderSz) + cgasNSec*cgasSecSz
	for i := range dir {
		if dir[i].Off > pos {
			if _, err := w.Write(zero[:dir[i].Off-pos]); err != nil {
				return failW(err)
			}
			pos = dir[i].Off
		}
		var werr error
		switch uint32(i + 1) {
		case cgasSecFiles:
			werr = cgasWriteFiles(w, g.Files)
		case cgasSecSyms:
			werr = cgasWriteProj(w, g.symStore, cgasFillSymN)
		case cgasSecRare:
			werr = cgasWriteProj(w, g.rare, cgasFillRareN)
		case cgasSecParams:
			werr = cgasWriteProj(w, g.Params, cgasFillParamN)
		case cgasSecFields:
			werr = cgasWriteProj(w, g.Fields, cgasFillFieldN)
		case cgasSecLocals:
			werr = cgasWriteProj(w, g.Locals, cgasFillLocalN)
		default:
			if len(body[i]) > 0 {
				_, werr = w.Write(body[i])
			}
		}
		if werr != nil {
			return failW(werr)
		}
		pos += dir[i].Len
	}
	if err := w.Flush(); err != nil {
		return failW(err)
	}
	if err := f.Close(); err != nil {
		return 0, fmt.Errorf("write %s: %v", path, err)
	}
	return int64(total), nil
}

func loadAST(path string) (*Graph, error) {
	cgasGuard()
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil {
		return nil, err
	}
	if !st.Mode().IsRegular() {
		return nil, fmt.Errorf("%s: not a regular file", path)
	}
	if st.Size() < int64(cgasHeaderSz+cgasNSec*cgasSecSz) {
		return nil, fmt.Errorf("%s: too small (%d bytes) to be an AST state file", path, st.Size())
	}
	mem, err := syscall.Mmap(int(f.Fd()), 0, int(st.Size()),
		syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_PRIVATE)
	if err != nil {
		return nil, fmt.Errorf("mmap %s: %v", path, err)
	}
	bad := func(format string, args ...any) error {
		syscall.Munmap(mem)
		return fmt.Errorf("%s: "+format, append([]any{path}, args...)...)
	}
	if string(mem[0:4]) != cgasMagic {
		return nil, bad("bad magic %q, want %q -- not an AST state file", string(mem[0:4]), cgasMagic)
	}
	if v := cgGetU32(mem, 4); v != cgasVersion {
		return nil, bad("unsupported state format version %d -- CGAS v1 to v4 are retired, re-save with this build to produce v%d", v, cgasVersion)
	}
	secCount := int(cgGetU32(mem, 8))
	total := cgGetU64(mem, 16)
	arenaLen := cgGetU64(mem, 24)
	if secCount != cgasNSec {
		return nil, bad("section count %d, want %d", secCount, cgasNSec)
	}
	if total != uint64(st.Size()) {
		return nil, bad("truncated state file: header declares %d bytes, file has %d", total, st.Size())
	}
	dir := unsafe.Slice((*cgasSec)(unsafe.Pointer(&mem[cgasHeaderSz])), secCount)
	secs := make(map[uint32]cgasSec, secCount)
	for i := range dir {
		s := dir[i]
		if s.Off < uint64(cgasHeaderSz) || s.Off > total || s.Len > total-s.Off {
			return nil, bad("section %d lies outside the file", s.ID)
		}
		if _, dup := secs[s.ID]; dup {
			return nil, bad("duplicate section %d", s.ID)
		}
		secs[s.ID] = s
	}
	for id := uint32(1); id <= cgasNSec; id++ {
		if _, ok := secs[id]; !ok {
			return nil, bad("missing section %d", id)
		}
	}
	ao, al := secs[cgasSecStrings].Off, secs[cgasSecStrings].Len
	if al != arenaLen {
		return nil, bad("string arena length %d, header says %d", al, arenaLen)
	}
	cgMu.Lock()
	cgArena = mem[ao : ao+al : ao+al]
	cgMu.Unlock()
	dec := func(id uint32) (uint64, uint64) {
		return secs[id].Off, secs[id].Len
	}
	decErr := func(err error) (*Graph, error) {
		return nil, bad("%v", err)
	}

	off, ln := dec(cgasSecFiles)
	files, err := cgasCastRows[File](mem, off, ln, "files")
	if err != nil {
		return decErr(err)
	}
	if err := cgasCheckStrRefs(files, al, "files"); err != nil {
		return decErr(err)
	}
	off, ln = dec(cgasSecMods)
	mods, err := cgasCastRows[Module](mem, off, ln, "modules")
	if err != nil {
		return decErr(err)
	}
	if err := cgasCheckStrRefs(mods, al, "modules"); err != nil {
		return decErr(err)
	}
	off, ln = dec(cgasSecSyms)
	symNRows, err := cgasCastRows[cgasSymN](mem, off, ln, "symbols")
	if err != nil {
		return decErr(err)
	}
	if err := cgasCheckStrRefs(symNRows, al, "symbols"); err != nil {
		return decErr(err)
	}
	if err := cgasCheckBools(symNRows, symNBoolOffs, "symbols"); err != nil {
		return decErr(err)
	}
	symRows := make([]Symbol, len(symNRows))
	for i := range symNRows {
		r := &symNRows[i]
		d := &symRows[i]
		d.ID = r.ID
		d.FileID = r.FileID
		d.ModuleID = r.ModuleID
		d.name = r.Name
		d.qualName = r.QualName
		d.kind = r.Kind
		d.LineStart = r.LineStart
		d.signature = r.Signature
		d.returnType = r.ReturnType
		d.HasSignature = r.HasSignature
		d.HasReturnType = r.HasReturnType
		d.NParams = int32(r.NParams)
		d.IsPublic = int32(r.IsPublic)
		d.IsStatic = int32(r.IsStatic)
		d.IsAbstract = int32(r.IsAbstract)
		d.IsOverride = int32(r.IsOverride)
		d.IsTest = int32(r.IsTest)
		d.IsEntrypoint = int32(r.IsEntrypoint)
		d.IsGenerated = int32(r.IsGenerated)
		d.Sloc = r.Sloc
		d.NCommentLines = r.NCommentLines
		d.HasDoc = int32(r.HasDoc)
		d.Cyclomatic = int32(r.Cyclomatic)
		d.Cognitive = int32(r.Cognitive)
		d.MaxNesting = int32(r.MaxNesting)
		d.NTokens = r.NTokens
		d.NOperators = r.NOperators
		d.NOperands = r.NOperands
		d.NDistinctOperators = int32(r.NDistinctOperators)
		d.NDistinctOperands = int32(r.NDistinctOperands)
		d.NLoops = int32(r.NLoops)
		d.NBranches = int32(r.NBranches)
		d.NReturns = int32(r.NReturns)
		d.NSwitch = int32(r.NSwitch)
		d.NCases = int32(r.NCases)
		d.NLabels = int32(r.NLabels)
		d.NGotos = int32(r.NGotos)
		d.MaxLoopDepth = int32(r.MaxLoopDepth)
		d.CallInLoop = int32(r.CallInLoop)
		d.AllocInLoop = int32(r.AllocInLoop)
		d.IOInLoop = int32(r.IOInLoop)
		d.LockInLoop = int32(r.LockInLoop)
		d.BranchInLoop = int32(r.BranchInLoop)
		d.NLocals = int32(r.NLocals)
		d.NCmp = int32(r.NCmp)
		d.NArith = int32(r.NArith)
		d.NShift = int32(r.NShift)
		d.NFloatLit = int32(r.NFloatLit)
		d.NMagic = int32(r.NMagic)
		d.NNullCheck = int32(r.NNullCheck)
		d.NCalls = int32(r.NCalls)
		d.NDynamicCalls = int32(r.NDynamicCalls)
		d.NUnresolvedCalls = int32(r.NUnresolvedCalls)
		d.FanIn = int32(r.FanIn)
		d.FanOut = int32(r.FanOut)
		d.NCallsites = int32(r.NCallsites)
		d.IsRecursive = int32(r.IsRecursive)
		d.RiskScore = int32(r.RiskScore)
		d.NMemory = int32(r.NMemory)
		d.NAlloc = int32(r.NAlloc)
		d.NIO = int32(r.NIO)
		d.NStdio = int32(r.NStdio)
		d.NExec = int32(r.NExec)
		d.NLibm = int32(r.NLibm)
		d.NInteger = int32(r.NInteger)
		d.NConcurrency = int32(r.NConcurrency)
		d.NReentrancy = int32(r.NReentrancy)
		d.IsInline = int32(r.IsInline)
		d.IsVariadic = int32(r.IsVariadic)
		d.NPtrLocals = int32(r.NPtrLocals)
		d.NDeref = int32(r.NDeref)
		d.NCast = int32(r.NCast)
		d.NSizeof = int32(r.NSizeof)
		d.NIntrinsic = int32(r.NIntrinsic)
		d.NAtomic = int32(r.NAtomic)
		d.NRestrict = int32(r.NRestrict)
		d.NLikely = int32(r.NLikely)
		d.NBuiltin = int32(r.NBuiltin)
		d.rare = r.Rare
	}
	off, ln = dec(cgasSecRare)
	rareNRows, err := cgasCastRows[cgasRareN](mem, off, ln, "rare columns")
	if err != nil {
		return decErr(err)
	}
	rare := make([]symRare, len(rareNRows))
	for i := range rareNRows {
		r := &rareNRows[i]
		d := &rare[i]
		d.SwitchInLoop = int32(r.SwitchInLoop)
		d.LibmInLoop = int32(r.LibmInLoop)
		d.DivInLoop = int32(r.DivInLoop)
		d.StrlenInLoop = int32(r.StrlenInLoop)
		d.RetNull = int32(r.RetNull)
		d.RetNeg = int32(r.RetNeg)
		d.RetZero = int32(r.RetZero)
		d.RetVal = int32(r.RetVal)
		d.RetVoid = int32(r.RetVoid)
		d.NFnptrCalls = int32(r.NFnptrCalls)
		d.NMacroCalls = int32(r.NMacroCalls)
		d.NExternalCalls = int32(r.NExternalCalls)
		d.NFree = int32(r.NFree)
		d.NConstCast = int32(r.NConstCast)
		d.NToctou = int32(r.NToctou)
		d.NLockAcquire = int32(r.NLockAcquire)
		d.NLockRelease = int32(r.NLockRelease)
		d.NNarrowCast = int32(r.NNarrowCast)
		d.NSignCmp = int32(r.NSignCmp)
		d.NVariadicFmt = int32(r.NVariadicFmt)
		d.NMemcpy = int32(r.NMemcpy)
		d.NAllocsite = int32(r.NAllocsite)
		d.NGlobalWrite = int32(r.NGlobalWrite)
		d.NErrno = int32(r.NErrno)
		d.NWeakRandom = int32(r.NWeakRandom)
		d.NShiftVar = int32(r.NShiftVar)
		d.NReallocSelf = int32(r.NReallocSelf)
		d.NVla = int32(r.NVla)
		d.NGetenv = int32(r.NGetenv)
		d.NAssertSide = int32(r.NAssertSide)
		d.NFreeThenUse = int32(r.NFreeThenUse)
		d.NEpoll = int32(r.NEpoll)
		d.NUring = int32(r.NUring)
		d.NKqueue = int32(r.NKqueue)
		d.NEventWait = int32(r.NEventWait)
		d.NEtReg = int32(r.NEtReg)
		d.NOneshotReg = int32(r.NOneshotReg)
		d.NWriteReady = int32(r.NWriteReady)
		d.NErrFlag = int32(r.NErrFlag)
		d.NRearm = int32(r.NRearm)
		d.NDereg = int32(r.NDereg)
		d.NEagain = int32(r.NEagain)
		d.NEintr = int32(r.NEintr)
		d.NUringRes = int32(r.NUringRes)
		d.NUringRing = int32(r.NUringRing)
		d.NUringBarrier = int32(r.NUringBarrier)
		d.NUringSqpoll = int32(r.NUringSqpoll)
		d.NNonblockSet = int32(r.NNonblockSet)
		d.NEventCreate = int32(r.NEventCreate)
		d.NEventDestroy = int32(r.NEventDestroy)
		d.NUringSqe = int32(r.NUringSqe)
		d.NUringSeen = int32(r.NUringSeen)
		d.NUringUdata = int32(r.NUringUdata)
		d.NUringLink = int32(r.NUringLink)
		d.NUringStreamOps = int32(r.NUringStreamOps)
		d.NUringTeardown = int32(r.NUringTeardown)
		d.NEvTimeoutIndefinite = int32(r.NEvTimeoutIndefinite)
		d.NEvTimeoutZero = int32(r.NEvTimeoutZero)
		d.NEvBatchOne = int32(r.NEvBatchOne)
		d.NKqTimer = int32(r.NKqTimer)
		d.NKqTimerZeroData = int32(r.NKqTimerZeroData)
		d.NKqReceipt = int32(r.NKqReceipt)
		d.NRetNegCheck = int32(r.NRetNegCheck)
		d.NDomainGuard = int32(r.NDomainGuard)
		d.NUcharCast = int32(r.NUcharCast)
		d.NErrnoZero = int32(r.NErrnoZero)
		d.NEndptr = int32(r.NEndptr)
		d.NVaEnd = int32(r.NVaEnd)
		d.NMapFailed = int32(r.NMapFailed)
		d.NMonotonicClock = int32(r.NMonotonicClock)
		d.NStackszArray = int32(r.NStackszArray)
		d.NPtrOvfCheck = int32(r.NPtrOvfCheck)
		d.NCallocTransposed = int32(r.NCallocTransposed)
		d.NVaArgArr = int32(r.NVaArgArr)
	}
	for i := range symRows {
		if r := symRows[i].rare; r >= int32(len(rare)) {
			return nil, bad("symbol %d names rare column %d of %d", symRows[i].ID, r, len(rare))
		}
	}
	off, ln = dec(cgasSecParams)
	paramNRows, err := cgasCastRows[cgasParamN](mem, off, ln, "params")
	if err != nil {
		return decErr(err)
	}
	if err := cgasCheckStrRefs(paramNRows, al, "params"); err != nil {
		return decErr(err)
	}
	if err := cgasCheckBools(paramNRows, paramNBoolOffs, "params"); err != nil {
		return decErr(err)
	}
	params := make([]Param, len(paramNRows))
	for i := range paramNRows {
		r := &paramNRows[i]
		d := &params[i]
		d.SymbolID = r.SymbolID
		d.Pos = int32(r.Pos)
		d.name = r.Name
		d.HasName = r.HasName
		d.typ = r.Type
		d.def = r.DefaultValue
		d.HasDefaultValue = r.HasDefaultValue
		d.IsOptional = int32(r.IsOptional)
		d.IsVariadic = int32(r.IsVariadic)
		d.IsRef = int32(r.IsRef)
		d.IsMutable = int32(r.IsMutable)
		d.IsNullable = int32(r.IsNullable)
		d.IsGeneric = int32(r.IsGeneric)
		d.IsUntyped = int32(r.IsUntyped)
		d.TypeDepth = int32(r.TypeDepth)
	}
	off, ln = dec(cgasSecFields)
	fieldNRows, err := cgasCastRows[cgasFieldN](mem, off, ln, "fields")
	if err != nil {
		return decErr(err)
	}
	if err := cgasCheckStrRefs(fieldNRows, al, "fields"); err != nil {
		return decErr(err)
	}
	fields := make([]Field, len(fieldNRows))
	for i := range fieldNRows {
		r := &fieldNRows[i]
		d := &fields[i]
		d.SymbolID = r.SymbolID
		d.Ordinal = int32(r.Ordinal)
		d.name = r.Name
		d.typ = r.Type
		d.vis = r.Visibility
		d.Line = r.Line
		d.IsStatic = int32(r.IsStatic)
		d.IsConst = int32(r.IsConst)
		d.IsMutable = int32(r.IsMutable)
		d.IsNullable = int32(r.IsNullable)
		d.IsCollection = int32(r.IsCollection)
		d.IsUntyped = int32(r.IsUntyped)
		d.HasDefault = int32(r.HasDefault)
		d.TypeDepth = int32(r.TypeDepth)
	}
	off, ln = dec(cgasSecLocals)
	localNRows, err := cgasCastRows[cgasLocalN](mem, off, ln, "locals")
	if err != nil {
		return decErr(err)
	}
	if err := cgasCheckStrRefs(localNRows, al, "locals"); err != nil {
		return decErr(err)
	}
	locals := make([]Local, len(localNRows))
	for i := range localNRows {
		r := &localNRows[i]
		d := &locals[i]
		d.SymbolID = r.SymbolID
		d.Ordinal = int32(r.Ordinal)
		d.name = r.Name
		d.typ = r.Type
		d.Line = r.Line
		d.IsConst = int32(r.IsConst)
		d.IsMutable = int32(r.IsMutable)
		d.IsUntyped = int32(r.IsUntyped)
		d.HasInit = int32(r.HasInit)
		d.InLoop = int32(r.InLoop)
		d.ScopeDepth = int32(r.ScopeDepth)
	}
	off, ln = dec(cgasSecLits)
	lits, err := cgasCastRows[Literal](mem, off, ln, "literals")
	if err != nil {
		return decErr(err)
	}
	if err := cgasCheckStrRefs(lits, al, "literals"); err != nil {
		return decErr(err)
	}
	if err := cgasCheckBools(lits, literalBoolOffs, "literals"); err != nil {
		return decErr(err)
	}
	off, ln = dec(cgasSecMark)
	mark, err := cgasCastRows[Marker](mem, off, ln, "markers")
	if err != nil {
		return decErr(err)
	}
	if err := cgasCheckStrRefs(mark, al, "markers"); err != nil {
		return decErr(err)
	}
	off, ln = dec(cgasSecAttrs)
	attrs, err := cgasCastRows[Attribute](mem, off, ln, "attributes")
	if err != nil {
		return decErr(err)
	}
	if err := cgasCheckStrRefs(attrs, al, "attributes"); err != nil {
		return decErr(err)
	}
	if err := cgasCheckBools(attrs, attrBoolOffs, "attributes"); err != nil {
		return decErr(err)
	}
	off, ln = dec(cgasSecImps)
	imps, err := cgasCastRows[Import](mem, off, ln, "imports")
	if err != nil {
		return decErr(err)
	}
	if err := cgasCheckStrRefs(imps, al, "imports"); err != nil {
		return decErr(err)
	}
	if err := cgasCheckBools(imps, importBoolOffs, "imports"); err != nil {
		return decErr(err)
	}
	off, ln = dec(cgasSecHaz)
	haz, err := cgasCastRows[Hazard](mem, off, ln, "hazards")
	if err != nil {
		return decErr(err)
	}
	if err := cgasCheckStrRefs(haz, al, "hazards"); err != nil {
		return decErr(err)
	}
	off, ln = dec(cgasSecEnums)
	enums, err := cgasCastRows[EnumMember](mem, off, ln, "enum members")
	if err != nil {
		return decErr(err)
	}
	if err := cgasCheckStrRefs(enums, al, "enum members"); err != nil {
		return decErr(err)
	}
	if err := cgasCheckBools(enums, enumBoolOffs, "enum members"); err != nil {
		return decErr(err)
	}
	off, ln = dec(cgasSecLayout)
	layout, err := cgasCastRows[LayoutRow](mem, off, ln, "layout rows")
	if err != nil {
		return decErr(err)
	}
	off, ln = dec(cgasSecSSize)
	ssize, err := cgasCastRows[StructSize](mem, off, ln, "struct sizes")
	if err != nil {
		return decErr(err)
	}
	off, ln = dec(cgasSecDecls)
	decls, err := cgasCastRows[Declaration](mem, off, ln, "declarations")
	if err != nil {
		return decErr(err)
	}
	if err := cgasCheckStrRefs(decls, al, "declarations"); err != nil {
		return decErr(err)
	}
	off, ln = dec(cgasSecAddr)
	addr, err := cgasCastRows[AddrTaken](mem, off, ln, "addr-taken rows")
	if err != nil {
		return decErr(err)
	}
	if err := cgasCheckStrRefs(addr, al, "addr-taken rows"); err != nil {
		return decErr(err)
	}
	if err := cgasCheckBools(addr, addrBoolOffs, "addr-taken rows"); err != nil {
		return decErr(err)
	}
	off, ln = dec(cgasSecSecrets)
	secrets, err := cgasCastRows[SecretCandidate](mem, off, ln, "secret candidates")
	if err != nil {
		return decErr(err)
	}
	if err := cgasCheckStrRefs(secrets, al, "secret candidates"); err != nil {
		return decErr(err)
	}
	if err := cgasCheckBools(secrets, secretBoolOffs, "secret candidates"); err != nil {
		return decErr(err)
	}
	off, ln = dec(cgasSecAllocs)
	allocs, err := cgasCastRows[AllocSite](mem, off, ln, "allocation sites")
	if err != nil {
		return decErr(err)
	}
	if err := cgasCheckStrRefs(allocs, al, "allocation sites"); err != nil {
		return decErr(err)
	}
	off, ln = dec(cgasSecMemops)
	memops, err := cgasCastRows[Memop](mem, off, ln, "memops")
	if err != nil {
		return decErr(err)
	}
	if err := cgasCheckStrRefs(memops, al, "memops"); err != nil {
		return decErr(err)
	}
	off, ln = dec(cgasSecMacros)
	macros, err := cgasCastRows[Macro](mem, off, ln, "macros")
	if err != nil {
		return decErr(err)
	}
	if err := cgasCheckStrRefs(macros, al, "macros"); err != nil {
		return decErr(err)
	}
	if err := cgasCheckBools(macros, macroBoolOffs, "macros"); err != nil {
		return decErr(err)
	}
	off, ln = dec(cgasSecGlobals)
	globals, err := cgasCastRows[Global](mem, off, ln, "globals")
	if err != nil {
		return decErr(err)
	}
	if err := cgasCheckStrRefs(globals, al, "globals"); err != nil {
		return decErr(err)
	}
	off, ln = dec(cgasSecCfgs)
	cfgs, err := cgasCastRows[ConfigBlock](mem, off, ln, "config blocks")
	if err != nil {
		return decErr(err)
	}
	if err := cgasCheckStrRefs(cfgs, al, "config blocks"); err != nil {
		return decErr(err)
	}
	off, ln = dec(cgasSecReach)
	reach, err := cgasCastRows[Reach](mem, off, ln, "reach rows")
	if err != nil {
		return decErr(err)
	}
	off, ln = dec(cgasSecMkRules)
	mkRules, err := cgasCastRows[MakefileRule](mem, off, ln, "makefile rules")
	if err != nil {
		return decErr(err)
	}
	if err := cgasCheckStrRefs(mkRules, al, "makefile rules"); err != nil {
		return decErr(err)
	}
	off, ln = dec(cgasSecLocks)
	locks, err := cgasCastRows[LockRow](mem, off, ln, "lock rows")
	if err != nil {
		return decErr(err)
	}
	if err := cgasCheckStrRefs(locks, al, "lock rows"); err != nil {
		return decErr(err)
	}
	off, ln = dec(cgasSecEvOps)
	evOps, err := cgasCastRows[EventOp](mem, off, ln, "event ops")
	if err != nil {
		return decErr(err)
	}
	if err := cgasCheckStrRefs(evOps, al, "event ops"); err != nil {
		return decErr(err)
	}
	off, ln = dec(cgasSecAPIUses)
	apiUses, err := cgasCastRows[APIUse](mem, off, ln, "api uses")
	if err != nil {
		return decErr(err)
	}
	if err := cgasCheckStrRefs(apiUses, al, "api uses"); err != nil {
		return decErr(err)
	}
	off, ln = dec(cgasSecCycles)
	cycles, err := cgasCastRows[IncludeCycle](mem, off, ln, "include cycles")
	if err != nil {
		return decErr(err)
	}
	if err := cgasCheckStrRefs(cycles, al, "include cycles"); err != nil {
		return decErr(err)
	}
	off, ln = dec(cgasSecEdges)
	edges, err := cgasCastRows[Edge](mem, off, ln, "call-graph edges")
	if err != nil {
		return decErr(err)
	}
	off, ln = dec(cgasSecSites)
	sites, err := cgasCastRows[Callsite](mem, off, ln, "call sites")
	if err != nil {
		return decErr(err)
	}
	off, ln = dec(cgasSecUnres)
	unres, err := cgasCastRows[Unresolved](mem, off, ln, "unresolved calls")
	if err != nil {
		return decErr(err)
	}
	if err := cgasCheckStrRefs(unres, al, "unresolved calls"); err != nil {
		return decErr(err)
	}
	off, ln = dec(cgasSecMeta)
	meta, err := cgasCastRows[cgMetaRow](mem, off, ln, "meta")
	if err != nil {
		return decErr(err)
	}
	if err := cgasCheckStrRefs(meta, al, "meta"); err != nil {
		return decErr(err)
	}
	cgMu.Lock()
	if cgIntern == nil {
		cgIntern = make(map[string]cgStr, len(meta)*2)
	}
	for i := range meta {
		cgIntern[meta[i].Key()] = meta[i].K
		cgIntern[meta[i].Val()] = meta[i].V
	}
	cgMu.Unlock()
	off, ln = dec(cgasSecCounters)
	ctrs, err := cgasCastRows[cgasCounters](mem, off, ln, "counters")
	if err != nil {
		return decErr(err)
	}
	if len(ctrs) != 1 {
		return nil, bad("counters section holds %d rows, want 1", len(ctrs))
	}

	g := &Graph{astBlob: mem}
	g.Files = files
	g.Modules = mods
	g.symStore = symRows
	g.Symbols = make([]*Symbol, len(symRows))
	for i := range symRows {
		g.Symbols[i] = &g.symStore[i]
	}
	g.rare = rare
	g.Params = params
	g.Fields = fields
	g.Locals = locals
	g.Literals = lits
	g.Markers = mark
	g.Attrs = attrs
	g.Imports = imps
	g.Hazards = haz
	g.EnumMem = enums
	g.Layout = layout
	g.SSize = ssize
	g.Decls = decls
	g.Addr = addr
	g.Secrets = secrets
	g.Allocs = allocs
	g.Memops = memops
	g.Macros = macros
	g.Globals = globals
	g.Cfgs = cfgs
	g.Reach = reach
	g.MkRules = mkRules
	g.Locks = locks
	g.EvOps = evOps
	g.APIUses = apiUses
	g.Cycles = cycles
	g.Edges = edges
	g.Callsites = sites
	g.Unres = unres
	g.Meta = make(map[string]string, len(meta))
	for i := range meta {
		g.Meta[meta[i].Key()] = meta[i].Val()
	}
	g.FilesParsed = ctrs[0].FilesParsed
	g.FilesFailed = ctrs[0].FilesFailed
	g.EdgesN = ctrs[0].EdgesN
	g.MacroN = ctrs[0].MacroN
	g.ExternN = ctrs[0].ExternN
	g.DeclN = ctrs[0].DeclN
	g.UnresN = ctrs[0].UnresN
	g.CallsTotal = ctrs[0].CallsTotal
	g.AddrPruned = ctrs[0].AddrPruned
	g.MakefilesRead = ctrs[0].MakefilesRead
	g.FilesSkippedBig = ctrs[0].FilesSkippedBig
	g.FilesSkippedSpecial = ctrs[0].FilesSkippedSpecial
	g.FilesSkippedEscape = ctrs[0].FilesSkippedEscape
	g.FilesSkippedDenied = ctrs[0].FilesSkippedDenied
	g.WalkErrors = ctrs[0].WalkErrors
	g.fileByRel = make(map[string]int32, len(g.Files))
	for i := range g.Files {
		g.fileByRel[g.Files[i].Path()] = g.Files[i].ID
	}
	buildIndex(g)
	return g, nil
}

type FileRow struct {
	ID           int32   `json:"ID"`
	Path         string  `json:"Path"`
	Dir          string  `json:"Dir"`
	Basename     string  `json:"Basename"`
	Ext          string  `json:"Ext"`
	Lang         string  `json:"Lang"`
	ModuleID     int32   `json:"ModuleID"`
	Bytes        int32   `json:"Bytes"`
	Lines        int32   `json:"Lines"`
	Sloc         int32   `json:"Sloc"`
	BlankLines   int32   `json:"BlankLines"`
	CommentLines int32   `json:"CommentLines"`
	DocLines     int32   `json:"DocLines"`
	MaxLineLen   int32   `json:"MaxLineLen"`
	Sha1         string  `json:"Sha1"`
	Parsed       int32   `json:"Parsed"`
	IsTest       int32   `json:"IsTest"`
	IsGenerated  int32   `json:"IsGenerated"`
	IsVendored   int32   `json:"IsVendored"`
	NParseErrors int32   `json:"NParseErrors"`
	NMissing     int32   `json:"NMissingNodes"`
	ParseMs      float64 `json:"ParseMs"`

	NSymbols   int32 `json:"NSymbols"`
	NFunctions int32 `json:"NFunctions"`
	NTypes     int32 `json:"NTypes"`
	NImports   int32 `json:"NImports"`
	TotalCyclo int32 `json:"TotalCyclo"`
	MaxCyclo   int32 `json:"MaxCyclo"`
	TotalRisk  int32 `json:"TotalRisk"`
}

type ModuleRow struct {
	ID          int32   `json:"ID"`
	Name        string  `json:"Name"`
	Kind        string  `json:"Kind"`
	NFiles      int32   `json:"NFiles"`
	NSymbols    int32   `json:"NSymbols"`
	NPublic     int32   `json:"NPublic"`
	Sloc        int32   `json:"Sloc"`
	FanIn       int32   `json:"FanIn"`
	FanOut      int32   `json:"FanOut"`
	Instability float64 `json:"Instability"`
}

type SymbolRare struct {
	SwitchInLoop, LibmInLoop, DivInLoop         int32
	StrlenInLoop                                int32
	RetNull, RetNeg, RetZero, RetVal, RetVoid   int32
	NFnptrCalls, NMacroCalls                    int32
	NExternalCalls                              int32
	NFree, NConstCast, NToctou                  int32
	NLockAcquire, NLockRelease                  int32
	NNarrowCast, NSignCmp, NVariadicFmt         int32
	NMemcpy, NAllocsite                         int32
	NGlobalWrite                                int32
	NErrno, NWeakRandom, NShiftVar              int32
	NReallocSelf, NVla, NGetenv                 int32
	NAssertSide                                 int32
	NFreeThenUse                                int32
	NEpoll, NUring, NKqueue, NEventWait         int32
	NEtReg, NOneshotReg, NWriteReady            int32
	NErrFlag, NRearm, NDereg, NEagain, NEintr   int32
	NUringRes, NUringRing, NUringBarrier        int32
	NUringSqpoll                                int32
	NNonblockSet, NEventCreate, NEventDestroy   int32
	NUringSqe, NUringSeen, NUringUdata          int32
	NUringLink, NUringStreamOps, NUringTeardown int32
	NEvTimeoutIndefinite, NEvTimeoutZero        int32
	NEvBatchOne, NKqTimer, NKqTimerZeroData     int32
	NKqReceipt                                  int32
	NRetNegCheck, NDomainGuard, NUcharCast      int32
	NErrnoZero, NEndptr, NVaEnd, NMapFailed     int32
	NMonotonicClock, NStackszArray              int32
	NPtrOvfCheck, NCallocTransposed, NVaArgArr  int32
}

type SymbolRow struct {
	ID            int32
	FileID        int32
	ModuleID      int32
	Name          string
	QualName      string
	Kind          string
	LineStart     int32
	Signature     string
	ReturnType    string
	HasSignature  bool
	HasReturnType bool

	NParams int32

	IsPublic, IsStatic     int32
	IsAbstract, IsOverride int32
	IsTest, IsEntrypoint   int32
	IsGenerated            int32

	Sloc, NCommentLines, HasDoc int32

	Cyclomatic, Cognitive, MaxNesting     int32
	NTokens, NOperators, NOperands        int32
	NDistinctOperators, NDistinctOperands int32

	NLoops, NBranches, NReturns int32
	NSwitch, NCases             int32
	NLabels, NGotos             int32

	MaxLoopDepth, CallInLoop, AllocInLoop, IOInLoop int32
	LockInLoop, BranchInLoop                        int32

	NLocals, NCmp, NArith int32
	NShift                int32
	NFloatLit, NMagic     int32
	NNullCheck            int32

	NCalls, NDynamicCalls, NUnresolvedCalls int32
	FanIn, FanOut, NCallsites               int32
	IsRecursive                             int32

	RiskScore    int32
	NMemory      int32
	NAlloc       int32
	NIO          int32
	NStdio       int32
	NExec        int32
	NLibm        int32
	NInteger     int32
	NConcurrency int32
	NReentrancy  int32

	IsInline, IsVariadic               int32
	NPtrLocals, NDeref, NCast, NSizeof int32
	NIntrinsic, NAtomic, NRestrict     int32
	NLikely, NBuiltin                  int32

	LineEnd         int32
	NLines          int32
	Visibility      string
	NPtrParams      int32
	NAddrof         int32
	NArrow          int32
	NMemberAcc      int32
	NSubscript      int32
	NBitop          int32
	NStringLit      int32
	NBodyBytes      int32
	NTernary        int32
	NLogical        int32
	NVolatile       int32
	NStaticAssert   int32
	NAssign         int32
	NCompoundAssign int32
	NIncdec         int32

	SymbolRare
}

type ParamRow struct {
	SymbolID, Pos   int32
	Name            string
	HasName         bool
	Type            string
	DefaultValue    string
	HasDefaultValue bool
	IsOptional      int32
	IsVariadic      int32
	IsRef           int32
	IsMutable       int32
	IsNullable      int32
	IsGeneric       int32
	IsUntyped       int32
	TypeDepth       int32
}

type FieldRow struct {
	SymbolID, Ordinal     int32
	Name, Type            string
	Visibility            string
	Line                  int32
	IsStatic, IsConst     int32
	IsMutable, IsNullable int32
	IsCollection          int32
	IsUntyped             int32
	HasDefault            int32
	TypeDepth             int32
}

type LocalRow struct {
	SymbolID, Ordinal  int32
	Name, Type         string
	Line               int32
	IsConst, IsMutable int32
	IsUntyped          int32
	HasInit, InLoop    int32
	ScopeDepth         int32
}

type EdgeRow struct {
	CallerID, CalleeID           int32
	NCalls, SameFile, SameModule int32
	IsSelf                       int32
}

type CallsiteRow struct{ CallerID, CalleeID, Line int32 }

type UnresolvedRow struct {
	CallerID  int32
	Name      string
	N         int32
	FirstLine int32
}

type ImportRow struct {
	ID                     int32
	FileID                 int32
	Target                 string
	TargetID               int32
	HasTargetID            bool
	Alias                  string
	HasAlias               bool
	Kind                   string
	Line                   int32
	IsExternal, IsRelative int32
	IsWildcard, IsTypeOnly int32
	IsDynamic, NNames      int32
}

type HazardRow struct {
	SymbolID  int32
	Pattern   string
	Category  string
	N         int32
	FirstLine int32
}

type AttributeRow struct {
	ID          int32
	SymbolID    int32
	HasSymbolID bool
	FileID      int32
	Name        string
	Args        string
	HasArgs     bool
	Line        int32
}

type LiteralRow struct {
	ID       int32
	SymbolID int32
	HasSym   bool
	FileID   int32
	Kind     string
	Value    string
	Line     int32
	IsMagic  int32
}

type EnumMemberRow struct {
	SymbolID, Ordinal int32
	Name              string
	Value             string
	HasValue          bool
	NFields           int32
}

type MarkerRow struct {
	ID       int32
	FileID   int32
	SymbolID int32
	HasSym   bool
	Kind     string
	Line     int32
	Text     string
}

type LayoutRowR struct {
	SymbolID, Ordinal  int32
	ByteOff, ByteSize  int32
	PadBefore, Exact   int32
	PtrDepth, ArrayLen int32
	IsFnptr, Depth     int32
	InUnion            int32
}

type StructSizeRow struct {
	SymbolID, TotalSize, TailPad, TotalPad int32
	MaxAlign, Exact, NLines64              int32
}

type DeclarationRow struct {
	ID     int32
	FileID int32
	Name   string
	Line   int32
}

type AddrTakenRow struct {
	ID       int32
	SymbolID int32
	HasSym   bool
	FileID   int32
	Name     string
	Line     int32
	Kind     string
}

type SecretCandidateRow struct {
	ID       int32
	SymbolID int32
	HasSym   bool
	FileID   int32
	Value    string
	Line     int32
}

type AllocSiteRow struct {
	ID       int32
	SymbolID int32
	FileID   int32
	Fn       string
	SizeExpr string
	Line     int32
}

type MemopRow struct {
	ID               int32
	SymbolID         int32
	FileID           int32
	Fn               string
	Dst, Src         string
	SizeArg          string
	SizeBuf, DstTail string
	Line             int32
}

type MacroRow struct {
	SymbolID       int32
	IsFunctionlike int32
	NParams        int32
	Body           string
	HasBody        bool
	BodyLen        int32
	IsMultiline    int32
	NUses          int32
}

type GlobalRow struct {
	ID         int32
	FileID     int32
	ModuleID   int32
	Name       string
	Type       string
	Line       int32
	IsStatic   int32
	IsConst    int32
	IsVolatile int32
	IsAtomic   int32
	IsArray    int32
	PtrDepth   int32
	HasInit    int32
}

type ConfigBlockRow struct {
	ID        int32
	FileID    int32
	Directive string
	Expr      string
	Line      int32
	IsConfig  int32
}

type ReachRow struct {
	SymbolID, NTransitive, NTransitiveOut int32
}

type LockRowR struct {
	ID       int32
	SymbolID int32
	FileID   int32
	Name     string
	Line     int32
}

type EventOpRow struct {
	ID       int32
	SymbolID int32
	FileID   int32
	Family   string
	Fn       string
	Args     string
	Line     int32
}

type APIUseRow struct {
	ID       int32
	SymbolID int32
	FileID   int32
	NS       string
	Fn       string
	Line     int32
}

type IncludeCycleRow struct {
	ID      int32
	APath   string
	BPath   string
	Length  int32
	Members string
}

const (
	TIdent = iota
	TNumber
	TChar
	TStr
	TPunct
	THeader
	TEOF
	TNewline
)

type Tok struct {
	Text string
	Off  int32
	End  int32

	OrigOff int32
	Line    int32
	Col     int32
	SL      int32
	File    uint16
	Punct   uint8
	Kind    uint8

	WSBefore bool
}

var punctTable = []string{

	"%:%:",

	"<<=", ">>=", "...",

	"->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
	"*=", "/=", "%=", "+=", "-=", "&=", "^=", "|=", "##", "<:", ":>", "<%", "%>",
	"%:",

	"[", "]", "(", ")", "{", "}", ".", "&", "*", "+", "-", "~", "!",
	"/", "%", "<", ">", "^", "|", "?", ":", ";", "=", ",", "#",
}

var punctIndex = func() map[string]uint8 {
	m := make(map[string]uint8, len(punctTable))
	if len(punctTable) > 256 {
		panic("punct table exceeds uint8")
	}
	for i, p := range punctTable {
		m[p] = uint8(i)
	}
	return m
}()

var (
	PArrow   = mustPunct("->")
	PInc     = mustPunct("++")
	PDec     = mustPunct("--")
	PLsh     = mustPunct("<<")
	PRsh     = mustPunct(">>")
	PLe      = mustPunct("<=")
	PColon   = mustPunct(":")
	PSemi    = mustPunct(";")
	PEllip   = mustPunct("...")
	PLparen  = mustPunct("(")
	PRparen  = mustPunct(")")
	PLbrack  = mustPunct("[")
	PRbrack  = mustPunct("]")
	PLbrace  = mustPunct("{")
	PRbrace  = mustPunct("}")
	PTerse   = mustPunct("?")
	PLt      = mustPunct("<")
	PGt      = mustPunct(">")
	PAmp     = mustPunct("&")
	PAmpAmp  = mustPunct("&&")
	PStar    = mustPunct("*")
	PPlus    = mustPunct("+")
	PMinus   = mustPunct("-")
	PTilde   = mustPunct("~")
	PBang    = mustPunct("!")
	PSlash   = mustPunct("/")
	PMod     = mustPunct("%")
	PCaret   = mustPunct("^")
	PPipe    = mustPunct("|")
	PPipePip = mustPunct("||")
	PEq      = mustPunct("=")
	PDot     = mustPunct(".")
	PComma   = mustPunct(",")
	PHash    = mustPunct("#")
	PHashAt  = mustPunct("##")
	PEqEq    = mustPunct("==")
	PNotEq   = mustPunct("!=")
	PGe      = mustPunct(">=")
	PSlashEq = mustPunct("/=")
	PModEq   = mustPunct("%=")
)

func mustPunct(s string) uint8 {
	v, ok := punctIndex[s]
	if !ok {
		panic("bad punct " + s)
	}
	return v
}

func c23IsIdentStart(c byte) bool {
	return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80
}

func c23IsIdentPart(c byte) bool {
	return c23IsIdentStart(c) || (c >= '0' && c <= '9')
}

var c23Keywords = map[string]bool{
	"alignas": true, "alignof": true, "asm": true, "auto": true, "bool": true,
	"break": true, "case": true, "char": true, "const": true, "constexpr": true,
	"continue": true, "default": true, "do": true, "double": true, "else": true,
	"enum": true, "extern": true, "false": true, "float": true, "for": true,
	"goto": true, "if": true, "inline": true, "int": true, "long": true,
	"nullptr": true, "register": true, "restrict": true, "return": true,
	"short": true, "signed": true, "sizeof": true, "static": true,
	"static_assert": true, "struct": true, "switch": true, "thread_local": true,
	"true": true, "typedef": true, "typeof": true, "typeof_unqual": true,
	"union": true, "unsigned": true, "void": true, "volatile": true,
	"while":    true,
	"_Alignas": true, "_Alignof": true, "_Atomic": true, "_BitInt": true,
	"_Bool": true, "_Complex": true, "_Decimal128": true, "_Decimal64": true,
	"_Decimal32": true, "_Generic": true, "_Imaginary": true, "_Noreturn": true,
	"_Static_assert": true, "_Thread_local": true,
	"__asm__": true, "__asm": true, "asm_": false,
	"__attribute__": true, "__attribute": true,
	"__const": true, "__const__": true, "__inline": true, "__inline__": true,
	"__restrict": true, "__restrict__": true, "__signed": true,
	"__signed__": true, "__volatile": true, "__volatile__": true,
	"__typeof": true, "__typeof__": true, "__typeof_unqual__": true,
	"__extension__": true, "__alignof": true, "__alignof__": true,
	"__thread": true, "__builtin_va_arg": true, "__builtin_offsetof": true,
	"__builtin_types_compatible_p": true, "__label__": true,
}

func isKeyword(s string) bool { return c23Keywords[s] }

var c23AttrNames = map[string]bool{
	"deprecated": true, "fallthrough": true, "maybe_unused": true,
	"nodiscard": true, "noreturn": true, "_Noreturn": true,
	"reproducible": true, "unsequenced": true,
}

func isC23AttrName(s string) bool { return c23AttrNames[s] }

func spliceSource(src []byte) (out []byte, origOf []int32, origLine []int32, lineStart []int32) {
	out = make([]byte, 0, len(src)+64)
	origOf = make([]int32, 0, len(src)+64)
	origLine = make([]int32, 0, len(src)+64)
	lineStart = append(lineStart, 0)
	ol, oo := int32(1), int32(0)
	i := 0
	n := len(src)
	push := func(c byte) {
		out = append(out, c)
		origOf = append(origOf, oo)
		origLine = append(origLine, ol)
	}
	for i < n {
		c := src[i]
		if c == '\\' && i+1 < n {

			if src[i+1] == '\n' {
				ol++
				oo += 2
				i += 2
				continue
			}
			if src[i+1] == '\r' {
				adv := 2
				if i+2 < n && src[i+2] == '\n' {
					adv = 3
				}
				ol++
				oo += int32(adv)
				i += adv
				continue
			}
		}
		switch {
		case c == '\n':
			push('\n')
			oo++
			i++
			lineStart = append(lineStart, int32(len(out)))
			ol++
		case c == '\r':
			push('\n')
			oo++
			i++
			if i < n && src[i] == '\n' {
				oo++
				i++
			}
			lineStart = append(lineStart, int32(len(out)))
			ol++
		default:
			push(c)
			oo++
			i++
		}
	}
	return out, origOf, origLine, lineStart
}

func stripBOM(b []byte) []byte {
	if len(b) >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF {
		return b[3:]
	}
	return b
}

type lexErr struct {
	off  int32
	msg  string
	line int32
}

type lexer struct {
	src      []byte
	origOf   []int32
	origLine []int32
	lineStar []int32
	fileID   uint32
	pos      int32
	curLine  int32
	posLine  int32
	errs     *[]lexErr

	afterHash  bool
	includeCtx bool
}

func tokFileID(id uint32) uint16 {
	if id > maxFilesPerRun {
		panic("file id exceeds tok uint16")
	}
	return uint16(id)
}

func newLexer(src []byte, fileID uint32, errs *[]lexErr) *lexer {
	src = stripBOM(src)
	lineStar := make([]int32, 0, len(src)/24+8)
	lineStar = append(lineStar, 0)
	for i := 0; i < len(src); i++ {
		c := src[i]
		if c == '\r' || (c == '\\' && i+1 < len(src) &&
			(src[i+1] == '\n' || src[i+1] == '\r')) {
			b, origOf, origLine, ls := spliceSource(src)
			return &lexer{src: b, origOf: origOf, origLine: origLine,
				lineStar: ls, fileID: fileID, errs: errs}
		}
		if c == '\n' {
			lineStar = append(lineStar, int32(i+1))
		}
	}
	return &lexer{src: src, lineStar: lineStar, fileID: fileID, errs: errs}
}

// onDirectiveLine reports whether the current line's first character is `#`.
// Only consulted on the error path.
func (lx *lexer) onDirectiveLine() bool {
	i := int(lx.pos) - 1
	for i >= 0 && lx.src[i] != '\n' {
		i--
	}
	for i++; i < len(lx.src) && (lx.src[i] == ' ' || lx.src[i] == '\t'); i++ {
	}
	return i < len(lx.src) && lx.src[i] == '#'
}

func (lx *lexer) errf(off int32, msg string) {
	if lx.errs != nil && len(*lx.errs) < 4096 {
		*lx.errs = append(*lx.errs, lexErr{off: off, msg: msg, line: lx.lineAt(off)})
	}
}

func (lx *lexer) lineAt(off int32) int32 {
	if lx.origLine == nil {
		if int(off) >= len(lx.src) {
			off = int32(len(lx.src)) - 1
			if off < 0 {
				return 1
			}
		}
		lo, hi := 0, len(lx.lineStar)
		for lo < hi {
			mid := (lo + hi) / 2
			if lx.lineStar[mid] <= off {
				lo = mid + 1
			} else {
				hi = mid
			}
		}
		if lo < 1 {
			return 1
		}
		return int32(lo)
	}
	if int(off) < len(lx.origLine) {
		return lx.origLine[off]
	}
	if n := int32(len(lx.origLine)); n > 0 {
		return lx.origLine[n-1]
	}
	return 1
}

func (lx *lexer) posOf(off int32) (origOff, line, col int32) {
	if lx.origLine == nil {
		if int(off) >= len(lx.src) {
			off = int32(len(lx.src)) - 1
			if off < 0 {
				return 0, 1, 1
			}
		}
	} else if int(off) >= len(lx.origOf) {
		off = int32(len(lx.origOf)) - 1
		if off < 0 {
			return 0, 1, 1
		}
	}

	lineIdx := int(lx.posLine)
	if lineIdx >= len(lx.lineStar) || lx.lineStar[lineIdx] > off {
		lo, hi := 0, len(lx.lineStar)
		for lo < hi {
			mid := (lo + hi) / 2
			if lx.lineStar[mid] <= off {
				lo = mid + 1
			} else {
				hi = mid
			}
		}
		lineIdx = lo - 1
		if lineIdx < 0 {
			lineIdx = 0
		}
	} else {
		for lineIdx+1 < len(lx.lineStar) && lx.lineStar[lineIdx+1] <= off {
			lineIdx++
		}
	}
	lx.posLine = int32(lineIdx)
	if lx.origLine == nil {
		return off, int32(lineIdx + 1), off - lx.lineStar[lineIdx] + 1
	}
	return lx.origOf[off], lx.origLine[off], off - lx.lineStar[lineIdx] + 1
}

func (lx *lexer) at(off int32) byte {
	if int(off) >= len(lx.src) {
		return 0
	}
	return lx.src[off]
}

func (lx *lexer) eof() bool { return lx.pos >= int32(len(lx.src)) }

func (lx *lexer) skipWS() {
	for lx.pos < int32(len(lx.src)) {
		c := lx.src[lx.pos]
		if c == '\n' {
			return
		}
		if c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r' {
			lx.pos++
			continue
		}
		if c == '/' && lx.at(lx.pos+1) == '/' {
			for lx.pos < int32(len(lx.src)) && lx.src[lx.pos] != '\n' {
				lx.pos++
			}
			continue
		}
		if c == '/' && lx.at(lx.pos+1) == '*' {
			start := lx.pos
			lx.pos += 2
			closed := false
			for lx.pos < int32(len(lx.src)) {
				if lx.src[lx.pos] == '*' && lx.at(lx.pos+1) == '/' {
					lx.pos += 2
					closed = true
					break
				}
				lx.pos++
			}
			if !closed {
				lx.errf(start, "unterminated /* comment")
			}
			continue
		}
		return
	}
}

func (lx *lexer) next() Tok {
	p0 := lx.pos
	lx.skipWS()
	wsBefore := lx.pos > p0

	for int(lx.curLine+1) < len(lx.lineStar) && lx.lineStar[lx.curLine+1] <= lx.pos {
		lx.curLine++
	}
	if lx.pos >= int32(len(lx.src)) {
		_, l, co := lx.posOf(int32(len(lx.src)))
		return Tok{Kind: TEOF, Off: int32(len(lx.src)), End: int32(len(lx.src)),
			Line: l, Col: co, File: tokFileID(lx.fileID), WSBefore: wsBefore, SL: lx.curLine}
	}

	if lx.src[lx.pos] == '\n' {
		lx.pos++
		_, l, co := lx.posOf(lx.pos - 1)
		lx.afterHash = false
		lx.includeCtx = false
		return Tok{Kind: TNewline, Text: "\n", Off: lx.pos - 1, End: lx.pos,
			Line: l, Col: co, File: tokFileID(lx.fileID), WSBefore: wsBefore, SL: lx.curLine}
	}
	start := lx.pos
	c := lx.src[lx.pos]
	var t Tok
	t.File = tokFileID(lx.fileID)
	t.WSBefore = wsBefore
	switch {

	case c == '\'' || (c == 'L' && lx.at(lx.pos+1) == '\'') ||
		(c == 'u' && (lx.at(lx.pos+1) == '\'' || (lx.at(lx.pos+1) == '8' && lx.at(lx.pos+2) == '\''))) ||
		(c == 'U' && lx.at(lx.pos+1) == '\''):
		t.Kind = TChar
		lx.scanQuoted('\'')
		t.Text = string(lx.src[start:lx.pos])
	case c == '"' || (c == 'L' && lx.at(lx.pos+1) == '"') ||
		(c == 'u' && (lx.at(lx.pos+1) == '"' || (lx.at(lx.pos+1) == '8' && lx.at(lx.pos+2) == '"'))) ||
		(c == 'U' && lx.at(lx.pos+1) == '"'):
		t.Kind = TStr
		lx.scanQuoted('"')
		t.Text = string(lx.src[start:lx.pos])
	case c23IsIdentStart(c):
		t.Kind = TIdent
		lx.pos++
		for lx.pos < int32(len(lx.src)) {
			ch := lx.src[lx.pos]
			if c23IsIdentPart(ch) {
				lx.pos++
				continue
			}
			if ch == '\\' && (lx.at(lx.pos+1) == 'u' || lx.at(lx.pos+1) == 'U') {
				lx.pos += 2
				nd := 4
				if lx.at(lx.pos-1) == 'U' {
					nd = 8
				}
				for k := 0; k < nd && isHex(lx.at(lx.pos)); k++ {
					lx.pos++
				}
				continue
			}
			break
		}
		t.Text = string(lx.src[start:lx.pos])
	case isDigit(c) || (c == '.' && isDigit(lx.at(lx.pos+1))):
		t.Kind = TNumber
		lx.scanPPNumber()
		t.Text = string(lx.src[start:lx.pos])
	case lx.includeCtx && c == '<':
		t.Kind = THeader
		lx.pos++
		for lx.pos < int32(len(lx.src)) {
			ch := lx.src[lx.pos]
			if ch == '>' {
				lx.pos++
				break
			}
			if ch == '\n' {
				lx.errf(start, "unterminated header name")
				break
			}
			lx.pos++
		}
		t.Text = string(lx.src[start:lx.pos])
	default:

		t.Kind = TPunct
		best := uint8(0xFF)
		bestLen := 0
		for l := 4; l >= 1; l-- {
			if int(lx.pos)+l > len(lx.src) {
				continue
			}
			if code, ok := punctIndex[string(lx.src[lx.pos:int(lx.pos)+l])]; ok {
				best, bestLen = code, l
				break
			}
		}
		if bestLen == 0 {
			// `#error mail bugs@example.org` and `#pragma` text are not C:
			// any character may appear on a directive line
			if !lx.onDirectiveLine() {
				lx.errf(lx.pos, "stray byte in source")
			}
			lx.pos++
			return lx.next()
		}
		lx.pos += int32(bestLen)
		t.Punct = normalizeDigraph(best, string(lx.src[lx.pos-int32(bestLen):lx.pos]))
		t.Text = string(lx.src[start:lx.pos])
	}
	t.Off = start
	t.End = lx.pos
	t.OrigOff, t.Line, t.Col = lx.posOf(start)
	t.SL = lx.curLine
	lx.updateIncludeCtx(t)
	return t
}

func (lx *lexer) updateIncludeCtx(t Tok) {
	if t.Kind == TPunct {
		if t.Punct == PHash {
			lx.afterHash = true
			return
		}
		if t.Punct == PRparen || t.Punct == PComma || t.Punct == PSemi {
			lx.includeCtx = false
			lx.afterHash = false
			return
		}
		return
	}
	if t.Kind == THeader {
		lx.includeCtx = false
		return
	}
	if t.Kind == TIdent {
		if lx.afterHash {
			switch t.Text {
			case "include", "include_next", "embed", "embed_next":
				lx.includeCtx = true
				lx.afterHash = false
				return
			}
			lx.afterHash = false
		}
		switch t.Text {
		case "__has_include", "__has_include_next", "__has_embed":
			lx.includeCtx = true
		}
	}
}

func normalizeDigraph(code uint8, text string) uint8 {
	switch text {
	case "<:":
		return PLbrack
	case ":>":
		return PRbrack
	case "<%":
		return PLbrace
	case "%>":
		return PRbrace
	case "%:":
		return PHash
	case "%:%:":
		return PHashAt
	}
	return code
}

func isHex(c byte) bool {
	return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')
}

func (lx *lexer) scanPPNumber() {
	lx.pos++
	for lx.pos < int32(len(lx.src)) {
		c := lx.src[lx.pos]
		if c23IsIdentPart(c) || c == '.' {

			if (c == 'e' || c == 'E' || c == 'p' || c == 'P') &&
				(lx.at(lx.pos+1) == '+' || lx.at(lx.pos+1) == '-') {
				lx.pos++
				if lx.pos < int32(len(lx.src)) {
					lx.pos++
				}
				continue
			}
			lx.pos++
			continue
		}
		if c == '\'' && c23IsIdentPart(lx.at(lx.pos+1)) {
			lx.pos++
			continue
		}
		break
	}
}

func (lx *lexer) scanQuoted(quote byte) {

	for lx.pos < int32(len(lx.src)) && lx.src[lx.pos] != quote {
		lx.pos++
	}
	if lx.pos >= int32(len(lx.src)) {
		lx.errf(lx.pos, "unterminated "+qname(quote))
		return
	}
	lx.pos++
	for lx.pos < int32(len(lx.src)) {
		c := lx.src[lx.pos]
		if c == '\\' {
			lx.pos += 2
			continue
		}
		if c == '\n' {
			lx.errf(lx.pos, "unterminated "+qname(quote))
			return
		}
		if c == quote {
			lx.pos++
			return
		}
		lx.pos++
	}
	lx.errf(lx.pos, "unterminated "+qname(quote))
}

func qname(q byte) string {
	if q == '\'' {
		return "char const"
	}
	return "string literal"
}

const (
	maxIncludeDepth = 100
	maxFilesPerRun  = 8000
	ppMaxFileBytes  = 32 << 20
	maxExpand       = 1 << 22
)

type ppMacro struct {
	Name       string
	IsFn       bool
	Params     []string
	IsVariadic bool
	VaName     string
	Body       []Tok
	Raw        string
	Line       int32
	FileID     uint32
	Uses       int32
	Multi      bool
}

type includeEdge struct {
	fromID   uint32
	target   string
	resolved string
	sys      bool
	line     int32
}

type pragmaRow struct {
	fileID uint32
	text   string
	line   int32
}

type cfgRow struct {
	fileID    uint32
	directive string
	expr      string
	// The condition's tokens, for a header whose tokens are shared and
	// outlive the parse: its text is only wanted by the one unit that ends up
	// deriving that header, so every other includer skips building it.
	toks []Tok
	line int32
}

func (c *cfgRow) text() string {
	if c.toks != nil {
		return tokLineText(c.toks)
	}
	return c.expr
}

// cfg records one conditional directive of src.
func (pp *preprocessor) cfg(src *fileTokSrc, dir string, rest []Tok, line int32) {
	row := cfgRow{fileID: src.id, directive: dir, line: line}
	if src.lf != nil {
		row.toks = rest
	} else {
		row.expr = tokLineText(rest)
	}
	pp.cfgs = append(pp.cfgs, row)
}

type fileTokSrc struct {
	path   string
	id     uint32
	raw    []byte
	toks   []Tok
	lf     *lexedFile
	own    bool
	lineAt int
	eofTok Tok
	pos    int
	once   bool
	active bool
}

type preprocessor struct {
	macros    map[string]*ppMacro
	allMacros []*ppMacro
	incDirs   []string
	root      string

	out  []Tok
	errs *[]lexErr

	files    []*fileTokSrc
	fileByID map[uint32]*fileTokSrc
	nfiles   int

	includes []includeEdge
	pragmas  []pragmaRow
	cfgs     []cfgRow

	cond    []condState
	onceSet map[string]bool
	onStack map[string]bool

	genFileID uint32

	lineDelta int64
	fileName  string
	rootName  string

	depth    int
	stopped  bool
	nExpand  int
	reg      *ppRegion
	expDepth int
	subDepth int
}

// ppRegion is everything one translation unit's preprocessing allocates and
// nothing outlives: the macro table, the macro records in it, the hide-set
// cells, the scratch stacks of the expander. A parse worker owns one for the
// whole run and resets it between units, so after the first few units the
// preprocessor allocates almost nothing -- on PostgreSQL every unit used to
// build a ten-thousand-entry macro table from an empty map and throw it away.
//
// It is a typed region, not an untyped one: these records hold strings and
// slices that live on the ordinary heap, and memory the collector cannot see
// into would let it free them. Slabs of ppMacro and hideSet keep the pointers
// visible and still turn a million small allocations into a few hundred.
//
// A region may only be reused once the unit's rows have been derived.
type ppRegion struct {
	macros   map[string]*ppMacro
	fileByID map[uint32]*fileTokSrc
	onceSet  map[string]bool
	onStack  map[string]bool

	allMacros []*ppMacro
	files     []*fileTokSrc
	includes  []includeEdge
	pragmas   []pragmaRow
	cfgs      []cfgRow
	cond      []condState

	subBufs  [][]Tok
	expBufs  [][]Tok
	workBufs [][]expTok
	argToks  [][]expTok
	argHdrs  [][][]expTok
	argEnds  []int

	macroSlabs [][]ppMacro
	macroSlab  int
	macroNext  int
	hideSlabs  [][]hideSet
	hideSlab   int
	hideNext   int

	last *preprocessor
}

const (
	macroSlabLen = 1024
	hideSlabLen  = 4096
)

func newPPRegion() *ppRegion {
	return &ppRegion{
		macros:   map[string]*ppMacro{},
		fileByID: map[uint32]*fileTokSrc{},
		onceSet:  map[string]bool{},
		onStack:  map[string]bool{},
	}
}

// reset takes back what the previous unit's preprocessor grew and empties it.
func (r *ppRegion) reset() {
	if pp := r.last; pp != nil {
		r.allMacros, r.files = pp.allMacros[:0], pp.files[:0]
		r.includes, r.pragmas = pp.includes[:0], pp.pragmas[:0]
		r.cfgs, r.cond = pp.cfgs[:0], pp.cond[:0]
		r.last = nil
	}
	clear(r.macros)
	clear(r.fileByID)
	clear(r.onceSet)
	clear(r.onStack)
	r.macroSlab, r.macroNext = 0, 0
	r.hideSlab, r.hideNext = 0, 0
}

// newMacro returns a zeroed record from the slab.
func (r *ppRegion) newMacro() *ppMacro {
	if r.macroNext == macroSlabLen {
		r.macroSlab++
		r.macroNext = 0
	}
	if r.macroSlab == len(r.macroSlabs) {
		r.macroSlabs = append(r.macroSlabs, make([]ppMacro, macroSlabLen))
	}
	m := &r.macroSlabs[r.macroSlab][r.macroNext]
	r.macroNext++
	*m = ppMacro{}
	return m
}

func (r *ppRegion) newHide(name string, parent *hideSet) *hideSet {
	if r.hideNext == hideSlabLen {
		r.hideSlab++
		r.hideNext = 0
	}
	if r.hideSlab == len(r.hideSlabs) {
		r.hideSlabs = append(r.hideSlabs, make([]hideSet, hideSlabLen))
	}
	h := &r.hideSlabs[r.hideSlab][r.hideNext]
	r.hideNext++
	h.name, h.parent = name, parent
	return h
}

type condState struct {
	parentActive bool
	takenYet     bool
	activeNow    bool
	seenElse     bool
}

// predefMacros is the predefined set, lexed once; each unit copies the
// records into its own slab because a macro counts its uses.
var predefMacros []ppMacro

func newPP(root string, incDirs []string, errs *[]lexErr, reg *ppRegion) *preprocessor {
	if reg == nil {
		reg = newPPRegion()
	} else {
		reg.reset()
	}
	pp := &preprocessor{
		macros:    reg.macros,
		incDirs:   incDirs,
		root:      root,
		errs:      errs,
		fileByID:  reg.fileByID,
		onceSet:   reg.onceSet,
		onStack:   reg.onStack,
		allMacros: reg.allMacros,
		files:     reg.files,
		includes:  reg.includes,
		pragmas:   reg.pragmas,
		cfgs:      reg.cfgs,
		cond:      reg.cond,
		reg:       reg,
	}
	reg.last = pp
	for i := range predefMacros {
		m := reg.newMacro()
		*m = predefMacros[i]
		pp.macros[m.Name] = m
	}
	return pp
}

func init() {

	for _, d := range [][2]string{
		{"__STDC__", "1"}, {"__STDC_VERSION__", "202311L"},
		{"__STDC_HOSTED__", "1"}, {"__c23parser", "1"},
		// What clang reports. Real C selects its implementation on this: with
		// it absent PostgreSQL reached 1,925 #error directives ("could not
		// find an implementation of pg_atomic_uint32") and redis compiled
		// likely() to nothing. Measured on three trees: 76 #errors left, and
		// not one new parse diagnostic from the GNU branches it opens.
		{"__GNUC__", "4"}, {"__GNUC_MINOR__", "2"}, {"__GNUC_PATCHLEVEL__", "1"},
		{"__CHAR_BIT__", "8"}, {"__SIZEOF_LONG__", "8"}, {"__SIZEOF_PTRDIFF_T__", "8"},
		{"__SIZE_TYPE__", "unsigned long"}, {"__PTRDIFF_TYPE__", "long"},
		{"LITTLE_ENDIAN", "1234"}, {"BIG_ENDIAN", "4321"},
		{"BYTE_ORDER", "1234"}, {"__BYTE_ORDER", "1234"},
		{"__ORDER_LITTLE_ENDIAN__", "1234"}, {"__ORDER_BIG_ENDIAN__", "4321"},
		{"__ORDER_PDP_ENDIAN__", "3412"},
		{"__linux__", "1"}, {"linux", "1"}, {"__unix__", "1"}, {"__unix", "1"},
		{"__APPLE__", "1"}, {"__MACH__", "1"},
		{"__x86_64__", "1"}, {"__x86_64", "1"}, {"__amd64__", "1"}, {"__amd64", "1"},
		{"__LITTLE_ENDIAN__", "1"}, {"__ARM_ARCH", "8"},

		{"__has_include", "1"}, {"__has_include_next", "1"},
		{"__has_c_attribute", "0"}, {"__has_embed", "0"},
	} {

		predefMacros = append(predefMacros, ppMacro{Name: d[0],
			Body: ppPredefBody(d[1]), Raw: d[1]})
	}
}

func ppPredefBody(text string) []Tok {
	lex := newLexer([]byte(text), 0, nil)
	var out []Tok
	for i := 0; i < 64; i++ {
		t := lex.next()
		if t.Kind == TEOF {
			break
		}
		out = append(out, t)
	}
	return out
}

func (pp *preprocessor) errf(line int32, msg string) {
	// every preprocessor diagnostic is charged to the translation unit's root
	// file, so one raised inside a header has to say which header
	if pp.rootName != "" && pp.fileName != "" && pp.fileName != pp.rootName {
		msg += " (in " + pp.fileName + ":" + strconv.Itoa(int(line)) + ")"
	}
	if pp.errs != nil && len(*pp.errs) < 4096 {
		*pp.errs = append(*pp.errs, lexErr{off: 0, msg: msg, line: line})
	}
}

var ppSrcMu sync.RWMutex
var ppSrcCache = map[string][]byte{}

// A unit that includes one large project header expands to a few hundred
// thousand tokens. With the cap below that, its buffer was never returned to
// the pool and every such unit regrew one from nothing: 500 MB of copying on
// redis, in spans large enough to contend on the heap lock.
const tokBufMax = 1 << 20

// The preprocessor's output buffers are kept on an explicit free list for the
// length of the parse, one per worker at most. They used to sit in a
// sync.Pool, which the collector empties on every cycle: with a cycle every
// few milliseconds each unit allocated and zeroed a fresh multi-megabyte
// buffer, and the heap grew and shrank underneath them (an eighth of the
// run's CPU was madvise).
var (
	tokFreeMu sync.Mutex
	tokFree   [][]Tok
)

const tokFreeMax = 32

func getTokBuf() []Tok {
	tokFreeMu.Lock()
	var b []Tok
	if n := len(tokFree); n > 0 {
		b = tokFree[n-1]
		tokFree[n-1] = nil
		tokFree = tokFree[:n-1]
	}
	tokFreeMu.Unlock()
	return b[:0]
}

func putTokBuf(b []Tok) {
	if c := cap(b); c < 1024 || c > tokBufMax {
		return
	}
	// Stale tokens are left in place: what their strings point at -- file
	// text and interned names -- outlives the parse anyway, and zeroing a
	// multi-megabyte buffer per unit is the cost this list exists to avoid.
	tokFreeMu.Lock()
	if len(tokFree) < tokFreeMax {
		tokFree = append(tokFree, b[:0])
	}
	tokFreeMu.Unlock()
}

func dropTokBufs() {
	tokFreeMu.Lock()
	tokFree = nil
	tokFreeMu.Unlock()
	faFreeMu.Lock()
	faFree = nil
	faFreeMu.Unlock()
}

func ppCachedRead(abs string) ([]byte, bool) {
	ppSrcMu.RLock()
	data, ok := ppSrcCache[abs]
	ppSrcMu.RUnlock()
	return data, ok
}

// A header is lexed once and its tokens shared by every translation unit that
// includes it. Lexing it again per includer was 2.4 GB of a 3.6 GB allocation
// total on redis -- server.h alone is read by two hundred files -- and the
// churn, not the work, was what the run spent its time on.
//
// The shared slice is read-only. The one per-unit field a token carries is its
// file id, so shared tokens hold 0 and process() stamps the id on the copies
// it emits; nothing else reads it before then.
type lexedFile struct {
	data []byte
	toks []Tok
	errs []lexErr
	// #define directives already parsed, by the index of the line's first
	// token. A definition depends on nothing but its own tokens, so the
	// second unit to include the header copies the 100-byte record instead
	// of re-reading the parameter list and copying the body.
	macros sync.Map
}

var (
	lexCache     sync.Map
	lexCacheToks atomic.Int64
)

// lexCacheMaxToks bounds what the cache may hold (about 48 bytes a token).
// Past it, headers are lexed per use exactly as before.
const lexCacheMaxToks = 8 << 20

func lexWhole(data []byte, fileID uint32, errs *[]lexErr, toks []Tok) []Tok {
	lex := newLexer(data, fileID, errs)
	if want := len(data)/5 + 16; cap(toks) < want {
		toks = make([]Tok, 0, want)
	}
	for {
		t := lex.next()
		toks = append(toks, t)
		if t.Kind == TEOF {
			return toks
		}
	}
}

func (pp *preprocessor) loadFile(abs string, shared bool) (*fileTokSrc, bool) {
	if pp.nfiles >= maxFilesPerRun {
		return nil, false
	}
	var lf *lexedFile
	if shared {
		if v, ok := lexCache.Load(abs); ok {
			lf = v.(*lexedFile)
		}
	}
	if lf == nil {
		data, ok := ppCachedRead(abs)
		if !ok {
			fi, err := os.Stat(abs)
			if err != nil || fi.IsDir() || fi.Size() > ppMaxFileBytes {
				return nil, false
			}
			data, err = os.ReadFile(abs)
			if err != nil {
				return nil, false
			}
		}
		if shared && lexCacheToks.Load() < lexCacheMaxToks {
			lf = &lexedFile{data: data}
			lf.toks = lexWhole(data, 0, &lf.errs, nil)
			if prev, loaded := lexCache.LoadOrStore(abs, lf); loaded {
				lf = prev.(*lexedFile)
			} else {
				lexCacheToks.Add(int64(len(lf.toks)))
			}
		} else {
			pp.nfiles++
			pp.genFileID++
			id := pp.genFileID
			src := &fileTokSrc{path: abs, id: id, raw: data}
			// a file lexed for this unit alone: its tokens die with the unit,
			// so the buffer comes from, and goes back to, the free list
			src.toks = lexWhole(data, id, pp.errs, getTokBuf())
			src.own = true
			src.eofTok = src.toks[len(src.toks)-1]
			pp.files = append(pp.files, src)
			pp.fileByID[id] = src
			return src, true
		}
	}
	pp.nfiles++
	pp.genFileID++
	id := pp.genFileID
	src := &fileTokSrc{path: abs, id: id, raw: lf.data, toks: lf.toks, lf: lf}
	src.eofTok = lf.toks[len(lf.toks)-1]
	src.eofTok.File = tokFileID(id)
	if pp.errs != nil {
		for _, e := range lf.errs {
			if len(*pp.errs) < 4096 {
				e.msg += " (in " + pp.relFileName(abs) + ":" + strconv.Itoa(int(e.line)) + ")"
				*pp.errs = append(*pp.errs, e)
			}
		}
	}
	pp.files = append(pp.files, src)
	pp.fileByID[id] = src
	return src, true
}

func (pp *preprocessor) relFileName(abs string) string {
	if pp.root == "" {
		return abs
	}
	base := filepath.Dir(pp.root)
	if base == "" {
		return abs
	}
	if r, err := filepath.Rel(base, abs); err == nil && !strings.HasPrefix(r, "..") {
		return filepath.ToSlash(r)
	}
	// a header elsewhere in the tree: relative to the tree, never absolute --
	// an absolute path in a message makes the output depend on where the
	// checkout happens to live
	if ppTreeRoot != "" {
		if r, err := filepath.Rel(ppTreeRoot, abs); err == nil && !strings.HasPrefix(r, "..") {
			return filepath.ToSlash(r)
		}
	}
	return filepath.Base(abs)
}

var ppTreeRoot string

func (pp *preprocessor) run(abs string) []Tok {
	pp.out = getTokBuf()
	pp.fileName = pp.relFileName(abs)
	pp.rootName = pp.fileName
	if src, ok := pp.loadFile(abs, false); ok {
		pp.process(src, 1)
	}

	eof := Tok{Kind: TEOF, Text: "", File: tokFileID(pp.genFileID), Line: 1, Col: 1}
	if len(pp.files) > 0 {
		eof = pp.files[len(pp.files)-1].eofTok
	}
	pp.out = append(pp.out, eof)
	return pp.out
}

func (pp *preprocessor) process(src *fileTokSrc, depth int) {
	if src.active || pp.onceSet[src.path] || depth > maxIncludeDepth {
		return
	}
	src.active = true
	pp.onStack[src.path] = true

	savedName := pp.fileName
	pp.fileName = pp.relFileName(src.path)
	defer func() {
		src.active = false
		pp.onStack[src.path] = false
		if src.own {
			putTokBuf(src.toks)
		}
		src.toks = nil
		pp.fileName = savedName
	}()

	fidTok := tokFileID(src.id)
	for src.pos < len(src.toks) {
		t := src.toks[src.pos]
		if t.Kind == TEOF {
			return
		}
		lineStart := src.pos
		lineEnd := src.pos
		for lineEnd < len(src.toks) && src.toks[lineEnd].Kind != TNewline &&
			src.toks[lineEnd].Kind != TEOF {
			lineEnd++
		}
		hadNL := lineEnd < len(src.toks) && src.toks[lineEnd].Kind == TNewline
		line := src.toks[lineStart:lineEnd]
		src.pos = lineEnd
		if hadNL {
			src.pos++
		}
		if len(line) == 0 {
			continue
		}
		if line[0].Kind == TPunct && line[0].Punct == PHash {
			src.lineAt = lineStart
			pp.directive(src, line, line[0].Line, depth)
			if pp.stopped {
				return
			}
			continue
		}
		if !pp.condActive() {
			continue
		}
		if pp.lineMayExpand(line) {
			// A function-like macro invocation may run over several lines.
			// Expansion is per line, so one left open at the end of a line
			// was never expanded at all: `serverLog(LL_WARNING,\n "...")`
			// stayed a call to the macro's name and the function behind it
			// lost the edge. Pull lines in until the invocation closes.
			if pp.openInvocation(line) {
				joined := append(make([]Tok, 0, len(line)*3), line...)
				for n := 0; n < 4000 && src.pos < len(src.toks); n++ {
					nt := src.toks[src.pos]
					if nt.Kind == TEOF || (nt.Kind == TPunct && nt.Punct == PHash) {
						break
					}
					e := src.pos
					for e < len(src.toks) && src.toks[e].Kind != TNewline &&
						src.toks[e].Kind != TEOF {
						e++
					}
					joined = append(joined, src.toks[src.pos:e]...)
					src.pos = e
					if e < len(src.toks) && src.toks[e].Kind == TNewline {
						src.pos++
					}
					if !pp.openInvocation(joined) {
						break
					}
				}
				line = joined
			}
			n0 := len(pp.out)
			pp.out = append(pp.out, pp.expand(line)...)
			for k := n0; k < len(pp.out); k++ {
				pp.out[k].File = fidTok
			}
		} else {
			n0 := len(pp.out)
			pp.out = append(pp.out, line...)
			for k := n0; k < len(pp.out); k++ {
				pp.out[k].File = fidTok
			}
		}
	}
}

// openInvocation reports whether toks ends inside the argument list of a
// function-like macro: the name was seen, its `(` opened, and the matching
// `)` has not arrived.
func (pp *preprocessor) openInvocation(toks []Tok) bool {
	depth := 0
	in := false
	for i := range toks {
		t := &toks[i]
		if !in {
			if t.Kind != TIdent || i+1 >= len(toks) {
				continue
			}
			nx := &toks[i+1]
			if nx.Kind != TPunct || nx.Punct != PLparen {
				continue
			}
			if m, ok := pp.macros[t.Text]; ok && m.IsFn {
				in = true
				depth = 0
			}
			continue
		}
		if t.Kind != TPunct {
			continue
		}
		switch t.Punct {
		case PLparen:
			depth++
		case PRparen:
			depth--
			if depth == 0 {
				in = false
			}
		}
	}
	return in
}

func (pp *preprocessor) directive(src *fileTokSrc, line []Tok, dirLine int32, depth int) {
	if len(line) < 2 {
		return
	}
	name := line[1]
	if name.Kind != TIdent {
		pp.errf(name.Line, "bad directive name")
		return
	}
	rest := line[2:]
	dir := name.Text
	switch dir {
	case "define":
		pp.doDefine(src, line)
	case "undef":
		if len(rest) > 0 && rest[0].Kind == TIdent {
			if pp.condActive() {
				delete(pp.macros, rest[0].Text)
			}
		}
	case "include", "include_next":
		pp.doInclude(src, rest, dirLine, depth)
	case "if":
		val := int64(0)
		active := pp.condActive()
		if active {
			val = pp.evalCond(src, rest, dirLine)
		}
		pp.cfg(src, "if", rest, dirLine)
		on := active && val != 0
		pp.cond = append(pp.cond, condState{parentActive: active,
			takenYet: on, activeNow: on})
	case "ifdef", "ifndef":
		val := false
		if pp.condActive() {
			def := false
			if len(rest) > 0 && rest[0].Kind == TIdent {
				_, def = pp.macros[rest[0].Text]
			}
			val = def
			if dir == "ifndef" {
				val = !def
			}
		}
		pp.cfg(src, dir, rest, dirLine)
		on := pp.condActive() && val
		pp.cond = append(pp.cond, condState{parentActive: pp.condActive(),
			takenYet: on, activeNow: on})
	case "elif":
		if len(pp.cond) == 0 {
			pp.errf(dirLine, "#elif without #if")
			return
		}
		st := &pp.cond[len(pp.cond)-1]
		pp.cfg(src, "elif", rest, dirLine)
		if st.seenElse {
			pp.errf(dirLine, "#elif after #else")
			return
		}
		st.activeNow = false
		if st.parentActive && !st.takenYet && pp.evalCond(src, rest, dirLine) != 0 {
			st.activeNow = true
			st.takenYet = true
		}
	case "elifdef", "elifndef":
		if len(pp.cond) == 0 {
			pp.errf(dirLine, "#"+dir+" without #if")
			return
		}
		st := &pp.cond[len(pp.cond)-1]
		pp.cfg(src, dir, rest, dirLine)
		if st.seenElse {
			return
		}
		st.activeNow = false
		if st.parentActive && !st.takenYet {
			def := false
			if len(rest) > 0 && rest[0].Kind == TIdent {
				_, def = pp.macros[rest[0].Text]
			}
			if dir == "elifndef" {
				def = !def
			}
			st.activeNow = def
			st.takenYet = def
		}
	case "else":
		if len(pp.cond) == 0 {
			pp.errf(dirLine, "#else without #if")
			return
		}
		st := &pp.cond[len(pp.cond)-1]
		pp.cfgs = append(pp.cfgs, cfgRow{fileID: src.id, directive: "else", line: dirLine})
		st.seenElse = true
		st.activeNow = st.parentActive && !st.takenYet
		st.takenYet = st.takenYet || st.activeNow
	case "endif":
		pp.cfgs = append(pp.cfgs, cfgRow{fileID: src.id, directive: "endif", line: dirLine})
		if len(pp.cond) == 0 {
			pp.errf(dirLine, "#endif without #if")
			return
		}
		pp.cond = pp.cond[:len(pp.cond)-1]
	case "line":
		pp.doLine(src, rest, dirLine)
	case "error":

		if pp.condActive() {
			pp.errf(dirLine, "#error: "+tokLineText(rest))
		}
	case "warning":
		pp.pragmas = append(pp.pragmas, pragmaRow{src.id, "#warning " + tokLineText(rest), dirLine})
	case "pragma":
		if len(rest) > 0 && rest[0].Kind == TIdent && rest[0].Text == "once" {
			pp.onceSet[src.path] = true
			src.once = true
		}
		pp.pragmas = append(pp.pragmas, pragmaRow{src.id, "#pragma " + tokLineText(rest), dirLine})
	case "embed", "embed_next":

		pp.pragmas = append(pp.pragmas, pragmaRow{src.id, "#" + dir + " " + tokLineText(rest), dirLine})
	default:
		pp.errf(dirLine, "unknown directive #"+dir)
	}
}

func tokLineText(toks []Tok) string {
	var b strings.Builder
	for i, t := range toks {
		if i > 0 && t.WSBefore {
			b.WriteByte(' ')
		}
		b.WriteString(t.Text)
	}
	return b.String()
}

func (pp *preprocessor) condActive() bool {
	for i := range pp.cond {
		if !pp.cond[i].activeNow {
			return false
		}
	}
	return true
}

func (pp *preprocessor) doDefine(src *fileTokSrc, line []Tok) {

	if len(line) < 3 || line[2].Kind != TIdent {
		pp.errf(line[0].Line, "malformed #define")
		return
	}
	nmTok := line[2]
	if src.lf != nil {
		if t, ok := src.lf.macros.Load(src.lineAt); ok {
			m := pp.reg.newMacro()
			*m = *t.(*ppMacro)
			m.FileID = src.id
			pp.allMacros = append(pp.allMacros, m)
			if pp.condActive() {
				pp.macros[m.Name] = m
			}
			return
		}
	}
	m := pp.reg.newMacro()
	m.Name, m.FileID, m.Line = nmTok.Text, src.id, nmTok.Line
	i := 3
	if i < len(line) && line[i].Kind == TPunct && line[i].Punct == PLparen && !line[i].WSBefore {

		i++
		var params []string
		m.IsVariadic = false
		cur := strings.Builder{}
		depthP := 1
		for i < len(line) && depthP > 0 {
			t := line[i]
			switch {
			case t.Kind == TPunct && t.Punct == PLparen:
				depthP++
				cur.WriteByte('(')
			case t.Kind == TPunct && t.Punct == PRparen:
				depthP--
				if depthP == 0 {
					break
				}
				cur.WriteByte(')')
			case t.Kind == TPunct && t.Punct == PComma && depthP == 1:
				s := strings.TrimSpace(cur.String())
				if s != "" {
					params = append(params, s)
				}
				cur.Reset()
			case t.Kind == TPunct && t.Punct == PEllip && depthP == 1:
				m.IsVariadic = true
				if c := strings.TrimSpace(cur.String()); c != "" {
					m.VaName = c
				}
				cur.Reset()
			case t.Kind == TNewline:

			default:
				cur.WriteString(t.Text)
			}
			i++
		}
		if depthP != 0 {
			pp.errf(nmTok.Line, "unbalanced macro parameter list")
			return
		}
		if s := strings.TrimSpace(cur.String()); s != "" {
			params = append(params, s)
		}
		m.IsFn = true
		m.Params = params
		if m.IsVariadic && m.VaName == "" {
			m.VaName = "__VA_ARGS__"
		}
	}
	if i < len(line) {
		m.Body = append([]Tok(nil), line[i:]...)
	}
	m.Raw = tokLineText(line[3:])
	// Raw has its continuations spliced out, so a trailing backslash can never
	// be seen there; the definition is multi-line iff its tokens span lines.
	for _, t := range line[3:] {
		if t.Kind != TNewline && t.Line > nmTok.Line {
			m.Multi = true
			break
		}
	}
	if src.lf != nil {
		t := new(ppMacro)
		*t = *m
		src.lf.macros.Store(src.lineAt, t)
	}
	pp.allMacros = append(pp.allMacros, m)
	if pp.condActive() {
		pp.macros[m.Name] = m
	}
}

func (pp *preprocessor) doInclude(src *fileTokSrc, rest []Tok, dirLine int32, depth int) {
	if len(rest) == 0 {
		pp.errf(dirLine, "empty #include")
		return
	}
	tgt := ""
	sys := false
	switch {
	case rest[0].Kind == THeader:
		tgt = strings.TrimSuffix(strings.TrimPrefix(rest[0].Text, "<"), ">")
		sys = true
	case rest[0].Kind == TStr:
		tgt = dequote(rest[0].Text)
	default:
		ex := pp.expand(rest)
		if len(ex) > 0 {
			switch {
			case ex[0].Kind == THeader:
				tgt = strings.TrimSuffix(strings.TrimPrefix(ex[0].Text, "<"), ">")
				sys = true
			case ex[0].Kind == TStr:
				tgt = dequote(ex[0].Text)
			}
		}
	}
	if tgt == "" {
		// `#if defined(X)` / `#include X` / `#endif`: in the arm that is not
		// taken there is nothing to resolve and nothing wrong
		if pp.condActive() {
			pp.errf(dirLine, "malformed #include")
		}
		return
	}
	resolved := pp.resolveInclude(src, tgt, sys)
	pp.includes = append(pp.includes, includeEdge{fromID: src.id, target: tgt,
		resolved: resolved, sys: sys, line: dirLine})
	if resolved == "" || !pp.condActive() {
		return
	}
	if pp.onceSet[resolved] || pp.onStack[resolved] {
		return
	}
	nxt, ok := pp.loadFile(resolved, true)
	if !ok {
		return
	}
	pp.process(nxt, depth+1)
}

func dequote(s string) string {
	s = strings.TrimPrefix(s, "u8")
	s = strings.TrimPrefix(s, "u")
	s = strings.TrimPrefix(s, "U")
	s = strings.TrimPrefix(s, "L")
	return strings.Trim(s, `"`)
}

type incCacheKey struct {
	root string
	dir  string
	tgt  string
	sys  bool
}

var (
	incCacheMu sync.RWMutex
	incCache   = map[incCacheKey]string{}

	statSeen sync.Map
	dirSeen  sync.Map
)

// statIsFile answers "is p an existing non-directory" from one listing of its
// directory instead of one stat per candidate. Include resolution probes every
// (directory, name) pair it is asked about and most probes miss; a miss costs
// a syscall, and on redis those were a fifth of the run's CPU. Only a symlink
// (which may point at a directory) still needs its own stat.
func statIsFile(p string) bool {
	if v, ok := statSeen.Load(p); ok {
		return v.(bool)
	}
	dir, base := filepath.Split(p)
	ents := dirEntries(dir)
	r := false
	switch {
	case ents == nil:
		fi, err := os.Stat(p)
		r = err == nil && !fi.IsDir()
	case ents[base] == 1:
		r = true
	case ents[base] == 2, ents[base] == 0 && ents["\x00"+strings.ToLower(base)] != 0:
		fi, err := os.Stat(p)
		r = err == nil && !fi.IsDir()
	}
	statSeen.Store(p, r)
	return r
}

// dirEntries lists dir once: 1 a regular file, 3 a directory, 2 anything else
// (a symlink, which only a stat can classify). nil means the listing failed
// for a reason other than the directory not existing.
//
// Whether dir exists at all is first asked of its PARENT's listing. Include
// resolution probes `<includer's dir>/utils/rel.h` for every includer, and on
// PostgreSQL nearly every one of those `utils` directories does not exist:
// answering from the parent turned eighteen thousand failed opens into none.
func dirEntries(dir string) map[string]uint8 {
	if v, ok := dirSeen.Load(dir); ok {
		return v.(map[string]uint8)
	}
	ents := map[string]uint8{}
	clean := strings.TrimRight(dir, string(filepath.Separator))
	if ppTreeRoot != "" && len(clean) > len(ppTreeRoot) && strings.HasPrefix(clean, ppTreeRoot) {
		parent, name := filepath.Split(clean)
		if pe := dirEntries(parent); pe != nil && pe[name] != 3 && pe[name] != 2 &&
			pe["\x00"+strings.ToLower(name)] == 0 {
			dirSeen.Store(dir, ents)
			return ents
		}
	}
	{
		if des, err := os.ReadDir(dir); err == nil {
			for _, de := range des {
				switch t := de.Type(); {
				case t.IsRegular():
					ents[de.Name()] = 1
				case t.IsDir():
					ents[de.Name()] = 3
				default:
					ents[de.Name()] = 2
				}
				// a case-insensitive filesystem resolves `Foo.h` to `foo.h`;
				// whether this one does is the filesystem's to say
				ents["\x00"+strings.ToLower(de.Name())] = 2
			}
		} else if !os.IsNotExist(err) {
			ents = nil
		}
		dirSeen.Store(dir, ents)
	}
	return ents
}

func (pp *preprocessor) resolveInclude(src *fileTokSrc, tgt string, sys bool) string {
	// The last place looked is the directory of the translation unit's root
	// file -- what `cc -I.` run beside the source gives. pp.root is that FILE;
	// joining the target onto it made a path that cannot exist, probed once per
	// (unit, include) because the cache key carried the file too.
	tuDir := ""
	if pp.root != "" {
		tuDir = filepath.Dir(pp.root)
	}
	// the includer's directory is part of the key for both forms: the tree
	// lookup below breaks ties by it
	key := incCacheKey{root: tuDir, tgt: tgt, sys: sys, dir: filepath.Dir(src.path)}
	cacheable := len(pp.incDirs) == 0
	if cacheable {
		incCacheMu.RLock()
		hit, ok := incCache[key]
		incCacheMu.RUnlock()
		if ok {
			return hit
		}
	}
	cands := make([]string, 0, 4+len(pp.incDirs))
	if !sys {
		cands = append(cands, filepath.Join(key.dir, tgt))
	}
	for _, d := range pp.incDirs {
		cands = append(cands, filepath.Join(d, tgt))
	}
	if tuDir != "" && (sys || tuDir != key.dir) {
		cands = append(cands, filepath.Join(tuDir, tgt))
	}
	res := ""
	for _, c := range cands {
		if statIsFile(c) {
			if rp, err2 := filepath.Abs(c); err2 == nil {
				res = rp
			} else {
				res = c
			}
			break
		}
	}
	if res == "" && !(sys && isSystemHeader(tgt)) {
		res = treeLookup(key.dir, tgt)
	}
	if cacheable {
		incCacheMu.Lock()
		incCache[key] = res
		incCacheMu.Unlock()
	}
	return res
}

// treeIdx maps a file's base name to every catalogued file with that name.
// It stands in for the -I flags nobody passed: a project built with
// `-I src/include` writes `#include "utils/rel.h"`, which is next to neither
// the includer nor the unit. Without it PostgreSQL resolved almost none of its
// own headers -- a third of its call sites were macros left unexpanded and a
// third of its files failed to parse on type names they had never seen.
var treeIdx map[string][]string

func buildTreeIdx(paths []string) {
	m := make(map[string][]string, len(paths))
	for _, p := range paths {
		b := filepath.Base(p)
		m[b] = append(m[b], p)
	}
	for _, v := range m {
		sort.Strings(v)
	}
	treeIdx = m
}

// treeLookup finds the file in the tree whose path ENDS with tgt. With one
// match that is the answer. With several -- `port.h` exists per platform --
// it takes the one sharing the most leading directories with the includer,
// then the shortest path, then the first in order. The choice depends only
// on the two paths, never on what was resolved before, so a run is
// reproducible whatever order the files are parsed in.
func treeLookup(fromDir, tgt string) string {
	if treeIdx == nil || tgt == "" || strings.Contains(tgt, "..") {
		return ""
	}
	tgt = filepath.Clean(filepath.FromSlash(tgt))
	suffix := string(filepath.Separator) + tgt
	best, bestShared := "", -1
	for _, c := range treeIdx[filepath.Base(tgt)] {
		if !strings.HasSuffix(c, suffix) {
			continue
		}
		shared := sharedDirs(fromDir, c)
		if shared > bestShared || (shared == bestShared && len(c) < len(best)) {
			best, bestShared = c, shared
		}
	}
	return best
}

// sharedDirs counts the leading path components a and b have in common.
func sharedDirs(a, b string) int {
	n := 0
	for i := 0; i < len(a) && i < len(b) && a[i] == b[i]; i++ {
		if a[i] == filepath.Separator {
			n++
		}
	}
	return n
}

// isSystemHeader: names the C library and the kernel own. A tree that ships a
// shim for one (PostgreSQL's port/win32/sys/socket.h) does not mean a Unix
// build of it includes that shim for `<sys/socket.h>`.
func isSystemHeader(tgt string) bool {
	for _, p := range [...]string{"sys/", "netinet/", "arpa/", "net/", "linux/",
		"mach/", "machine/", "bits/", "asm/", "netinet6/", "rpc/"} {
		if strings.HasPrefix(tgt, p) {
			return true
		}
	}
	return stdHeaders[tgt]
}

var stdHeaders = func() map[string]bool {
	m := map[string]bool{}
	for _, h := range strings.Fields(`assert.h complex.h ctype.h errno.h fenv.h
		float.h inttypes.h iso646.h limits.h locale.h math.h setjmp.h signal.h
		stdalign.h stdarg.h stdatomic.h stdbit.h stdbool.h stdckdint.h stddef.h
		stdint.h stdio.h stdlib.h stdnoreturn.h string.h tgmath.h threads.h
		time.h uchar.h wchar.h wctype.h
		aio.h cpio.h dirent.h dlfcn.h fcntl.h fnmatch.h ftw.h glob.h grp.h
		iconv.h langinfo.h libgen.h monetary.h mqueue.h ndbm.h netdb.h nl_types.h
		poll.h pthread.h pwd.h regex.h sched.h search.h semaphore.h spawn.h
		strings.h stropts.h syslog.h tar.h termios.h trace.h ulimit.h unistd.h
		utime.h utmpx.h wordexp.h getopt.h alloca.h malloc.h endian.h byteswap.h
		execinfo.h ifaddrs.h io.h direct.h process.h windows.h winsock2.h
		ws2tcpip.h crtdbg.h`) {
		m[h] = true
	}
	return m
}()

func (pp *preprocessor) doLine(src *fileTokSrc, rest []Tok, dirLine int32) {
	if len(rest) == 0 {
		return
	}
	ex := pp.expand(rest)
	if len(ex) == 0 {
		return
	}
	if v, ok := ppInt(ex[0].Text); ok {

		pp.lineDelta = v - int64(dirLine) - 1
	}
	if len(ex) > 2 && ex[1].Kind == TStr {
		pp.fileName = dequote(ex[1].Text)
	}
}

func ppInt(s string) (int64, bool) {
	v, err := strconv.ParseInt(stripNumSuffix(s), 0, 64)
	return v, err == nil
}

func stripNumSuffix(s string) string {

	i := len(s)
	for i > 0 {
		c := s[i-1]
		if c == 'u' || c == 'U' || c == 'l' || c == 'L' || c == 'z' || c == 'Z' || c == 'i' || c == 'I' {
			i--
			continue
		}
		break
	}
	return s[:i]
}

type expTok struct {
	t    Tok
	hide *hideSet
}

// A hide set is the macros a token must not be expanded by again. It only
// ever grows by one name per expansion and is shared by every token that
// expansion produced, so it is a linked list sharing its tail with its
// parent: one 32-byte cell per expansion, where copying a hash map per
// expansion was 380 MB on PostgreSQL. Sets are as deep as macros nest --
// a handful -- so the walk in has() is shorter than a hash.
type hideSet struct {
	name   string
	parent *hideSet
}

func (h *hideSet) has(n string) bool {
	for ; h != nil; h = h.parent {
		if h.name == n {
			return true
		}
	}
	return false
}

func (pp *preprocessor) lineMayExpand(in []Tok) bool {
	for i := range in {
		if in[i].Kind != TIdent {
			continue
		}
		tx := in[i].Text
		if tx == "__LINE__" || tx == "__FILE__" {
			return true
		}
		if _, ok := pp.macros[tx]; ok {
			return true
		}
	}
	return false
}

// workBuf returns this nesting depth's pending-token stack, emptied, with room
// for n. expandWork hands it back when it finishes.
func (pp *preprocessor) workBuf(n int) []expTok {
	d := pp.expDepth
	for d >= len(pp.reg.workBufs) {
		pp.reg.workBufs = append(pp.reg.workBufs, nil)
		pp.reg.expBufs = append(pp.reg.expBufs, nil)
	}
	w := pp.reg.workBufs[d]
	if cap(w) < n {
		w = make([]expTok, 0, n+n/2+16)
	}
	return w[:0]
}

// expand returns the expansion of in. The result lives in a buffer owned by
// the current nesting depth: it is valid until the next expansion at that
// depth, so a caller that needs two results at once must copy the first.
func (pp *preprocessor) expand(in []Tok) []Tok {
	work := pp.workBuf(len(in))[:len(in)]
	for i := range in {
		work[len(in)-1-i] = expTok{in[i], nil}
	}
	return pp.expandWork(work)
}

func (pp *preprocessor) expandETS(ets []expTok) []Tok {
	work := pp.workBuf(len(ets))
	for i := len(ets) - 1; i >= 0; i-- {
		work = append(work, ets[i])
	}
	return pp.expandWork(work)
}

func (pp *preprocessor) expandWork(work []expTok) []Tok {
	d := pp.expDepth
	pp.expDepth++
	for d >= len(pp.reg.expBufs) {
		pp.reg.workBufs = append(pp.reg.workBufs, nil)
		pp.reg.expBufs = append(pp.reg.expBufs, nil)
	}
	out := pp.reg.expBufs[d][:0]
	defer func() {
		pp.reg.expBufs[d] = out
		pp.reg.workBufs[d] = work[:0]
		pp.expDepth--
	}()
	for len(work) > 0 {
		et := work[len(work)-1]
		work = work[:len(work)-1]
		t := et.t
		if t.Kind == TNewline || t.Kind == TEOF || t.Kind != TIdent || et.hide.has(t.Text) {
			out = append(out, t)
			continue
		}
		m := pp.macros[t.Text]
		if m == nil {
			if nt, ok := pp.nativeBuiltin(t); ok {
				out = append(out, nt)
				continue
			}
			out = append(out, t)
			continue
		}
		if pp.nExpand++; pp.nExpand > maxExpand {
			out = append(out, t)
			continue
		}
		if !m.IsFn {
			m.Uses++
			nh := pp.reg.newHide(m.Name, et.hide)
			sub := pp.subst(m, nil, nh, t)
			for k := len(sub) - 1; k >= 0; k-- {
				work = append(work, expTok{sub[k], nh})
			}
			continue
		}

		j := len(work) - 1
		for j >= 0 && work[j].t.Kind == TNewline {
			j--
		}
		if j < 0 || work[j].t.Kind != TPunct || work[j].t.Punct != PLparen {
			out = append(out, t)
			continue
		}
		args, rest, ok := pp.collectArgs(work[:j])
		if !ok {
			out = append(out, t)
			continue
		}
		work = rest
		m.Uses++
		nh := pp.reg.newHide(m.Name, et.hide)
		sub := pp.subst(m, args, nh, t)
		for k := len(sub) - 1; k >= 0; k-- {
			work = append(work, expTok{sub[k], nh})
		}
	}
	return out
}

// collectArgs splits the tokens after a macro's `(` into its arguments. They
// are gathered into one buffer per nesting depth and sliced apart at the end;
// the caller substitutes them before it can collect again at this depth.
func (pp *preprocessor) collectArgs(work []expTok) ([][]expTok, []expTok, bool) {
	r := pp.reg
	d := pp.expDepth
	for d >= len(r.argToks) {
		r.argToks = append(r.argToks, nil)
		r.argHdrs = append(r.argHdrs, nil)
	}
	buf := r.argToks[d][:0]
	ends := r.argEnds[:0]
	depth := 1
	i := len(work) - 1
	done := false
	for i >= 0 && !done {
		et := work[i]
		t := et.t
		i--
		switch {
		case t.Kind == TEOF:
			r.argToks[d], r.argEnds = buf, ends
			return nil, work, false
		case t.Kind == TPunct && t.Punct == PLparen:
			depth++
			buf = append(buf, et)
		case t.Kind == TPunct && t.Punct == PRparen:
			depth--
			if depth == 0 {
				ends = append(ends, len(buf))
				done = true
			} else {
				buf = append(buf, et)
			}
		case t.Kind == TPunct && t.Punct == PComma && depth == 1:
			ends = append(ends, len(buf))
		case t.Kind == TNewline:
		default:
			buf = append(buf, et)
		}
	}
	r.argToks[d], r.argEnds = buf, ends
	if !done {
		return nil, work, false
	}
	args := r.argHdrs[d][:0]
	prev := 0
	for _, e := range ends {
		args = append(args, buf[prev:e:e])
		prev = e
	}
	r.argHdrs[d] = args
	return args, work[:i+1], true
}

func (pp *preprocessor) subst(m *ppMacro, args [][]expTok, useHide *hideSet, use Tok) []Tok {
	body := m.Body
	// The replacement list is built in a scratch buffer owned by this nesting
	// depth and handed back still pointing into it: every caller copies the
	// tokens out before it can call subst at the same depth again, and a
	// nested expansion runs one level down. Allocating it fresh per expansion
	// was 1.5 GB on PostgreSQL.
	d := pp.subDepth
	pp.subDepth++
	if d >= len(pp.reg.subBufs) {
		pp.reg.subBufs = append(pp.reg.subBufs, nil)
	}
	out := pp.reg.subBufs[d][:0]
	defer func() {
		pp.reg.subBufs[d] = out
		pp.subDepth--
	}()
	vaArgs := [][]expTok(nil)
	if m.IsVariadic && args != nil && len(args) > len(m.Params) {
		vaArgs = args[len(m.Params):]
		args = args[:len(m.Params)]
	}
	if m.IsVariadic && args != nil && len(args) < len(m.Params) {

		for len(args) < len(m.Params) {
			args = append(args, nil)
		}
	}
	for bi := 0; bi < len(body); bi++ {
		bt := body[bi]

		if bt.Kind == TPunct && bt.Punct == PHash && m.IsFn && bi+1 < len(body) {
			pt := body[bi+1]
			if pt.Kind == TIdent {
				if ai := paramIndex(m, pt.Text); ai >= 0 {
					out = append(out, stringize(argToks(args[ai]), use))
					bi++
					continue
				}
			}
		}

		if bt.Kind == TIdent && bt.Text == "__VA_OPT__" && m.IsVariadic &&
			bi+1 < len(body) && body[bi+1].Kind == TPunct && body[bi+1].Punct == PLparen {
			depth := 1
			k := bi + 2
			for k < len(body) && depth > 0 {
				if body[k].Kind == TPunct && body[k].Punct == PLparen {
					depth++
				} else if body[k].Kind == TPunct && body[k].Punct == PRparen {
					depth--
					if depth == 0 {
						break
					}
				}
				k++
			}
			inner := body[bi+2 : min(k, len(body))]
			empty := true
			for _, a := range vaArgs {
				if len(a) > 0 {
					empty = false
					break
				}
			}
			if !empty {
				innerM := &ppMacro{Name: "(va-opt)", Body: inner}
				out = append(out, pp.subst(innerM, args, useHide, use)...)
			}
			bi = k
			continue
		}

		if bt.Kind == TPunct && bt.Punct == PHashAt {
			var right []Tok
			if bi+1 < len(body) {
				nt := body[bi+1]
				right = nil
				isParam := false
				if nt.Kind == TIdent {
					if ai := paramIndex(m, nt.Text); ai >= 0 {
						right = argToks(args[ai])
						isParam = true
					} else if m.IsVariadic && nt.Text == m.VaName {
						right = joinVA(argTokss(vaArgs), use)
						isParam = true
						// GNU `, ## __VA_ARGS__`: the comma survives when
						// there are arguments and is deleted when there are
						// none. It is never pasted onto the first argument.
						if len(out) > 0 && out[len(out)-1].Kind == TPunct &&
							out[len(out)-1].Punct == PComma {
							if len(right) == 0 {
								out = out[:len(out)-1]
							} else {
								out = append(out, right...)
							}
							bi++
							continue
						}
					}
				}
				// an EMPTY argument pastes as nothing; only a token that is
				// not a parameter at all stands for itself
				if right == nil && !isParam {
					right = []Tok{nt}
				}
				bi++
			}
			if len(out) > 0 && len(right) > 0 {
				l := out[len(out)-1]
				out[len(out)-1] = pasteTok(l, right[0], use)
				out = append(out, right[1:]...)
			} else if len(right) > 0 {
				out = append(out, right...)
			} else if len(out) == 0 {

			}
			continue
		}

		if bt.Kind == TIdent {
			if ai := paramIndex(m, bt.Text); ai >= 0 {
				out = append(out, pp.expandETS(args[ai])...)
				continue
			}
			if m.IsVariadic && bt.Text == m.VaName {
				// __VA_ARGS__ is a parameter like any other: its arguments are
				// macro-expanded BEFORE substitution (C23 6.10.5.2). Passing
				// them through raw broke every argument-counting macro --
				// NARG(a, b) handed ARG_N the unexpanded RSEQ_N() as one arg.
				exp := make([][]Tok, len(vaArgs))
				for k, va := range vaArgs {
					// copied: the next argument's expansion reuses the buffer
					exp[k] = append([]Tok(nil), pp.expandETS(va)...)
				}
				out = append(out, joinVA(exp, use)...)
				continue
			}
		}
		out = append(out, bt)
	}

	for k := range out {
		o := out[k]
		o.File = use.File
		o.Off = use.Off
		o.End = use.End
		o.OrigOff = use.OrigOff
		o.Line = use.Line
		o.Col = use.Col
		o.SL = use.SL
		out[k] = o
	}
	return out
}

func argToks(a []expTok) []Tok {
	if len(a) == 0 {
		return nil
	}
	out := make([]Tok, len(a))
	for i, et := range a {
		out[i] = et.t
	}
	return out
}

func argTokss(aa [][]expTok) [][]Tok {
	out := make([][]Tok, len(aa))
	for i, a := range aa {
		out[i] = argToks(a)
	}
	return out
}

func joinVA(vaArgs [][]Tok, use Tok) []Tok {
	var joined []Tok
	for k, a := range vaArgs {
		if k > 0 {
			comma := use
			comma.Kind = TPunct
			comma.Punct = PComma
			comma.Text = ","
			joined = append(joined, comma)
		}
		joined = append(joined, a...)
	}
	return joined
}

func paramIndex(m *ppMacro, name string) int {
	for i, p := range m.Params {
		if p == name {
			return i
		}
	}
	return -1
}

func stringize(arg []Tok, use Tok) Tok {
	var b strings.Builder
	b.WriteByte('"')
	for i, t := range arg {
		if i > 0 && t.WSBefore {
			b.WriteByte(' ')
		}
		s := t.Text
		if t.Kind == TStr {
			s = strings.ReplaceAll(s, `\`, `\\`)
			s = strings.ReplaceAll(s, `"`, `\"`)
		} else if t.Kind == TChar {
			s = strings.ReplaceAll(s, `\`, `\\`)
		}
		b.WriteString(s)
	}
	b.WriteByte('"')
	tk := use
	tk.Kind = TStr
	tk.Text = b.String()
	tk.Punct = 0
	return tk
}

func pasteTok(l, r, use Tok) Tok {
	txt := l.Text + r.Text
	nk := uint8(TIdent)
	np := uint8(0)
	if txt == "" {

	} else if isDigit(txt[0]) || (txt[0] == '.' && len(txt) > 1 && isDigit(txt[1])) {
		nk = TNumber
	} else if c23IsIdentStart(txt[0]) {
		nk = TIdent
	} else if code, ok := punctIndex[txt]; ok {
		nk = TPunct
		np = code
	}
	tk := use
	tk.Kind = nk
	tk.Text = txt
	tk.Punct = np
	return tk
}

func (pp *preprocessor) nativeBuiltin(t Tok) (Tok, bool) {
	switch t.Text {
	case "__LINE__":
		nt := t
		nt.Kind = TNumber
		v := int64(t.Line) + pp.lineDelta
		if v < 1 {
			v = 1
		}
		nt.Text = strconv.FormatInt(v, 10)
		return nt, true
	case "__FILE__":
		nt := t
		nt.Kind = TStr
		nt.Text = strconv.Quote(pp.fileName)
		return nt, true
	}
	return t, false
}

func (pp *preprocessor) evalCond(src *fileTokSrc, toks []Tok, line int32) int64 {
	pre := pp.resolveCondOps(src, toks, line)
	ex := pp.expand(pre)
	v, ok := pp.evalExpr(ex, line)
	if !ok {
		pp.errf(line, "bad #if expression")
		return 0
	}
	return v
}

func (pp *preprocessor) resolveCondOps(src *fileTokSrc, toks []Tok, line int32) []Tok {
	out := make([]Tok, 0, len(toks))
	for i := 0; i < len(toks); i++ {
		t := toks[i]
		if t.Kind != TIdent {
			out = append(out, t)
			continue
		}
		switch t.Text {
		case "defined":
			name := ""
			j := i + 1
			if j < len(toks) && toks[j].Kind == TPunct && toks[j].Punct == PLparen {
				j++
				if j < len(toks) && toks[j].Kind == TIdent {
					name = toks[j].Text
					j++
				}
				if j < len(toks) && toks[j].Kind == TPunct && toks[j].Punct == PRparen {
					j++
				}
			} else if j < len(toks) && toks[j].Kind == TIdent {
				name = toks[j].Text
				j++
			}
			if name == "" {
				pp.errf(line, "bad defined()")
				return out
			}
			_, def := pp.macros[name]
			nt := t
			nt.Kind = TNumber
			nt.Text = btoa(def)
			out = append(out, nt)
			i = j - 1
			continue
		case "__has_include", "__has_include_next", "__has_embed":
			j := i + 1
			if !(j < len(toks) && toks[j].Kind == TPunct && toks[j].Punct == PLparen) {
				out = append(out, t)
				continue
			}

			depth := 1
			k := j + 1
			var inner []Tok
			for k < len(toks) && depth > 0 {
				if toks[k].Kind == TPunct && toks[k].Punct == PLparen {
					depth++
				} else if toks[k].Kind == TPunct && toks[k].Punct == PRparen {
					depth--
					if depth == 0 {
						break
					}
				}
				inner = append(inner, toks[k])
				k++
			}
			val := int64(0)
			if len(inner) > 0 {
				tgt := ""
				sys := false
				if inner[0].Kind == THeader {
					tgt = strings.TrimSuffix(strings.TrimPrefix(inner[0].Text, "<"), ">")
					sys = true
				} else if inner[0].Kind == TStr {
					tgt = dequote(inner[0].Text)
				} else {

					ex := pp.expand(inner)
					if len(ex) > 0 {
						if ex[0].Kind == THeader {
							tgt = strings.TrimSuffix(strings.TrimPrefix(ex[0].Text, "<"), ">")
							sys = true
						} else if ex[0].Kind == TStr {
							tgt = dequote(ex[0].Text)
						}
					}
				}
				if t.Text == "__has_c_attribute" {
					if len(inner) > 0 && inner[0].Kind == TIdent && isC23AttrName(inner[0].Text) {
						val = 202003
					}
				} else if t.Text == "__has_embed" {
					if r := pp.resolveInclude(src, tgt, sys); r != "" {
						val = 1
					}
				} else {
					if r := pp.resolveInclude(src, tgt, sys); r != "" {
						val = 1
					}
				}
			}
			nt := t
			nt.Kind = TNumber
			nt.Text = strconv.FormatInt(val, 10)
			out = append(out, nt)
			i = k
			continue
		default:
			out = append(out, t)
		}
	}
	return out
}

func btoa(b bool) string {
	if b {
		return "1"
	}
	return "0"
}

func (pp *preprocessor) evalExpr(toks []Tok, line int32) (int64, bool) {
	e := &ev{toks: toks, pp: pp, line: line}
	v := e.cond()
	if e.bad || e.pos < len(e.toks) && e.toks[e.pos].Kind != TEOF {

	}
	if e.bad {
		return 0, false
	}
	return v, true
}

type ev struct {
	toks []Tok
	pp   *preprocessor
	line int32
	pos  int
	bad  bool
}

func (e *ev) peek() Tok {
	if e.pos < len(e.toks) {
		return e.toks[e.pos]
	}
	return Tok{Kind: TEOF}
}

func (e *ev) acceptPunct(p uint8) bool {
	t := e.peek()
	if t.Kind == TPunct && t.Punct == p {
		e.pos++
		return true
	}
	return false
}

func (e *ev) cond() int64 {
	c := e.logor()
	if e.acceptPunct(PTerse) {
		a := e.cond()
		if e.acceptPunct(PColon) {
			b := e.cond()
			if c != 0 {
				return a
			}
			return b
		}
		e.bad = true
		return 0
	}
	return c
}

func (e *ev) logor() int64 {
	v := e.logand()
	for {
		if e.acceptPunct(PPipePip) {
			r := e.logand()
			if v != 0 || r != 0 {
				v = 1
			} else {
				v = 0
			}
			continue
		}
		return v
	}
}

func (e *ev) logand() int64 {
	v := e.bitor()
	for {
		if e.acceptPunct(PAmpAmp) {
			r := e.bitor()
			if v != 0 && r != 0 {
				v = 1
			} else {
				v = 0
			}
			continue
		}
		return v
	}
}

func (e *ev) bitor() int64 {
	v := e.bitxor()
	for e.acceptPunct(PPipe) {
		v |= e.bitxor()
	}
	return v
}

func (e *ev) bitxor() int64 {
	v := e.bitand()
	for e.acceptPunct(PCaret) {
		v ^= e.bitand()
	}
	return v
}

func (e *ev) bitand() int64 {
	v := e.equality()
	for e.acceptPunct(PAmp) {
		v &= e.equality()
	}
	return v
}

func (e *ev) equality() int64 {
	v := e.relational()
	for {
		if e.acceptPunct(mustPunct("==")) {
			if v == e.relational() {
				v = 1
			} else {
				v = 0
			}
			continue
		}
		if e.acceptPunct(mustPunct("!=")) {
			if v != e.relational() {
				v = 1
			} else {
				v = 0
			}
			continue
		}
		return v
	}
}

func (e *ev) relational() int64 {
	v := e.shift()
	for {
		if e.acceptPunct(PLe) {
			if v <= e.shift() {
				v = 1
			} else {
				v = 0
			}
			continue
		}
		if e.acceptPunct(mustPunct(">=")) {
			if v >= e.shift() {
				v = 1
			} else {
				v = 0
			}
			continue
		}
		if e.acceptPunct(PLt) {
			if v < e.shift() {
				v = 1
			} else {
				v = 0
			}
			continue
		}
		if e.acceptPunct(PGt) {
			if v > e.shift() {
				v = 1
			} else {
				v = 0
			}
			continue
		}
		return v
	}
}

func (e *ev) shift() int64 {
	v := e.additive()
	for {
		if e.acceptPunct(PLsh) {
			v <<= uint64(e.additive())
			continue
		}
		if e.acceptPunct(PRsh) {
			v >>= uint64(e.additive())
			continue
		}
		return v
	}
}

func (e *ev) additive() int64 {
	v := e.mult()
	for {
		if e.acceptPunct(PPlus) {
			v += e.mult()
			continue
		}
		if e.acceptPunct(PMinus) {
			v -= e.mult()
			continue
		}
		return v
	}
}

func (e *ev) mult() int64 {
	v := e.unary()
	for {
		if e.acceptPunct(PStar) {
			v *= e.unary()
			continue
		}
		if e.acceptPunct(PSlash) {
			d := e.unary()
			if d == 0 {
				e.pp.errf(e.line, "division by zero in #if")
				e.bad = true
				return 0
			}
			v /= d
			continue
		}
		if e.acceptPunct(PMod) {
			d := e.unary()
			if d == 0 {
				e.pp.errf(e.line, "mod by zero in #if")
				e.bad = true
				return 0
			}
			v %= d
			continue
		}
		return v
	}
}

func (e *ev) unary() int64 {
	if e.acceptPunct(PBang) {
		if e.unary() != 0 {
			return 0
		}
		return 1
	}
	if e.acceptPunct(PTilde) {
		return ^e.unary()
	}
	if e.acceptPunct(PMinus) {
		return -e.unary()
	}
	if e.acceptPunct(PPlus) {
		return e.unary()
	}
	return e.primary()
}

func (e *ev) primary() int64 {
	t := e.peek()
	switch t.Kind {
	case TNumber:
		e.pos++
		if v, ok := parseCInt(t.Text); ok {
			return v
		}
		e.pp.errf(e.line, "bad number in #if: "+t.Text)
		e.bad = true
		return 0
	case TChar:
		e.pos++
		return charValue(t.Text)
	case TIdent:
		e.pos++
		switch t.Text {
		case "true":
			return 1
		case "false":
			return 0
		}
		// Still an identifier after expansion, so no header in the tree
		// defined it. For the <limits.h>/<stdint.h> limits that is because the
		// system header was not read, not because the value is 0 -- and code
		// that selects on UINTPTR_MAX otherwise falls through to its #error.
		if v, ok := condLimits[t.Text]; ok {
			return v
		}
		return 0
	case TPunct:
		if t.Punct == PLparen {
			e.pos++
			v := e.cond()
			if !e.acceptPunct(PRparen) {
				e.bad = true
				return 0
			}
			return v
		}
	}
	e.bad = true
	return 0
}

// condLimits is the LP64 model already stated in meta.layout_model. Unsigned
// 64-bit maxima are stored as the bit pattern parseCInt gives the literal, so
// `UINTPTR_MAX == 0xffffffffffffffff` compares equal.
var condLimits = func() map[string]int64 {
	u64, _ := parseCInt("0xffffffffffffffff")
	const i64 = int64(9223372036854775807)
	return map[string]int64{
		"CHAR_BIT": 8, "SCHAR_MAX": 127, "SCHAR_MIN": -128, "UCHAR_MAX": 255,
		"CHAR_MAX": 127, "CHAR_MIN": -128,
		"SHRT_MAX": 32767, "SHRT_MIN": -32768, "USHRT_MAX": 65535,
		"INT_MAX": 2147483647, "INT_MIN": -2147483648, "UINT_MAX": 4294967295,
		"LONG_MAX": i64, "LONG_MIN": -i64 - 1, "ULONG_MAX": u64,
		"LLONG_MAX": i64, "LLONG_MIN": -i64 - 1, "ULLONG_MAX": u64,
		"INT8_MAX": 127, "INT16_MAX": 32767, "INT32_MAX": 2147483647, "INT64_MAX": i64,
		"INT8_MIN": -128, "INT16_MIN": -32768, "INT32_MIN": -2147483648, "INT64_MIN": -i64 - 1,
		"UINT8_MAX": 255, "UINT16_MAX": 65535, "UINT32_MAX": 4294967295, "UINT64_MAX": u64,
		"INTPTR_MAX": i64, "INTPTR_MIN": -i64 - 1, "UINTPTR_MAX": u64,
		"INTMAX_MAX": i64, "INTMAX_MIN": -i64 - 1, "UINTMAX_MAX": u64,
		"SIZE_MAX": u64, "SSIZE_MAX": i64, "PTRDIFF_MAX": i64, "PTRDIFF_MIN": -i64 - 1,
		"__WORDSIZE": 64, "__SIZEOF_POINTER__": 8, "__SIZEOF_INT__": 4,
		"__SIZEOF_SHORT__": 2, "__SIZEOF_LONG_LONG__": 8, "__SIZEOF_SIZE_T__": 8,
		"__LP64__": 1, "_LP64": 1,
		// what AC_CHECK_SIZEOF / AC_CHECK_ALIGNOF write into a generated
		// config.h, under the same LP64 model; the generated header is never
		// in a source tree, and code that selects on these ends in #error
		"SIZEOF_CHAR": 1, "SIZEOF_SHORT": 2, "SIZEOF_INT": 4, "SIZEOF_LONG": 8,
		"SIZEOF_LONG_LONG": 8, "SIZEOF_LONG_INT": 8, "SIZEOF_LONG_LONG_INT": 8,
		"SIZEOF_VOID_P": 8, "SIZEOF_VOIDP": 8, "SIZEOF_SIZE_T": 8,
		"SIZEOF_OFF_T": 8, "SIZEOF_TIME_T": 8, "SIZEOF_PTRDIFF_T": 8,
		"SIZEOF_INTPTR_T": 8, "SIZEOF_UINTPTR_T": 8, "SIZEOF_FLOAT": 4,
		"SIZEOF_DOUBLE": 8, "SIZEOF_INT64_T": 8, "SIZEOF_INT32_T": 4,
		"ALIGNOF_SHORT": 2, "ALIGNOF_INT": 4, "ALIGNOF_LONG": 8,
		"ALIGNOF_LONG_LONG_INT": 8, "ALIGNOF_DOUBLE": 8, "ALIGNOF_INT64_T": 8,
		"MAXIMUM_ALIGNOF": 8,
	}
}()

func parseCInt(s string) (int64, bool) {
	s = strings.ReplaceAll(s, "'", "")
	s = stripNumSuffix(s)
	if s == "" {
		return 0, false
	}
	neg := false
	base := 10
	body := s
	if strings.HasPrefix(body, "+") {
		body = body[1:]
	} else if strings.HasPrefix(body, "-") {
		neg = true
		body = body[1:]
	}
	_ = base
	if strings.HasPrefix(body, "0x") || strings.HasPrefix(body, "0X") {
		base = 16
		body = body[2:]
	} else if strings.HasPrefix(body, "0b") || strings.HasPrefix(body, "0B") {
		base = 2
		body = body[2:]
	} else if len(body) > 1 && body[0] == '0' {
		base = 8
		body = body[1:]
	}
	var v int64
	for i := 0; i < len(body); i++ {
		var d int64
		c := body[i]
		switch {
		case c >= '0' && c <= '9':
			d = int64(c - '0')
		case c >= 'a' && c <= 'z':
			d = int64(c-'a') + 10
		case c >= 'A' && c <= 'Z':
			d = int64(c-'A') + 10
		default:
			return 0, false
		}
		if d >= int64(base) {
			return 0, false
		}
		v = v*int64(base) + d
	}
	if neg {
		v = -v
	}
	return v, true
}

func charValue(s string) int64 {
	if len(s) < 2 {
		return 0
	}

	if s[0] == '\'' {
		s = s[1:]
	} else if s[0] == 'u' && len(s) > 1 && s[1] == '8' {
		s = s[3:]
	} else if s[0] == 'u' || s[0] == 'U' || s[0] == 'L' {
		s = s[2:]
	}
	s = strings.TrimSuffix(s, "'")
	var v int64
	i := 0
	for i < len(s) {
		c, adv := readCharVal(s, i)
		v = v<<8 | (c & 0xFF)
		i += adv
	}
	return v
}

func readCharVal(s string, i int) (int64, int) {
	if s[i] == '\\' && i+1 < len(s) {
		switch s[i+1] {
		case 'n':
			return 10, 2
		case 't':
			return 9, 2
		case 'r':
			return 13, 2
		case 'a':
			return 7, 2
		case 'b':
			return 8, 2
		case 'f':
			return 12, 2
		case 'v':
			return 11, 2
		case '\\':
			return '\\', 2
		case '\'':
			return '\'', 2
		case '"':
			return '"', 2
		case '?':
			return '?', 2
		case 'e':
			return 27, 2
		case 'x':
			v := int64(0)
			j := i + 2
			for j < len(s) && isHex(s[j]) {
				v = v*16 + hexVal(s[j])
				j++
			}
			return v, j - i
		case 'u', 'U':
			nd := 4
			if s[i+1] == 'U' {
				nd = 8
			}
			v := int64(0)
			j := i + 2
			for k := 0; k < nd && j < len(s) && isHex(s[j]); k++ {
				v = v*16 + hexVal(s[j])
				j++
			}
			return v, j - i
		default:
			if s[i+1] >= '0' && s[i+1] <= '7' {
				v := int64(0)
				j := i + 1
				for k := 0; k < 3 && j < len(s) && s[j] >= '0' && s[j] <= '7'; k++ {
					v = v*8 + int64(s[j]-'0')
					j++
				}
				return v, j - i
			}
			return int64(s[i+1]), 2
		}
	}
	if s[i] < 0x80 {
		return int64(s[i]), 1
	}

	r := int64(s[i]) & 0x3F
	n := 1
	if s[i]&0xE0 == 0xC0 {
		n = 2
	} else if s[i]&0xF0 == 0xE0 {
		n = 3
	} else if s[i]&0xF8 == 0xF0 {
		n = 4
	}
	j := i + 1
	for k := 1; k < n && j < len(s); k++ {
		r = r<<6 | int64(s[j])&0x3F
		j++
	}
	return r, j - i
}

func hexVal(c byte) int64 {
	switch {
	case c >= '0' && c <= '9':
		return int64(c - '0')
	case c >= 'a' && c <= 'f':
		return int64(c-'a') + 10
	default:
		return int64(c-'A') + 10
	}
}

const (
	NBad = iota

	NTU
	NFuncDef
	NDecl
	NParam
	NKRDecl
	NStructSpec
	NUnionSpec
	NEnumSpec
	NField
	NEnumerator
	NTypeRef
	NStaticAssert

	NCompoundStmt
	NDeclStmt
	NExprStmt
	NIf
	NSwitch
	NCase
	NWhile
	NDo
	NFor
	NReturn
	NBreak
	NContinue
	NGoto
	NLabel
	NAsm

	NIdent
	NNumLit
	NCharLit
	NStrLit
	NBoolLit
	NNullPtr
	NCall
	NMember
	NIndex
	NPostfix
	NUnary
	NBinary
	NAssign
	NCond
	NComma
	NCast
	NSizeofT
	NSizeofE
	NAlignofT
	NGeneric
	NStmtExpr
	NParen
	NCompoundLit
	NVaArg
	NInitList
	NInitItem
)

const (
	FConst     = 1 << 0
	FVolatile  = 1 << 1
	FStatic    = 1 << 2
	FExtern    = 1 << 3
	FInline    = 1 << 4
	FAuto      = 1 << 5
	FRegister  = 1 << 6
	FThreadLoc = 1 << 7
	FConstexpr = 1 << 8
	FRestrict  = 1 << 9
	FAtomic    = 1 << 10
	FVariadic  = 1 << 11
	FNoreturn  = 1 << 12
	FKRStyle   = 1 << 13
	FHasInit   = 1 << 14
	FAttrHere  = 1 << 15
)

type Node struct {
	Kind       uint16
	Flags      uint16
	A, B, C, D uint32
	File       uint32
	Off, End   int32
	Line, Col  int32
}

type FuncDef struct {
	NameIdx      uint32
	SigIdx       uint32
	RetIdx       uint32
	Param0       uint32
	NParams      uint32
	Body         uint32
	LineEnd      int32
	Storage      uint16
	Variadic     bool
	KR           bool
	Attr0, NAttr uint32
}

type DeclNode struct {
	NameIdx  uint32
	TypeIdx  uint32
	Typename uint32
	Bitfield int32
	ArrayLen int32
	Ptr      int32
	Init     uint32
	Storage  uint16
	Kind     uint8
	Line     int32
}

type ParamD struct {
	NameIdx  uint32
	TypeIdx  uint32
	Ptr      int32
	ArrayLen int32
	Storage  uint16
	Variadic bool
	Unnamed  bool
	IsConst  bool
	Line     int32
}

type TagD struct {
	NameIdx       uint32
	Tag           string
	Child0        uint32
	NChild        uint32
	UnderlyingIdx uint32
	HasUnderlying bool
	Defined       bool
	Line          int32
	LineEnd       int32
}

type FieldD struct {
	NameIdx    uint32
	TypeIdx    uint32
	Ptr        int32
	PtrTypeIdx uint32
	ArrayLen   int32
	Bitfield   int32
	IsFnptr    bool
	Depth      int32
	IsConst    bool
	Line       int32
}

type EnumD struct {
	NameIdx  uint32
	ValueIdx uint32
	HasValue bool
	Line     int32
}

type TypeD struct {
	TextIdx  uint32
	Ptr      int32
	Array    bool
	ArrayLen int32
	IsFn     bool
}

type InitD struct {
	FieldIdx uint32
	Index    int64
	HasIndex bool
	IsList   bool
}

type GenericAssoc struct {
	TypeIdx   uint32
	IsDefault bool
	Expr      uint32
}

type AttrRec struct {
	Name string
	Args string
	Line int32

	Off, End int32
}

type FileAST struct {
	Names  []string
	nameIx map[string]uint32

	Nodes []Node

	Funcs  []FuncDef
	Decls  []DeclNode
	Params []ParamD
	Tags   []TagD
	Fields []FieldD
	Enums  []EnumD
	Types  []TypeD
	Inits  []InitD
	Assocs []GenericAssoc

	Attrs []AttrRec

	SrcOff, SrcEnd []uint32
}

// A unit's tree is dead the moment its rows are derived, and the next unit
// needs one the same shape. Handing the arrays and the intern map on -- the
// map keeps its buckets through clear() -- replaces what was, on PostgreSQL,
// 2.3 GB of allocation in spans big enough to queue on the heap lock.
var (
	faFreeMu sync.Mutex
	faFree   []*FileAST
)

func getFA() *FileAST {
	faFreeMu.Lock()
	var fa *FileAST
	if n := len(faFree); n > 0 {
		fa = faFree[n-1]
		faFree[n-1] = nil
		faFree = faFree[:n-1]
	}
	faFreeMu.Unlock()
	if fa == nil {
		return &FileAST{}
	}
	return fa
}

func putFA(fa *FileAST) {
	if fa == nil {
		return
	}
	// strings held by the old rows stay valid: only the slots are reused
	clear(fa.Names)
	fa.Names = fa.Names[:0]
	clear(fa.nameIx)
	fa.Nodes = fa.Nodes[:0]
	fa.Funcs = fa.Funcs[:0]
	fa.Decls = fa.Decls[:0]
	fa.Params = fa.Params[:0]
	clear(fa.Tags)
	fa.Tags = fa.Tags[:0]
	fa.Fields = fa.Fields[:0]
	fa.Enums = fa.Enums[:0]
	fa.Types = fa.Types[:0]
	fa.Inits = fa.Inits[:0]
	fa.Assocs = fa.Assocs[:0]
	clear(fa.Attrs)
	fa.Attrs = fa.Attrs[:0]
	fa.SrcOff, fa.SrcEnd = fa.SrcOff[:0], fa.SrcEnd[:0]
	faFreeMu.Lock()
	if len(faFree) < 32 {
		faFree = append(faFree, fa)
	}
	faFreeMu.Unlock()
}

func (fa *FileAST) intern(s string) uint32 {
	if fa.nameIx == nil {
		fa.nameIx = map[string]uint32{}
	}
	if ix, ok := fa.nameIx[s]; ok {
		return ix
	}
	fa.Names = append(fa.Names, s)
	ix := uint32(len(fa.Names))
	fa.nameIx[s] = ix
	return ix
}

func (fa *FileAST) name(ix uint32) string {
	if ix == 0 || int(ix) > len(fa.Names) {
		return ""
	}
	return fa.Names[ix-1]
}

func (fa *FileAST) addNode(n Node) uint32 {
	fa.Nodes = append(fa.Nodes, n)
	return uint32(len(fa.Nodes)) - 1
}

func (fa *FileAST) span(i uint32) (int32, int32) {
	return fa.Nodes[i].Off, fa.Nodes[i].End
}

type parseError struct {
	line int32
	file uint32
	msg  string
}

// A scope's tables are made on first write. Nearly every block declares no
// typedef, tag or enumerator, and three maps per `{` was a million maps on a
// large tree.
type scope struct {
	typedefs   map[string]bool
	tags       map[string]bool
	enumConsts map[string]bool
}

func (sc *scope) setTypedef(name string, v bool) {
	if sc.typedefs == nil {
		sc.typedefs = map[string]bool{}
	}
	sc.typedefs[name] = v
}

type parser struct {
	fa      *FileAST
	toks    []Tok
	pos     int
	errs    *[]parseError
	srcs    map[uint32][]byte
	fileID  uint32
	scopes  []scope
	nErrors int
	stop    bool

	funcDepth int
	loopDepth int
	krMode    bool

	deferredTypedefs []defName
	deferredTags     []defName
	deferredConsts   []defName
}

type defName struct {
	name  string
	scope int
}

func newParser(fa *FileAST, toks []Tok, srcs map[uint32][]byte, fileID uint32, errs *[]parseError) *parser {
	p := &parser{fa: fa, toks: toks, srcs: srcs, fileID: fileID, errs: errs}
	p.pushScope()
	return p
}

func (p *parser) srcOf(t Tok) []byte {
	if b, ok := p.srcs[uint32(t.File)]; ok {
		return b
	}
	if len(p.srcs) == 1 {
		for _, b := range p.srcs {
			return b
		}
	}
	return nil
}

func (p *parser) pushScope() {
	p.scopes = append(p.scopes, scope{})
}

func (p *parser) popScope() {
	if len(p.scopes) > 1 {
		p.scopes = p.scopes[:len(p.scopes)-1]
	}
}

func (p *parser) cur() Tok {
	if p.pos < len(p.toks) {
		return p.toks[p.pos]
	}
	return p.toks[len(p.toks)-1]
}

func (p *parser) peek(n int) Tok {
	if p.pos+n < len(p.toks) {
		return p.toks[p.pos+n]
	}
	return p.toks[len(p.toks)-1]
}

func (p *parser) eof() bool { return p.cur().Kind == TEOF }

func (p *parser) advance() Tok {
	t := p.cur()
	if p.pos < len(p.toks)-1 {
		p.pos++
	}
	return t
}

func (p *parser) atPunct(code uint8) bool {
	t := p.cur()
	return t.Kind == TPunct && t.Punct == code
}

func (p *parser) atIdent(s string) bool {
	t := p.cur()
	return t.Kind == TIdent && t.Text == s
}

func (p *parser) acceptPunct(code uint8) bool {
	if p.atPunct(code) {
		p.advance()
		return true
	}
	return false
}

func (p *parser) acceptIdent(s string) bool {
	if p.atIdent(s) {
		p.advance()
		return true
	}
	return false
}

func (p *parser) errf(t Tok, msg string) {
	if p.errs != nil && len(*p.errs) < 4096 {
		*p.errs = append(*p.errs, parseError{line: t.Line, file: uint32(t.File), msg: msg})
	}
	p.nErrors++
}

func (p *parser) syncTo(t Tok, what string) {
	p.errf(t, "parse error near '"+t.Text+"' ("+what+"); syncing")
	depth := 0
	line := t.SL
	for !p.eof() {
		c := p.cur()
		if c.Kind == TPunct {
			switch c.Punct {
			case PLbrace:
				depth++
			case PRbrace:
				if depth == 0 {
					return
				}
				depth--
			case PSemi:
				if depth == 0 {
					p.advance()
					return
				}
			}
		}
		if c.SL > line && depth == 0 {
			return
		}
		p.advance()
	}
}

func (p *parser) expectPunct(code uint8, what string) bool {
	if p.acceptPunct(code) {
		return true
	}
	p.errf(p.cur(), "expected '"+punctTable[code]+"' "+what)
	return false
}

func (p *parser) closeParamList() {
	if p.acceptPunct(PRparen) {
		return
	}
	p.errf(p.cur(), "expected ')' to close parameter list; recovering")
	depth := 0
	for !p.eof() {
		t := p.cur()
		if t.Kind == TPunct {
			switch t.Punct {
			case PLparen, PLbrack:
				depth++
			case PRparen, PRbrack:
				if depth == 0 && t.Punct == PRparen {
					p.advance()
					return
				}
				if depth > 0 {
					depth--
				}
			case PLbrace, PSemi:
				if depth == 0 {
					return
				}
			}
		}
		p.advance()
	}
}

var sysTypedefs = map[string]bool{
	"size_t": true, "ssize_t": true, "ptrdiff_t": true, "intptr_t": true,
	"uintptr_t": true, "int8_t": true, "int16_t": true, "int32_t": true,
	"int64_t": true, "uint8_t": true, "uint16_t": true, "uint32_t": true,
	"uint64_t": true, "int_least8_t": true, "int_least16_t": true,
	"int_least32_t": true, "int_least64_t": true, "uint_least8_t": true,
	"uint_least16_t": true, "uint_least32_t": true, "uint_least64_t": true,
	"int_fast8_t": true, "int_fast16_t": true, "int_fast32_t": true,
	"int_fast64_t": true, "uint_fast8_t": true, "uint_fast16_t": true,
	"uint_fast32_t": true, "uint_fast64_t": true, "intmax_t": true,
	"uintmax_t": true, "off_t": true, "time_t": true, "clock_t": true,
	"pid_t": true, "uid_t": true, "gid_t": true, "ino_t": true, "dev_t": true,
	"mode_t": true, "nlink_t": true, "blkcnt_t": true, "blksize_t": true,
	"socklen_t": true, "sa_family_t": true, "in_addr_t": true,
	"in_port_t": true, "useconds_t": true, "suseconds_t": true,
	"key_t": true, "id_t": true, "va_list": true, "FILE": true, "DIR": true,
	"fpos_t": true, "sigset_t": true, "siginfo_t": true, "regex_t": true,
	"regoff_t": true, "pthread_t": true, "pthread_mutex_t": true,
	"pthread_rwlock_t": true, "pthread_cond_t": true, "pthread_key_t": true,
	"pthread_once_t": true, "pthread_attr_t": true, "pthread_spinlock_t": true,
	"socket_t": true, "nfds_t": true, "speed_t": true, "tcflag_t": true,
	"cc_t": true, "wchar_t": true, "wint_t": true, "wctype_t": true,
	"char16_t": true, "char32_t": true, "max_align_t": true, "div_t": true,
	"ldiv_t": true, "lldiv_t": true, "tm": true, "timespec": true,
	"timeval": true, "stat": true, "stat64": true, "dirent": true,
	"rlim_t": true, "rlimit": true, "rlimit64": true, "stack_t": true,
	"ucontext_t": true, "errno_t": true, "rsize_t": true,
}

func (p *parser) isTypedefName(s string) bool {
	// innermost entry wins: `false` is a parameter or local that hides the
	// type name for the rest of its scope (see addTypedefGuard)
	for i := len(p.scopes) - 1; i >= 0; i-- {
		if v, ok := p.scopes[i].typedefs[s]; ok {
			return v
		}
	}
	return sysTypedefs[s]
}

// A type name this run never saw defined -- a typedef from a header outside
// the tree, or one behind an include path nobody passed -- is an ordinary
// identifier to the grammar, and `sds s = sdsempty();` then fails as an
// expression and takes the rest of the function with it. These three helpers
// recognise the shapes that cannot be anything BUT a use of a type name, and
// record the name so later uses in the same translation unit parse directly.

// strMacroHere: inside a run of adjacent string literals, an identifier that
// is not being called. No expression allows a name directly after a string,
// so it is a string-valued macro (PRId64, a project's FMT_x) that this run
// could not expand; it joins the literal instead of ending the parse.
func (p *parser) strMacroHere() bool {
	t := p.cur()
	if t.Kind != TIdent || cKeywords[t.Text] {
		return false
	}
	nx := p.peek(1)
	return !(nx.Kind == TPunct && nx.Punct == PLparen)
}

func (p *parser) plainIdent(t Tok) bool {
	return t.Kind == TIdent && !cKeywords[t.Text] && !p.isTypedefName(t.Text)
}

// skipPtrQuals returns the lookahead index after any `*` and qualifiers
// starting at j, and how many stars it passed.
func (p *parser) skipPtrQuals(j int) (int, int) {
	stars := 0
	for {
		x := p.peek(j)
		if x.Kind == TPunct && x.Punct == PStar {
			stars++
			j++
			continue
		}
		if x.Kind == TIdent {
			switch x.Text {
			case "const", "volatile", "restrict", "__restrict", "__restrict__":
				j++
				continue
			}
		}
		return j, stars
	}
}

// inferTypedef: at the start of a statement, `T name =`, `T name;`,
// `T name,`, `T name[` and the same with stars between. `a * b;` is a legal
// expression statement and a useless one; no real code is lost reading it as
// a declaration.
func (p *parser) inferTypedef() bool {
	t := p.cur()
	if !p.plainIdent(t) {
		return false
	}
	j, _ := p.skipPtrQuals(1)
	nm := p.peek(j)
	if nm.Kind != TIdent || cKeywords[nm.Text] {
		return false
	}
	nx := p.peek(j + 1)
	if nx.Kind != TPunct {
		return false
	}
	switch nx.Punct {
	case PEq, PSemi, PComma, PLbrack:
	default:
		return false
	}
	p.scopes[0].setTypedef(t.Text, true)
	return true
}

// inferCastType: with the cursor on `(`, the shapes `(T *)`, `(T){`,
// `(T[n]){` and `(T) operand` where the operand starts with a name or a
// literal. Two primaries cannot be adjacent in an expression, so each of
// these is a cast or a compound literal. `(T)(x)`, `(T)*p`, `(T)-1` and
// `(T)&x` stay ambiguous and are left alone.
func (p *parser) inferCastType() bool {
	t := p.peek(1)
	if !p.plainIdent(t) {
		return false
	}
	j, stars := p.skipPtrQuals(2)
	arr := false
	if x := p.peek(j); x.Kind == TPunct && x.Punct == PLbrack {
		k := j + 1
		for n := 0; n < 6; n++ {
			y := p.peek(k)
			if y.Kind == TPunct && y.Punct == PRbrack {
				break
			}
			k++
		}
		if y := p.peek(k); y.Kind != TPunct || y.Punct != PRbrack {
			return false
		}
		arr = true
		j = k + 1
	}
	if x := p.peek(j); x.Kind != TPunct || x.Punct != PRparen {
		return false
	}
	nx := p.peek(j + 1)
	ok := false
	switch {
	case nx.Kind == TPunct && nx.Punct == PLbrace:
		ok = true
	case arr:
	case stars > 0:
		ok = true
	case nx.Kind == TNumber || nx.Kind == TStr || nx.Kind == TChar:
		ok = true
	case nx.Kind == TIdent:
		ok = !cKeywords[nx.Text] || nx.Text == "sizeof"
	}
	if ok {
		p.scopes[0].setTypedef(t.Text, true)
	}
	return ok
}

// inferSizeofType: `sizeof(T *)` and `sizeof(T[n])`. `sizeof(T)` alone is
// left as an expression -- it is counted the same either way.
func (p *parser) inferSizeofType() bool {
	t := p.peek(1)
	if !p.plainIdent(t) {
		return false
	}
	j, stars := p.skipPtrQuals(2)
	if stars == 0 {
		return false
	}
	if x := p.peek(j); x.Kind != TPunct || x.Punct != PRparen {
		return false
	}
	p.scopes[0].setTypedef(t.Text, true)
	return true
}

func (p *parser) addTypedef(name string) {
	p.scopes[len(p.scopes)-1].setTypedef(name, true)
}

func (p *parser) addTag(name string) {
	sc := &p.scopes[len(p.scopes)-1]
	if sc.tags == nil {
		sc.tags = map[string]bool{}
	}
	sc.tags[name] = true
}

func (p *parser) addEnumConst(name string) {
	sc := &p.scopes[len(p.scopes)-1]
	if sc.enumConsts == nil {
		sc.enumConsts = map[string]bool{}
	}
	sc.enumConsts[name] = true
}

type spanMark struct{ pos int }

const (
	BVoid = iota
	BBool
	BChar
	BShort
	BInt
	BLong
	BLongLong
	BFloat
	BDouble
	BSigned
	BUnsigned
	BDecimal32
	BDecimal64
	BDecimal128
	BTypedef
	BStruct
	BUnion
	BEnum
	BTypeof
	BBitInt
	BAuto
	BNone
)

type tspecs struct {
	words                                 []string
	base                                  string
	kind                                  int
	tag                                   string
	tagNode                               uint32
	typedef                               string
	isTypedef                             bool
	hasBase                               bool
	constq, volatileq, restrictq, atomicq bool
	alignas                               bool
	storage                               uint16
	saw                                   bool
}

func (ts *tspecs) baseText() string {
	// nine declarations in ten carry no qualifier: the base IS the text
	if !ts.constq && !ts.volatileq && !ts.restrictq && !ts.atomicq {
		return strings.TrimRight(ts.base, " ")
	}
	var b strings.Builder
	if ts.constq {
		b.WriteString("const ")
	}
	if ts.volatileq {
		b.WriteString("volatile ")
	}
	if ts.restrictq {
		b.WriteString("restrict ")
	}
	if ts.atomicq {
		b.WriteString("_Atomic ")
	}
	b.WriteString(ts.base)
	return strings.TrimRight(b.String(), " ")
}

func (p *parser) recordAttr(name, args string, line int32, off, end int32) {
	p.fa.Attrs = append(p.fa.Attrs, AttrRec{Name: name, Args: args, Line: line,
		Off: off, End: end})
}

func (p *parser) skipBalanced() (string, int32, int32) {
	if !p.atPunct(PLparen) {
		return "", 0, 0
	}
	startTok := p.advance()
	depth := 1
	var b strings.Builder
	first := startTok.Off
	for !p.eof() && depth > 0 {
		t := p.cur()
		if t.Kind == TPunct {
			switch t.Punct {
			case PLparen:
				depth++
			case PRparen:
				depth--
				if depth == 0 {
					end := t.End
					p.advance()
					return b.String(), first, end
				}

				p.advance()
				continue
			}
		}
		if b.Len() > 0 || t.Kind != TPunct || t.Punct != PLparen {
			if b.Len() > 0 && t.WSBefore {
				b.WriteByte(' ')
			}
			b.WriteString(t.Text)
		}
		p.advance()
	}
	return b.String(), first, startTok.End
}

func (p *parser) parseAttributes() {
	for {
		t := p.cur()
		if t.Kind == TPunct && t.Punct == mustPunct("[") &&
			p.peek(1).Kind == TPunct && p.peek(1).Punct == mustPunct("[") {
			p.advance()
			p.advance()
			depth := 2
			var name, args strings.Builder
			gotName := false
			scoped := false
			for !p.eof() && depth > 0 {
				c := p.cur()
				if c.Kind == TPunct && c.Punct == mustPunct("[") {
					depth++
				} else if c.Kind == TPunct && c.Punct == mustPunct("]") {
					depth--
					p.advance()
					continue
				}
				if depth == 2 {
					if c.Kind == TIdent {
						if gotName && !scoped {
							if args.Len() > 0 && c.WSBefore {
								args.WriteByte(' ')
							}
							args.WriteString(c.Text)
						} else {
							name.WriteString(c.Text)
							gotName = true
							scoped = false
						}
					} else if c.Kind == TPunct {

						if c.Punct == PColon && gotName && args.Len() == 0 &&
							p.peek(1).Kind == TPunct && p.peek(1).Punct == PColon {
							name.WriteString("::")
							scoped = true
							p.advance()
							p.advance()
							continue
						}
						if args.Len() > 0 || gotName {
							args.WriteString(c.Text)
						}
					} else {
						args.WriteString(c.Text)
					}
				}
				p.advance()
			}

			if args.Len() > 0 {
				p.recordAttr(name.String(), args.String(), t.Line, t.Off, t.End)
			} else {
				p.recordAttr(name.String(), "", t.Line, t.Off, t.End)
			}
			continue
		}
		if t.Kind == TIdent && (t.Text == "__attribute__" || t.Text == "__attribute") {
			p.advance()
			args, off, end := p.skipBalanced()
			p.recordAttr("__attribute__", args, t.Line, off, end)
			continue
		}
		return
	}
}

func (p *parser) skipAsm() {

	for p.atIdent("asm") || p.atIdent("__asm__") || p.atIdent("__asm") ||
		p.atIdent("volatile") || p.atIdent("__volatile__") || p.atIdent("inline") ||
		p.atIdent("goto") {
		name := p.cur()
		p.advance()
		if p.atPunct(PLparen) {
			_, off, end := p.skipBalanced()
			p.recordAttr(name.Text, asmText(p.srcOf(name), off, end), name.Line, off, end)
		}
	}
}

func asmText(src []byte, off, end int32) string {
	if int(off) < len(src) && int(end) <= len(src) && off < end {
		return squeeze(string(src[off:end]), "")
	}
	return ""
}

func (p *parser) parseDeclSpecs() tspecs {
	var ts tspecs
	ts.kind = BNone
	for !p.eof() {
		t := p.cur()
		if t.Kind != TIdent && !(t.Kind == TPunct && (t.Punct == mustPunct("[") || t.Punct == mustPunct("]"))) {
			if t.Kind != TIdent {

				break
			}
		}
		if t.Kind != TIdent {
			break
		}
		switch t.Text {

		case "typedef":
			ts.storage |= 0
			ts.saw = true
			ts.typedef = "typedef"
			ts.isTypedef = true
			p.advance()
			continue
		case "extern":
			ts.storage |= FExtern
			ts.saw = true
			p.advance()
			continue
		case "static":
			ts.storage |= FStatic
			ts.saw = true
			p.advance()
			continue
		case "auto":
			ts.storage |= FAuto
			ts.saw = true
			ts.kind = BAuto
			ts.base = "auto"
			ts.hasBase = true
			p.advance()
			continue
		case "register":
			ts.storage |= FRegister
			ts.saw = true
			p.advance()
			continue
		case "thread_local", "_Thread_local", "__thread":
			ts.storage |= FThreadLoc
			ts.saw = true
			p.advance()
			continue
		case "constexpr":
			ts.storage |= FConstexpr
			ts.saw = true
			p.advance()
			continue

		case "inline", "__inline", "__inline__":
			ts.storage |= FInline
			ts.saw = true
			p.advance()
			continue
		case "_Noreturn", "noreturn":
			ts.storage |= FNoreturn
			ts.saw = true
			p.advance()
			continue

		case "const", "__const", "__const__":
			ts.constq = true
			ts.saw = true
			p.advance()
			continue
		case "volatile", "__volatile", "__volatile__":
			ts.volatileq = true
			ts.saw = true
			p.advance()
			continue
		case "restrict", "__restrict", "__restrict__":
			ts.restrictq = true
			ts.saw = true
			p.advance()
			continue
		case "_Atomic":

			if p.peek(1).Kind == TPunct && p.peek(1).Punct == PLparen {
				p.advance()
				p.advance()
				inner := p.parseTypeName()
				p.expectPunct(PRparen, "after _Atomic(")
				ts.base = "_Atomic(" + inner.text + ")"
				ts.kind = BTypeof
				ts.hasBase = true
				ts.saw = true
				continue
			}
			ts.atomicq = true
			ts.saw = true
			p.advance()
			continue
		case "alignas", "_Alignas":
			p.advance()
			p.skipBalanced()
			ts.alignas = true
			ts.saw = true
			continue
		case "__extension__":
			p.advance()
			continue
		case "__attribute__", "__attribute":
			p.advance()
			args, aoff, aend := p.skipBalanced()
			p.recordAttr("__attribute__", args, t.Line, aoff, aend)
			continue
		case "asm", "__asm", "__asm__":
			p.advance()
			p.skipBalanced()
			continue

		case "void":
			ts.words = append(ts.words, "void")
			ts.base = joinWords(ts.words)
			ts.kind = BVoid
			ts.hasBase = true
			ts.saw = true
			p.advance()
			continue
		case "_Bool", "bool":
			ts.words = append(ts.words, "_Bool")
			ts.base = joinWords(ts.words)
			ts.kind = BBool
			ts.hasBase = true
			ts.saw = true
			p.advance()
			continue
		case "char":
			ts.words = append(ts.words, "char")
			ts.base = joinWords(ts.words)
			ts.kind = BChar
			ts.hasBase = true
			ts.saw = true
			p.advance()
			continue
		case "short":
			ts.words = append(ts.words, "short")
			ts.base = joinWords(ts.words)
			ts.kind = BShort
			ts.hasBase = true
			ts.saw = true
			p.advance()
			continue
		case "int":
			ts.words = append(ts.words, "int")
			ts.base = joinWords(ts.words)
			if ts.kind == BNone {
				ts.kind = BInt
			}
			ts.hasBase = true
			ts.saw = true
			p.advance()
			continue
		case "long":
			if len(ts.words) > 0 && ts.words[len(ts.words)-1] == "long" {
				ts.words[len(ts.words)-1] = "long long"
				ts.kind = BLongLong
			} else {
				ts.words = append(ts.words, "long")
				ts.kind = BLong
			}
			ts.base = joinWords(ts.words)
			ts.hasBase = true
			ts.saw = true
			p.advance()
			continue
		case "float":
			ts.words = append(ts.words, "float")
			ts.base = joinWords(ts.words)
			ts.kind = BFloat
			ts.hasBase = true
			ts.saw = true
			p.advance()
			continue
		case "double":
			ts.words = append(ts.words, "double")
			ts.base = joinWords(ts.words)
			ts.kind = BDouble
			ts.hasBase = true
			ts.saw = true
			p.advance()
			continue
		case "signed", "__signed", "__signed__":
			ts.words = append(ts.words, "signed")
			ts.base = joinWords(ts.words)
			ts.saw = true
			p.advance()
			continue
		case "unsigned":
			ts.words = append(ts.words, "unsigned")
			ts.base = joinWords(ts.words)
			ts.kind = BSigned
			ts.hasBase = true
			ts.saw = true
			p.advance()
			continue
		case "_Decimal32":
			ts.base = "_Decimal32"
			ts.kind = BDecimal32
			ts.hasBase = true
			ts.saw = true
			p.advance()
			continue
		case "_Decimal64":
			ts.base = "_Decimal64"
			ts.kind = BDecimal64
			ts.hasBase = true
			ts.saw = true
			p.advance()
			continue
		case "_Decimal128":
			ts.base = "_Decimal128"
			ts.kind = BDecimal128
			ts.hasBase = true
			ts.saw = true
			p.advance()
			continue
		case "_Complex", "_Imaginary":
			ts.words = append(ts.words, t.Text)
			ts.base = joinWords(ts.words)
			ts.saw = true
			p.advance()
			continue
		case "_BitInt":
			p.advance()
			p.expectPunct(PLparen, "after _BitInt")
			var w strings.Builder
			for !p.eof() && !p.atPunct(PRparen) {
				w.WriteString(p.cur().Text)
				p.advance()
			}
			p.expectPunct(PRparen, "after _BitInt width")
			ts.base = "_BitInt(" + w.String() + ")"
			ts.kind = BBitInt
			ts.hasBase = true
			ts.saw = true
			continue
		case "typeof", "typeof_unqual", "__typeof", "__typeof__":
			unqual := t.Text != "typeof" && t.Text != "__typeof__" && t.Text != "__typeof"
			p.advance()
			p.expectPunct(PLparen, "after typeof")
			text := "typeof("
			if p.isTypeStart() {
				tn := p.parseTypeName()
				text += tn.text
				p.expectPunct(PRparen, "to close typeof(")
			} else {

				depth := 1
				var b strings.Builder
				for !p.eof() && depth > 0 {
					c := p.cur()
					if c.Kind == TPunct {
						if c.Punct == PLparen {
							depth++
						} else if c.Punct == PRparen {
							depth--
							if depth == 0 {
								p.advance()
								break
							}
						}
					}
					if b.Len() > 0 && c.WSBefore {
						b.WriteByte(' ')
					}
					b.WriteString(c.Text)
					p.advance()
				}
				text += b.String()
			}
			if unqual {
				text = strings.Replace(text, "typeof(", "typeof_unqual(", 1)
			}
			text += ")"
			ts.base = text
			ts.kind = BTypeof
			ts.hasBase = true
			ts.saw = true
			continue
		case "struct", "union", "enum":
			p.advance()
			node := p.parseTagSpec(t.Text)
			ts.tagNode = node
			ts.kind = map[string]int{"struct": BStruct, "union": BUnion, "enum": BEnum}[t.Text]
			td := p.fa.Tags[p.fa.Nodes[node].A-1]
			_ = td
			ts.base = t.Text
			if td.NameIdx != 0 {
				ts.base += " " + p.fa.name(td.NameIdx)
			}
			if td.HasUnderlying {
				ts.base += " : " + p.fa.name(td.UnderlyingIdx)
			}
			ts.tag = t.Text
			ts.hasBase = true
			ts.saw = true
			continue
		default:
			if p.isTypedefName(t.Text) {
				ts.base = t.Text
				ts.kind = BTypedef
				ts.typedef = t.Text
				ts.hasBase = true
				ts.saw = true
				p.advance()
				continue
			}

			if !ts.hasBase && p.unknownTypeFollows() {

				ts.words = append(ts.words, t.Text)
				ts.base = joinWords(ts.words)
				ts.kind = BTypedef
				ts.typedef = t.Text
				ts.hasBase = true
				ts.saw = true
				p.advance()
				continue
			}
			if ts.saw {
				return ts
			}
			return ts
		}
	}

	p.parseAttributes()
	return ts
}

func joinWords(ws []string) string {
	return strings.Join(ws, " ")
}

func (p *parser) unknownTypeFollows() bool {
	if p.krMode {
		return false
	}
	nx := p.peek(1)
	switch nx.Kind {
	case TIdent:
		switch nx.Text {
		case "__attribute__", "__attribute", "__asm__", "__asm", "asm",
			"__extension__":
			return false
		}

		switch nx.Text {
		case "const", "volatile", "restrict", "_Atomic", "struct", "union",
			"enum", "void", "char", "short", "int", "long", "float", "double",
			"signed", "unsigned", "_Bool", "bool", "_Complex", "_Imaginary",
			"typeof", "typeof_unqual", "_BitInt", "_Decimal32", "_Decimal64",
			"_Decimal128", "constexpr", "alignas", "_Alignas":
			return true
		}
		return !isKeyword(nx.Text)
	case TPunct:
		switch nx.Punct {
		case PStar, PLbrack, PSemi, PEq, PLparen:
			return true
		}
	}
	return false
}

func (p *parser) parseTagSpec(kind string) uint32 {
	kwTok := p.cur()
	p.parseAttributes()
	node := uint32(0)
	td := TagD{Tag: kind}
	if p.cur().Kind == TIdent {
		nm := p.advance()
		td.NameIdx = p.fa.intern(nm.Text)
		td.Line = int32(nm.Line)
	}

	if kind == "enum" && p.atPunct(PColon) {
		p.advance()
		tn := p.parseTypeName()
		td.UnderlyingIdx = p.fa.intern(tn.text)
		td.HasUnderlying = true
	}
	if p.atPunct(PLbrace) {
		ob := p.advance()
		td.Defined = true
		if td.Line == 0 {
			td.Line = int32(ob.Line)
		}

		child0 := uint32(len(p.fa.Nodes))
		if kind == "enum" {
			for !p.eof() && !p.atPunct(PRbrace) {
				p.parseAttributes()
				if p.atPunct(PRbrace) {
					break
				}
				if p.cur().Kind != TIdent {
					p.errf(p.cur(), "expected enumerator name")
					p.syncTo(p.cur(), "enumerator")
					if p.atPunct(PComma) {
						p.advance()
					}
					continue
				}
				nm := p.advance()
				ed := EnumD{NameIdx: p.fa.intern(nm.Text), Line: nm.Line}
				if p.acceptPunct(PEq) {
					vs, ve := p.exprSpan()
					if ve > vs {
						ed.HasValue = true
						ed.ValueIdx = p.fa.intern(rawText(p.srcOf(nm), vs, ve))
					}
				}
				en := Node{Kind: NEnumerator, File: uint32(nm.File), Line: nm.Line,
					Col: nm.Col, Off: nm.Off, End: nm.End}
				if ed.HasValue {
					en.Flags = 1
				}
				p.fa.Enums = append(p.fa.Enums, ed)
				en.A = uint32(len(p.fa.Enums))
				en.B = ed.ValueIdx
				p.addNode(en)
				p.addEnumConst(p.fa.name(ed.NameIdx))
				if !p.acceptPunct(PComma) {
					break
				}
			}
			p.expectPunct(PRbrace, "to close enum")
			p.parseAttributes()
			td.Child0 = child0
			td.NChild = uint32(len(p.fa.Nodes)) - child0
			endTok := p.cur()
			td.LineEnd = endTok.Line
		} else {
			for !p.eof() && !p.atPunct(PRbrace) {
				p.parseMember(kind)
			}
			p.expectPunct(PRbrace, "to close "+kind)
			p.parseAttributes()
			td.Child0 = child0
			td.NChild = uint32(len(p.fa.Nodes)) - child0
			endTok := p.cur()
			td.LineEnd = endTok.Line
		}
	} else {
		td.LineEnd = td.Line
	}
	p.fa.Tags = append(p.fa.Tags, td)
	idx := uint32(len(p.fa.Tags))
	k := uint16(NStructSpec)
	switch kind {
	case "union":
		k = NUnionSpec
	case "enum":
		k = NEnumSpec
	}
	n := Node{Kind: k, A: idx, Off: 0, End: 0}
	if td.Line != 0 {
		n.Line = td.Line
	}
	n.File = uint32(kwTok.File)
	node = p.addNode(n)
	if td.NameIdx != 0 {
		p.addTag(p.fa.name(td.NameIdx))
	}
	return node
}

func (p *parser) parseMember(kind string) {
	t := p.cur()
	if p.atPunct(PSemi) {
		p.advance()
		return
	}
	if p.atIdent("_Static_assert") || p.atIdent("static_assert") {
		p.parseStaticAssert()
		return
	}
	specs := p.parseDeclSpecs()
	if !specs.saw {
		p.errf(t, "bad member declaration")
		p.syncTo(t, "member")
		return
	}

	if p.atPunct(PSemi) && (specs.kind == BStruct || specs.kind == BUnion) {
		p.advance()
		fd := FieldD{TypeIdx: p.fa.intern(specs.baseText()), PtrTypeIdx: p.fa.intern(specs.baseText()),
			Depth: int32(p.scopeDepth()), Line: t.Line}
		p.fa.Fields = append(p.fa.Fields, fd)
		n := Node{Kind: NField, A: uint32(len(p.fa.Fields)), File: uint32(t.File),
			Off: t.Off, End: t.End, Line: t.Line, Col: t.Col}
		p.addNode(n)
		return
	}
	for {
		p.parseAttributes()
		d := p.declarator(&specs, false)
		p.parseAttributes()
		bf := int32(0)
		if p.acceptPunct(PColon) {
			_, ve := p.condExprSpan()
			if ve > 0 {
				bf = -1
			}
		}

		fd := FieldD{
			TypeIdx:    p.fa.intern(specs.baseText()),
			Ptr:        int32(d.ptr),
			PtrTypeIdx: p.fa.intern(specs.baseText() + d.ptrStars()),
			ArrayLen:   arrayLenI(d),
			Bitfield:   bf,
			IsFnptr:    d.fnptr,
			Depth:      int32(d.depth),
			IsConst:    specs.constq,
			Line:       int32(t.Line),
		}
		if d.name != "" {
			fd.NameIdx = p.fa.intern(d.name)
		}
		p.fa.Fields = append(p.fa.Fields, fd)
		n := Node{Kind: NField, A: uint32(len(p.fa.Fields)), File: uint32(t.File),
			Off: t.Off, End: t.End, Line: t.Line, Col: t.Col}
		p.addNode(n)
		if p.acceptPunct(PComma) {
			continue
		}
		p.expectPunct(PSemi, "after member")
		break
	}
}

func (p *parser) scopeDepth() int { return len(p.scopes) }

func arrayLenI(d dcl) int32 {
	if !d.isArray {
		return 0
	}
	if d.arrayConst {
		return int32(d.arrayLen)
	}
	return -1
}

func rawText(src []byte, off, end int32) string {
	if int(off) <= len(src) && int(end) <= len(src) && off <= end {
		return squeeze(string(src[off:end]), "")
	}
	return ""
}

func (p *parser) parseStaticAssert() {
	t := p.advance()
	p.expectPunct(PLparen, "after _Static_assert")
	n := Node{Kind: NStaticAssert, Off: t.Off, Line: t.Line, Col: t.Col}
	child0 := uint32(len(p.fa.Nodes))
	if !p.atPunct(PRparen) {
		p.assignment()
	}
	if p.acceptPunct(PComma) {
		for p.cur().Kind == TStr {
			p.advance()
		}
	}
	p.expectPunct(PRparen, "to close _Static_assert")
	p.expectPunct(PSemi, "after _Static_assert")
	n.A = child0
	n.B = uint32(len(p.fa.Nodes)) - child0
	n.End = p.cur().End
	p.addNode(n)
}

func (p *parser) exprSpan() (int32, int32) {
	p.assignment()
	return p.lastSpan()
}

func (p *parser) condExprSpan() (int32, int32) {
	p.assignment()
	return p.lastSpan()
}

func (p *parser) lastSpan() (int32, int32) {
	if len(p.fa.Nodes) == 0 {
		return 0, 0
	}
	n := p.fa.Nodes[len(p.fa.Nodes)-1]
	return n.Off, n.End
}

type paramT struct {
	name     string
	typ      string
	ptr      int
	arrayLen int32
	isConst  bool
	variadic bool
	unnamed  bool
	line     int32
}

type dcl struct {
	name       string
	nameTok    Tok
	hasName    bool
	ptr        int
	isArray    bool
	arrayLen   int64
	arrayConst bool
	isFn       bool
	fnptr      bool
	parenName  bool
	params     []paramT
	variadic   bool
	kr         bool
	krNames    []string
	depth      int
	constq     bool
	volatileq  bool
}

func (d *dcl) ptrStars() string {
	s := ""
	for i := 0; i < d.ptr; i++ {
		s += " *"
	}
	return s
}

func (p *parser) skipDecorationIdents(specs *tspecs) {
	if specs == nil || !specs.saw || p.krMode {
		return
	}

	baseIsDecoration := false
	if specs.kind == BTypedef && specs.hasBase && !isKeyword(specs.base) &&
		!p.isTypedefName(specs.base) && c23IsIdentStart(specs.base[0]) &&
		!strings.ContainsAny(specs.base, " *([;,") {
		baseIsDecoration = true
	}
	for {
		t := p.cur()
		if t.Kind != TIdent || isKeyword(t.Text) || p.isTypedefName(t.Text) {
			return
		}
		n1 := p.peek(1)
		if n1.Kind != TIdent || isKeyword(n1.Text) {
			return
		}
		n2 := p.peek(2)
		if n2.Kind != TPunct {
			return
		}
		switch n2.Punct {
		case PLparen:
		case PComma, PRparen:

			if !baseIsDecoration {
				return
			}
		default:
			return
		}
		p.advance()
	}
}

func (p *parser) declarator(specs *tspecs, abstract bool) dcl {
	d := dcl{}
	for {
		p.parseAttributes()
		if !p.acceptPunct(PStar) {
			break
		}
		d.ptr++
		for {
			t := p.cur()
			if t.Kind != TIdent {
				break
			}
			switch t.Text {
			case "const", "__const", "__const__":
				d.constq = true
			case "volatile", "__volatile", "__volatile__":
				d.volatileq = true
			case "restrict", "__restrict", "__restrict__":
			case "_Atomic":
			default:
				goto ptrDone
			}
			p.advance()
		}
	ptrDone:
	}
	p.skipDecorationIdents(specs)
	d = p.directDeclarator(d, abstract)
	p.parseAttributes()

	p.skipDeclaratorAsm()
	return d
}

func (p *parser) skipDeclaratorAsm() {
	for p.atIdent("__asm__") || p.atIdent("__asm") || p.atIdent("asm") {
		p.advance()
		p.skipBalanced()
	}
}

func (p *parser) directDeclarator(d dcl, abstract bool) dcl {
	t := p.cur()
	switch {
	case t.Kind == TIdent && !p.atPunct(0):

		if !isKeyword(t.Text) {
			d.name = t.Text
			d.nameTok = t
			d.hasName = true
			p.advance()
		}
	case t.Kind == TPunct && t.Punct == PLparen:

		nx := p.peek(1)
		if abstract && parenIsParamList(nx) {

			d = p.declaratorSuffix(d, abstract)
			return d
		}
		p.advance()
		d.depth++
		inner := p.declarator(nil, abstract)
		p.expectPunct(PRparen, "to close declarator")
		if inner.hasName {
			d.name = inner.name
			d.nameTok = inner.nameTok
			d.hasName = true
			d.parenName = true
		}
		d.ptr += inner.ptr
		d.depth += inner.depth
		d.constq = d.constq || inner.constq
	}
	d = p.declaratorSuffix(d, abstract)
	return d
}

func parenIsParamList(nx Tok) bool {
	if nx.Kind == TPunct && (nx.Punct == PRparen || nx.Punct == PEllip) {
		return true
	}
	if nx.Kind != TIdent {
		return false
	}
	switch nx.Text {
	case "const", "volatile", "restrict", "_Atomic", "void", "char", "short",
		"int", "long", "float", "double", "signed", "unsigned", "_Bool", "bool",
		"struct", "union", "enum", "typeof", "typeof_unqual", "_BitInt",
		"_Decimal32", "_Decimal64", "_Decimal128", "register", "inline":
		return true
	}
	return false
}

func (p *parser) declaratorSuffix(d dcl, abstract bool) dcl {
	for !p.eof() {
		t := p.cur()
		// `int x [[maybe_unused]];` -- a C23 attribute after the declarator,
		// not the start of an array bound
		if t.Kind == TPunct && t.Punct == PLbrack {
			if nx := p.peek(1); nx.Kind == TPunct && nx.Punct == PLbrack {
				p.parseAttributes()
				continue
			}
		}
		if t.Kind == TPunct && t.Punct == PLbrack {
			p.advance()
			d.isArray = true
			d.arrayConst = false
			d.arrayLen = 0

			if t2 := p.cur(); t2.Kind == TIdent && t2.Text == "static" {
				d.arrayConst = true
				p.advance()
			}
			for p.cur().Kind == TIdent {
				switch p.cur().Text {
				case "const", "volatile", "restrict", "__restrict", "__restrict__",
					"_Atomic", "static":
					d.arrayConst = d.arrayConst || p.cur().Text == "static"
					p.advance()
					continue
				}
				break
			}
			if p.atPunct(PStar) && p.peek(1).Kind == TPunct && p.peek(1).Punct == PRbrack {
				p.advance()
				d.arrayConst = false
				d.arrayLen = -1
			} else if !p.atPunct(PRbrack) {
				start := p.cur()
				p.assignment()
				end := p.prevEnd()
				if v, ok := constNumText(p.srcOf(start), start.Off, end); ok {
					d.arrayLen = v
					d.arrayConst = true
				} else {
					d.arrayLen = -1
				}
			}
			p.expectPunct(PRbrack, "to close array")
			continue
		}
		if t.Kind == TPunct && t.Punct == PLparen {
			p.advance()
			params, variadic, kr := p.paramList()
			d.isFn = true
			d.params = params
			d.variadic = variadic
			d.kr = kr
			if kr {
				for _, pt := range params {
					d.krNames = append(d.krNames, pt.name)
				}
			}
			d.fnptr = d.parenName && d.ptr > 0
			continue
		}
		return d
	}
	return d
}

func (p *parser) prevEnd() int32 {
	if p.pos > 0 {
		return p.toks[p.pos-1].End
	}
	return p.cur().End
}

func constNumText(src []byte, off, end int32) (int64, bool) {
	s := strings.TrimSpace(rawText(src, off, end))
	if s == "" {
		return 0, false
	}
	v, err := strconv.ParseInt(s, 0, 64)
	if err != nil {

		return 0, false
	}
	return v, true
}

func (p *parser) paramList() ([]paramT, bool, bool) {
	var params []paramT
	variadic := false
	if p.atPunct(PRparen) {
		p.advance()
		return nil, false, false
	}
	if p.atIdent("void") && p.peek(1).Kind == TPunct && p.peek(1).Punct == PRparen {
		p.advance()
		p.advance()
		return nil, false, false
	}
	kr := false
	for !p.eof() {
		p.parseAttributes()
		if p.acceptPunct(PEllip) {
			variadic = true
			p.closeParamList()
			return params, true, kr
		}
		t := p.cur()
		specs := p.parseDeclSpecs()
		if !specs.saw {

			if t.Kind == TIdent && !isKeyword(t.Text) {
				p.advance()
				kr = true
				params = append(params, paramT{name: t.Text, line: int32(t.Line)})
				if p.acceptPunct(PComma) {
					continue
				}
				p.closeParamList()
				return params, false, kr
			}
			p.errf(t, "expected parameter declaration")
			if p.atPunct(PRparen) {
				p.advance()
				return params, variadic, kr
			}
			if p.atPunct(PComma) {
				p.advance()
				continue
			}

			p.closeParamList()
			return params, variadic, kr
		}
		isVariadicArg := false
		if p.atPunct(PEllip) {
			isVariadicArg = true
		}
		d := p.declarator(&specs, true)
		if isVariadicArg {
			variadic = true
		}
		pt := paramT{
			typ:      specs.baseText() + d.ptrStars(),
			ptr:      d.ptr,
			arrayLen: arrayLenI(d),
			isConst:  specs.constq,
			variadic: variadic && d.name == "" && !d.hasName,
			unnamed:  !d.hasName,
			line:     int32(t.Line),
		}
		if pt.arrayLen != 0 {

		}
		if d.hasName {
			pt.name = d.name
		}
		if specs.typedef == "typedef" {

			specs.typedef = ""
		}
		params = append(params, pt)

		for p.cur().Kind == TIdent && !isKeyword(p.cur().Text) {
			nx := p.peek(1)
			if nx.Kind == TIdent && !isKeyword(nx.Text) {
				p.advance()
				continue
			}
			if nx.Kind == TPunct && nx.Punct == PLparen {
				p.advance()
				p.skipBalanced()
				continue
			}
			if nx.Kind == TPunct && (nx.Punct == PComma || nx.Punct == PRparen) {
				p.advance()
			}
			break
		}
		if p.acceptPunct(PComma) {
			continue
		}
		p.closeParamList()
		return params, variadic, kr
	}
	return params, variadic, kr
}

type tname struct {
	text    string
	ptr     int
	isFn    bool
	isArray bool
}

func (p *parser) parseTypeName() tname {
	specs := p.parseDeclSpecs()
	d := p.declarator(&specs, true)
	return tname{
		text:    specs.baseText() + d.ptrStars(),
		ptr:     d.ptr,
		isFn:    d.isFn,
		isArray: d.isArray,
	}
}

func (p *parser) parseTU() {

	mk := p.addNode(Node{Kind: NTU})
	p.fa.Nodes[mk].A = uint32(len(p.fa.Nodes))
	lastPos := -1
	reps := 0
	for !p.eof() {
		if p.pos == lastPos {
			reps++
			if reps > 4 {

				p.errf(p.cur(), "no progress; skipping token")
				p.advance()
				reps = 0
				continue
			}
		} else {
			reps = 0
			lastPos = p.pos
		}
		p.parseExternal()
		if len(p.fa.Nodes) > 4<<20 {
			p.errf(p.cur(), "node budget exceeded")
			return
		}
	}
	p.fa.Nodes[mk].B = uint32(len(p.fa.Nodes)) - p.fa.Nodes[mk].A
	if len(p.fa.Nodes) > 0 {
		p.fa.Nodes[mk].End = p.toks[len(p.toks)-1].End
	}
}

func (p *parser) parseExternal() {
	t := p.cur()
	switch {
	case p.atPunct(PSemi):
		p.advance()
		return
	case p.atPunct(PRbrace):
		p.errf(t, "unexpected '}' at file scope")
		p.advance()
		return
	case p.atIdent("__extension__"):
		p.advance()
		return
	case p.atIdent("asm") || p.atIdent("__asm__") || p.atIdent("__asm"):
		p.skipAsm()
		p.acceptPunct(PSemi)
		return
	case p.atIdent("_Static_assert") || p.atIdent("static_assert"):
		p.parseStaticAssert()
		return
	case p.atIdent("extern") && p.peek(1).Kind == TStr:

		p.advance()
		p.advance()
		if p.acceptPunct(PLbrace) {
			for !p.eof() && !p.atPunct(PRbrace) {
				p.parseExternal()
			}
			p.acceptPunct(PRbrace)
			return
		}
		p.parseExternal()
		return
	}
	// `LUAI_FUNC Proto *luaY_parser (...)` with neither name known: the first
	// is an attribute macro that expanded to nothing in the real build, the
	// second is the return type.
	if p.plainIdent(t) && p.plainIdent(p.peek(1)) {
		if j, stars := p.skipPtrQuals(2); stars > 0 {
			nm, after := p.peek(j), p.peek(j+1)
			if nm.Kind == TIdent && !cKeywords[nm.Text] && after.Kind == TPunct &&
				after.Punct == PLparen {
				p.scopes[0].setTypedef(p.peek(1).Text, true)
				p.advance()
				t = p.cur()
			}
		}
	}
	p.parseAttributes()
	specs := p.parseDeclSpecs()
	p.parseAttributes()
	if !specs.saw {
		p.errf(t, "expected declaration")
		p.syncTo(t, "external declaration")
		return
	}
	if specs.isTypedef {
		d0 := p.declarator(&specs, false)
		if d0.isFn && (p.atPunct(PLbrace) || d0.kr) {
			p.parseFuncDef(specs, d0, t)
			return
		}
		p.parseDeclList(specs, d0, t, true)
		return
	}
	d := p.declarator(&specs, false)

	if d.isFn && !p.atPunct(PLbrace) && !p.atPunct(PSemi) && krShaped(d) {
		save := p.pos
		saveErrs := p.nErrors
		if p.errs != nil {
			saveErrs = len(*p.errs)
		}
		p.krMode = true
		d2 := p.declarator(&specs, false)
		p.krMode = false
		if d2.isFn && d2.kr {
			d = d2
		} else {
			p.pos = save
			p.nErrors = saveErrs
			if p.errs != nil {
				*p.errs = (*p.errs)[:min(saveErrs, len(*p.errs))]
			}
		}
	}
	if d.isFn && (p.atPunct(PLbrace) || d.kr) {
		p.parseFuncDef(specs, d, t)
		return
	}
	p.parseDeclList(specs, d, t, true)
}

func krShaped(d dcl) bool {
	if len(d.params) == 0 || d.variadic {
		return false
	}
	for _, pt := range d.params {
		if !pt.unnamed || pt.ptr > 0 || strings.Contains(pt.typ, " ") ||
			strings.Contains(pt.typ, "*") || strings.Contains(pt.typ, "(") {
			return false
		}
	}
	return true
}

func (p *parser) parseDeclList(specs tspecs, first dcl, t Tok, topLevel bool) []uint32 {
	var created []uint32
	d := first
	have := true
	for {
		if !have {
			p.parseAttributes()
			d = p.declarator(&specs, false)
			p.parseAttributes()
		}
		have = false
		nd := DeclNode{
			NameIdx:  0,
			TypeIdx:  p.fa.intern(specs.baseText() + d.ptrStars()),
			Ptr:      int32(d.ptr),
			ArrayLen: arrayLenI(d),
			Storage:  specs.storage,
			Line:     int32(t.Line),
		}
		if d.hasName {
			nd.NameIdx = p.fa.intern(d.name)
		}
		kind := uint8(0)
		switch {
		case specs.isTypedef:
			kind = 2
		case d.isFn && !d.fnptr:
			kind = 1
		}
		nd.Kind = kind
		if kind == 2 && d.hasName {
			p.addTypedef(d.name)
		} else if kind == 0 && d.hasName && p.funcDepth > 0 {
			p.addTypedefGuard(d.name)
		}

		if p.acceptPunct(PEq) {
			nd.Init = p.initializer()
			nd.Storage |= FHasInit
		}
		p.fa.Decls = append(p.fa.Decls, nd)
		idxA := uint32(len(p.fa.Decls))
		n := Node{Kind: NDecl, A: idxA, Off: t.Off, End: t.End,
			Line: t.Line, Col: t.Col}
		if d.hasName {
			n.B = nd.NameIdx
		}
		if nd.Init != 0 {
			n.Flags |= FHasInit
		}
		n.File = uint32(t.File)
		created = append(created, p.addNode(n))
		if p.acceptPunct(PComma) {
			t = p.cur()
			continue
		}
		p.expectPunct(PSemi, "after declaration")
		break
	}
	return created
}

func (p *parser) parseFuncDef(specs tspecs, d dcl, t Tok) {

	if d.kr {
		for !p.eof() && !p.atPunct(PLbrace) {
			kt := p.cur()
			kspecs := p.parseDeclSpecs()
			if !kspecs.saw {
				p.errf(kt, "bad K&R declaration")
				p.advance()
				continue
			}
			have := false
			for {
				p.parseAttributes()
				kd := p.declarator(&kspecs, false)
				for i := range d.params {
					if d.params[i].name == kd.name {
						d.params[i].typ = kspecs.baseText() + kd.ptrStars()
						d.params[i].ptr = kd.ptr
						d.params[i].isConst = kspecs.constq
						d.params[i].unnamed = false
					}
				}
				have = true
				if !p.acceptPunct(PComma) {
					break
				}
			}
			_ = have
			p.expectPunct(PSemi, "after K&R declaration")
		}
	}
	ob := p.cur()
	p.funcDepth++
	p.pushScope()

	p0 := uint32(len(p.fa.Nodes))
	sig := funcSignature(specs, d)
	ret := specs.baseText() + d.ptrStars()
	for i := range d.params {
		pt := &d.params[i]
		pd := ParamD{
			TypeIdx:  p.fa.intern(pt.typ),
			Ptr:      int32(pt.ptr),
			ArrayLen: pt.arrayLen,
			Variadic: pt.variadic && i == len(d.params)-1 && d.variadic,
			Unnamed:  pt.unnamed,
			IsConst:  pt.isConst,
			Line:     pt.line,
		}
		if pt.name != "" {
			pd.NameIdx = p.fa.intern(pt.name)
		}
		p.fa.Params = append(p.fa.Params, pd)
		pn := Node{Kind: NParam, A: uint32(len(p.fa.Params)), Line: pt.line}
		p.addNode(pn)
		if pt.name != "" {
			p.addTypedefGuard(pt.name)
		}
	}
	fd := FuncDef{
		NameIdx:  p.fa.intern(d.name),
		SigIdx:   p.fa.intern(sig),
		RetIdx:   p.fa.intern(ret),
		Param0:   p0,
		NParams:  uint32(len(d.params)),
		LineEnd:  int32(ob.Line),
		Storage:  specs.storage,
		Variadic: d.variadic,
		KR:       d.kr,
	}
	body := p.block()
	p.popScope()
	p.funcDepth--
	fd.Body = body
	if body != 0 {
		fd.LineEnd = int32(p.fa.Nodes[body].Line)
	}
	p.fa.Funcs = append(p.fa.Funcs, fd)
	fn := Node{Kind: NFuncDef, A: uint32(len(p.fa.Funcs)), File: uint32(t.File),
		Off: t.Off, Line: t.Line, Col: t.Col}
	if body != 0 {
		fn.End = p.fa.Nodes[body].End
	} else {
		fn.End = t.End
	}
	p.addNode(fn)
}

// addTypedefGuard records that an ordinary identifier now hides a type name.
// `void rotate(list *list) { if (listLength(list) ...` expands to
// `(list)->len`, which is a cast of nothing unless `list` is known to be the
// parameter. C makes the same choice: the type is unusable in that scope.
func (p *parser) addTypedefGuard(name string) {
	if name != "" && len(p.scopes) > 1 && p.isTypedefName(name) {
		p.scopes[len(p.scopes)-1].setTypedef(name, false)
	}
}

func funcSignature(specs tspecs, d dcl) string {
	var b strings.Builder
	if specs.storage&FStatic != 0 {
		b.WriteString("static ")
	}
	if specs.storage&FInline != 0 {
		b.WriteString("inline ")
	}
	if specs.storage&FExtern != 0 {
		b.WriteString("extern ")
	}
	b.WriteString(specs.baseText())
	b.WriteString(d.ptrStars())
	b.WriteString(" ")
	b.WriteString(d.name)
	b.WriteString("(")
	for i, pt := range d.params {
		if i > 0 {
			b.WriteString(", ")
		}
		if pt.typ == "" {
			b.WriteString(pt.name)
		} else if pt.unnamed {
			b.WriteString(pt.typ)
		} else {
			b.WriteString(pt.typ)
			b.WriteString(" ")
			b.WriteString(pt.name)
		}
	}
	if d.variadic {
		if len(d.params) > 0 {
			b.WriteString(", ")
		}
		b.WriteString("...")
	}
	b.WriteString(")")
	return b.String()
}

func (p *parser) block() uint32 {
	ob := p.advance()
	p.pushScope()
	mk := p.addNode(Node{Kind: NCompoundStmt, Off: ob.Off, End: ob.End,
		Line: ob.Line, Col: ob.Col})
	p.fa.Nodes[mk].A = uint32(len(p.fa.Nodes))
	for !p.atPunct(PRbrace) && !p.eof() {
		p.stmt()
	}
	p.fa.Nodes[mk].B = uint32(len(p.fa.Nodes)) - p.fa.Nodes[mk].A
	if p.acceptPunct(PRbrace) {
		p.fa.Nodes[mk].End = p.toks[p.pos-1].End
	}
	p.popScope()
	return mk
}

func (p *parser) isDeclStart() bool {
	t := p.cur()
	if t.Kind != TIdent {
		return false
	}
	switch t.Text {
	case "static", "extern", "register", "auto", "typedef", "const", "volatile",
		"restrict", "_Atomic", "struct", "union", "enum", "void", "char", "short",
		"int", "long", "float", "double", "signed", "unsigned", "_Bool", "bool",
		"_Complex", "_Imaginary", "typeof", "typeof_unqual", "_BitInt",
		"_Decimal32", "_Decimal64", "_Decimal128", "constexpr", "thread_local",
		"_Thread_local", "alignas", "_Alignas", "__extension__",
		"__attribute__", "__attribute", "__inline", "__inline__", "_Noreturn",
		"noreturn", "static_assert", "_Static_assert":
		return true
	}
	return p.isTypedefName(t.Text)
}

func (p *parser) stmt() uint32 {
	t := p.cur()
	if t.Kind == TPunct && t.Punct == mustPunct("[") {
		p.parseAttributes()
		t = p.cur()
	}
	switch {
	case p.atPunct(PLbrace):
		return p.block()
	case p.atPunct(PSemi):
		p.advance()
		return p.addNode(Node{Kind: NExprStmt, Off: t.Off, End: t.End,
			Line: t.Line, Col: t.Col})
	case p.atIdent("if"):
		return p.ifStmt()
	case p.atIdent("switch"):
		return p.switchStmt()
	case p.atIdent("while"):
		return p.whileStmt()
	case p.atIdent("do"):
		return p.doStmt()
	case p.atIdent("for"):
		return p.forStmt()
	case p.atIdent("return"):
		return p.returnStmt()
	case p.atIdent("break"):
		p.advance()
		p.expectPunct(PSemi, "after break")
		return p.addNode(Node{Kind: NBreak, Off: t.Off, End: t.End,
			Line: t.Line, Col: t.Col})
	case p.atIdent("continue"):
		p.advance()
		p.expectPunct(PSemi, "after continue")
		return p.addNode(Node{Kind: NContinue, Off: t.Off, End: t.End,
			Line: t.Line, Col: t.Col})
	case p.atIdent("goto"):
		p.advance()
		n := Node{Kind: NGoto, Off: t.Off, Line: t.Line, Col: t.Col}
		if p.acceptPunct(mustPunct("&&")) {
			if p.cur().Kind == TIdent {
				n.A = p.fa.intern(p.advance().Text)
			}
		} else if p.cur().Kind == TIdent {
			n.A = p.fa.intern(p.advance().Text)
		}
		p.expectPunct(PSemi, "after goto")
		n.End = p.prevEnd()
		return p.addNode(n)
	case p.atIdent("case"), p.atIdent("default"):
		return p.caseStmt()
	case p.atIdent("_Static_assert") || p.atIdent("static_assert"):
		p.parseStaticAssert()
		return uint32(len(p.fa.Nodes)) - 1
	case p.atIdent("asm") || p.atIdent("__asm__") || p.atIdent("__asm"):
		at := p.advance()
		p.skipAsm()
		p.acceptPunct(PSemi)
		return p.addNode(Node{Kind: NAsm, Off: at.Off, End: p.prevEnd(),
			Line: at.Line, Col: at.Col})
	case p.atIdent("__extension__"):
		p.advance()
		return p.stmt()
	case t.Kind == TIdent && p.peek(1).Kind == TPunct && p.peek(1).Punct == PColon &&
		!p.atIdent("default") && !p.atIdent("case"):

		p.advance()
		p.advance()
		mk := p.addNode(Node{Kind: NLabel, A: p.fa.intern(t.Text), Off: t.Off,
			Line: t.Line, Col: t.Col})
		body := p.stmt()
		p.fa.Nodes[mk].B = body
		p.fa.Nodes[mk].End = p.prevEnd()
		return mk
	case p.isDeclStart() || p.inferTypedef():
		if mk, ok := p.tryDeclStatement(); ok {
			return mk
		}
		fallthrough
	default:
		e0 := uint32(len(p.fa.Nodes))
		_ = e0
		mk := p.addNode(Node{Kind: NExprStmt})
		e := p.expr()
		p.fa.Nodes[mk].A = e
		p.expectPunct(PSemi, "after expression statement")
		p.fa.Nodes[mk].Off = t.Off
		p.fa.Nodes[mk].End = p.prevEnd()
		p.fa.Nodes[mk].Line = t.Line
		p.fa.Nodes[mk].Col = t.Col
		return mk
	}
}

func (p *parser) tryDeclStatement() (uint32, bool) {
	savePos := p.pos
	saveNodes := len(p.fa.Nodes)
	saveDecls := len(p.fa.Decls)
	saveParams := len(p.fa.Params)
	saveTags := len(p.fa.Tags)
	saveFields := len(p.fa.Fields)
	saveEnums := len(p.fa.Enums)
	saveTypes := len(p.fa.Types)
	saveInits := len(p.fa.Inits)
	saveErrs := p.nErrors
	t := p.cur()
	mk := p.addNode(Node{Kind: NDeclStmt, Off: t.Off, Line: t.Line,
		Col: t.Col})
	p.fa.Nodes[mk].A = uint32(len(p.fa.Nodes))
	p.parseAttributes()
	specs := p.parseDeclSpecs()
	if !specs.saw {
		p.rollbackDecl(savePos, saveNodes, saveDecls, saveParams, saveTags,
			saveFields, saveEnums, saveTypes, saveInits, saveErrs, mk)
		return 0, false
	}
	d0 := p.declarator(&specs, false)
	p.parseDeclList(specs, d0, t, false)
	if p.nErrors > saveErrs {
		p.rollbackDecl(savePos, saveNodes, saveDecls, saveParams, saveTags,
			saveFields, saveEnums, saveTypes, saveInits, saveErrs, mk)
		return 0, false
	}
	p.fa.Nodes[mk].B = uint32(len(p.fa.Nodes)) - p.fa.Nodes[mk].A
	p.fa.Nodes[mk].End = p.prevEnd()
	return mk, true
}

func (p *parser) rollbackDecl(savePos, nN, nD, nP, nT, nF, nE, nTy, nI, nEr int, mk uint32) {
	p.pos = savePos
	p.fa.Nodes = p.fa.Nodes[:nN]
	p.fa.Decls = p.fa.Decls[:nD]
	p.fa.Params = p.fa.Params[:nP]
	p.fa.Tags = p.fa.Tags[:nT]
	p.fa.Fields = p.fa.Fields[:nF]
	p.fa.Enums = p.fa.Enums[:nE]
	p.fa.Types = p.fa.Types[:nTy]
	p.fa.Inits = p.fa.Inits[:nI]
	if p.errs != nil {
		*p.errs = (*p.errs)[:min(nEr, len(*p.errs))]
	}
	_ = mk
	p.nErrors = nEr
}

func (p *parser) ifStmt() uint32 {
	t := p.advance()
	p.expectPunct(PLparen, "after if")
	c := p.expr()
	p.expectPunct(PRparen, "after if condition")
	th := p.stmt()
	el := uint32(0)
	if p.acceptIdent("else") {
		el = p.stmt()
	}
	return p.addNode(Node{Kind: NIf, A: c, B: th, C: el, Off: t.Off,
		End: p.prevEnd(), Line: t.Line, Col: t.Col})
}

func (p *parser) switchStmt() uint32 {
	t := p.advance()
	p.expectPunct(PLparen, "after switch")
	c := p.expr()
	p.expectPunct(PRparen, "after switch condition")
	b := p.stmt()
	return p.addNode(Node{Kind: NSwitch, A: c, B: b, Off: t.Off, End: p.prevEnd(),
		Line: t.Line, Col: t.Col})
}

func (p *parser) whileStmt() uint32 {
	t := p.advance()
	// The loop node is created BEFORE its body, as forStmt does. A block is
	// walked as the flat run of every node created inside it, so a loop node
	// that came after its body let the body be visited first, outside any
	// loop: no loop depth and no *_in_loop count for any while loop.
	mk := p.addNode(Node{Kind: NWhile, Off: t.Off, Line: t.Line, Col: t.Col})
	p.expectPunct(PLparen, "after while")
	c := p.expr()
	p.expectPunct(PRparen, "after while condition")
	p.loopDepth++
	b := p.stmt()
	p.loopDepth--
	p.fa.Nodes[mk].A = c
	p.fa.Nodes[mk].B = b
	p.fa.Nodes[mk].End = p.prevEnd()
	return mk
}

func (p *parser) doStmt() uint32 {
	t := p.advance()
	mk := p.addNode(Node{Kind: NDo, Off: t.Off, Line: t.Line, Col: t.Col})
	p.loopDepth++
	b := p.stmt()
	p.loopDepth--
	p.fa.Nodes[mk].A = b
	if p.acceptIdent("while") {
		p.expectPunct(PLparen, "after do while")
		c := p.expr()
		p.expectPunct(PRparen, "after do while")
		p.expectPunct(PSemi, "after do-while")
		p.fa.Nodes[mk].B = c
	}
	p.fa.Nodes[mk].End = p.prevEnd()
	return mk
}

func (p *parser) forStmt() uint32 {
	t := p.advance()
	p.expectPunct(PLparen, "after for")
	p.pushScope()
	mk := p.addNode(Node{Kind: NFor, Off: t.Off, Line: t.Line, Col: t.Col})
	init := uint32(0)
	if !p.atPunct(PSemi) {
		if p.isDeclStart() || p.inferTypedef() {
			if n, ok := p.tryDeclStatement(); ok {
				init = n
			}
		} else {
			ie := p.addNode(Node{Kind: NExprStmt})
			e := p.expr()
			p.fa.Nodes[ie].A = e
			p.expectPunct(PSemi, "after for-init")
			p.fa.Nodes[ie].End = p.prevEnd()
			init = ie
		}
	} else {
		p.advance()
	}
	cond := uint32(0)
	if !p.atPunct(PSemi) {
		cond = p.expr()
	}
	p.expectPunct(PSemi, "after for-cond")
	step := uint32(0)
	if !p.atPunct(PRparen) {
		step = p.expr()
	}
	p.expectPunct(PRparen, "after for-clauses")
	p.loopDepth++
	b := p.stmt()
	p.loopDepth--
	p.fa.Nodes[mk].A = init
	p.fa.Nodes[mk].B = cond
	p.fa.Nodes[mk].C = step
	p.fa.Nodes[mk].D = b
	p.fa.Nodes[mk].End = p.prevEnd()
	p.popScope()
	return mk
}

func (p *parser) returnStmt() uint32 {
	t := p.advance()
	n := Node{Kind: NReturn, Off: t.Off, Line: t.Line, Col: t.Col}
	if !p.atPunct(PSemi) {
		n.A = p.expr()
	}
	p.expectPunct(PSemi, "after return")
	n.End = p.prevEnd()
	return p.addNode(n)
}

func (p *parser) caseStmt() uint32 {
	t := p.advance()
	n := Node{Kind: NCase, Off: t.Off, Line: t.Line, Col: t.Col}
	if t.Text == "default" {
		p.expectPunct(PColon, "after default")
	} else {
		n.A = p.condExprNode()
		if p.acceptPunct(mustPunct("...")) {
			p.condExprNode()
		}
		p.expectPunct(PColon, "after case")
	}
	n.B = p.stmt()
	n.End = p.prevEnd()
	return p.addNode(n)
}

var binPrec = func() map[uint8]int {
	m := map[uint8]int{
		mustPunct("||"): 1, mustPunct("&&"): 2, mustPunct("|"): 3,
		mustPunct("^"): 4, mustPunct("&"): 5, mustPunct("=="): 6, mustPunct("!="): 6,
		mustPunct("<"): 7, mustPunct(">"): 7, mustPunct("<="): 7, mustPunct(">="): 7,
		mustPunct("<<"): 8, mustPunct(">>"): 8, mustPunct("+"): 9, mustPunct("-"): 9,
		mustPunct("*"): 10, mustPunct("/"): 10, mustPunct("%"): 10,
	}
	return m
}()

func isAssignOp(code uint8) bool {
	switch code {
	case PEq:
		return true
	}
	t := punctTable[code]
	if len(t) == 3 && (t == ">>=" || t == "<<=") {
		return true
	}
	return len(t) == 2 && t[1] == '=' && t[0] != '=' && t[0] != '!' && t[0] != '<' && t[0] != '>'
}

func (p *parser) expr() uint32 {
	l := p.assignment()
	for p.atPunct(PComma) {
		p.advance()
		r := p.assignment()
		l = p.addNode(Node{Kind: NComma, A: l, B: r, Off: p.fa.Nodes[l].Off,
			End: p.fa.Nodes[r].End, Line: p.fa.Nodes[l].Line, Col: p.fa.Nodes[l].Col})
	}
	return l
}

func (p *parser) assignment() uint32 {
	c := p.binary(1)
	if p.atPunct(PTerse) {
		p.advance()
		th := p.expr()
		p.expectPunct(PColon, "in ?:")
		el := p.assignment()
		return p.addNode(Node{Kind: NCond, A: c, B: th, C: el, Off: p.fa.Nodes[c].Off,
			End: p.fa.Nodes[el].End, Line: p.fa.Nodes[c].Line, Col: p.fa.Nodes[c].Col})
	}
	if t := p.cur(); t.Kind == TPunct && isAssignOp(t.Punct) {
		p.advance()
		r := p.assignment()
		return p.addNode(Node{Kind: NAssign, Flags: uint16(t.Punct), A: c, B: r,
			Off: p.fa.Nodes[c].Off, End: p.fa.Nodes[r].End,
			Line: p.fa.Nodes[c].Line, Col: p.fa.Nodes[c].Col})
	}
	return c
}

func (p *parser) binary(minPrec int) uint32 {
	l := p.unary()
	for {
		t := p.cur()
		if t.Kind != TPunct {
			return l
		}
		prec, ok := binPrec[t.Punct]
		if !ok || prec < minPrec {
			return l
		}
		p.advance()
		r := p.binary(prec + 1)
		n := Node{Kind: NBinary, Flags: uint16(t.Punct), A: l, B: r,
			Off: p.fa.Nodes[l].Off, End: p.fa.Nodes[r].End,
			Line: p.fa.Nodes[l].Line, Col: p.fa.Nodes[l].Col}
		l = p.addNode(n)
	}
}

func (p *parser) isTypeStart() bool {
	t := p.cur()
	if t.Kind != TIdent {
		return false
	}
	switch t.Text {
	case "const", "volatile", "restrict", "_Atomic", "struct", "union", "enum",
		"void", "char", "short", "int", "long", "float", "double", "signed",
		"unsigned", "_Bool", "bool", "_Complex", "_Imaginary", "typeof",
		"typeof_unqual", "_BitInt", "_Decimal32", "_Decimal64", "_Decimal128",
		"constexpr", "alignas", "_Alignas":
		return true
	}
	return p.isTypedefName(t.Text)
}

func (p *parser) unary() uint32 {
	t := p.cur()
	if t.Kind == TPunct {
		switch t.Punct {
		case PInc, PDec, PPlus, PMinus, PBang, PTilde:
			p.advance()
			o := p.unary()
			return p.addNode(Node{Kind: NUnary, Flags: uint16(t.Punct), A: o,
				Off: t.Off, End: p.fa.Nodes[o].End, Line: t.Line, Col: t.Col})
		case PStar:
			p.advance()
			o := p.unary()
			return p.addNode(Node{Kind: NUnary, Flags: uint16(PStar), A: o, Off: t.Off,
				End: p.fa.Nodes[o].End, Line: t.Line, Col: t.Col})
		case PAmp:
			p.advance()
			o := p.unary()
			return p.addNode(Node{Kind: NUnary, Flags: uint16(PAmp), A: o, Off: t.Off,
				End: p.fa.Nodes[o].End, Line: t.Line, Col: t.Col})
		case mustPunct("&&"):
			p.advance()
			if p.cur().Kind == TIdent {
				nm := p.advance()
				return p.addNode(Node{Kind: NIdent, A: p.fa.intern(nm.Text),
					Flags: 1, Off: t.Off, End: nm.End, Line: t.Line, Col: t.Col})
			}
		case PLparen:

			if p.peek(1).Kind != TEOF && (p.isTypeStartAt(1) || p.inferCastType()) {
				save := p.pos
				p.advance()
				tn := p.parseTypeName()
				if p.acceptPunct(PRparen) {
					if p.atPunct(PLbrace) {
						il := p.initializer()
						return p.addNode(Node{Kind: NCompoundLit, A: p.addTypeD(tn), B: il,
							Off: t.Off, End: p.fa.Nodes[il].End, Line: t.Line, Col: t.Col})
					}
					o := p.unary()
					return p.addNode(Node{Kind: NCast, A: p.addTypeD(tn), B: o,
						Off: t.Off, End: p.fa.Nodes[o].End, Line: t.Line, Col: t.Col})
				}
				p.pos = save
			}
		}
	}
	if t.Kind == TIdent {
		switch t.Text {
		case "sizeof":
			p.advance()
			if p.atPunct(PLparen) && (p.isTypeStartAt(1) || p.inferSizeofType()) {
				save := p.pos
				p.advance()
				tn := p.parseTypeName()
				if p.acceptPunct(PRparen) {
					return p.addNode(Node{Kind: NSizeofT, A: p.addTypeD(tn), Off: t.Off,
						End: p.prevEnd(), Line: t.Line, Col: t.Col})
				}
				p.pos = save
			}
			o := p.unary()
			return p.addNode(Node{Kind: NSizeofE, A: o, Off: t.Off,
				End: p.fa.Nodes[o].End, Line: t.Line, Col: t.Col})
		case "_Alignof", "alignof", "__alignof", "__alignof__":
			p.advance()
			p.expectPunct(PLparen, "after alignof")
			tn := p.parseTypeName()
			p.expectPunct(PRparen, "after alignof type")
			return p.addNode(Node{Kind: NAlignofT, A: p.addTypeD(tn), Off: t.Off,
				End: p.prevEnd(), Line: t.Line, Col: t.Col})
		case "__extension__":
			p.advance()
			return p.unary()
		case "_Generic":
			return p.genericSelection()
		case "__real", "__imag":
			p.advance()
			o := p.unary()
			return p.addNode(Node{Kind: NUnary, Flags: uint16(PStar), A: o, Off: t.Off,
				End: p.fa.Nodes[o].End, Line: t.Line, Col: t.Col})
		}
	}
	return p.postfix()
}

func (p *parser) isTypeStartAt(n int) bool {
	t := p.peek(n)
	if t.Kind != TIdent {
		return false
	}
	switch t.Text {
	case "const", "volatile", "restrict", "_Atomic", "struct", "union", "enum",
		"void", "char", "short", "int", "long", "float", "double", "signed",
		"unsigned", "_Bool", "bool", "_Complex", "_Imaginary", "typeof",
		"typeof_unqual", "_BitInt", "_Decimal32", "_Decimal64", "_Decimal128",
		"constexpr", "register":
		return true
	}
	return p.isTypedefName(t.Text)
}

func (p *parser) addTypeD(tn tname) uint32 {
	p.fa.Types = append(p.fa.Types, TypeD{
		TextIdx: p.fa.intern(tn.text), Ptr: int32(tn.ptr),
		Array: tn.isArray, IsFn: tn.isFn,
	})
	return uint32(len(p.fa.Types))
}

func (p *parser) genericSelection() uint32 {
	t := p.advance()
	p.expectPunct(PLparen, "after _Generic")
	ctrl := p.assignment()
	a0 := uint32(len(p.fa.Assocs))
	nAss := uint32(0)
	for p.acceptPunct(PComma) {
		asc := GenericAssoc{}
		if p.atIdent("default") {
			p.advance()
			asc.IsDefault = true
		} else {
			tn := p.parseTypeName()
			asc.TypeIdx = p.addTypeD(tn)
		}
		p.expectPunct(PColon, "in _Generic association")
		asc.Expr = p.assignment()
		p.fa.Assocs = append(p.fa.Assocs, asc)
		nAss++
	}
	p.expectPunct(PRparen, "to close _Generic")
	return p.addNode(Node{Kind: NGeneric, A: ctrl, B: a0, C: nAss, Off: t.Off,
		End: p.prevEnd(), Line: t.Line, Col: t.Col})
}

func (p *parser) postfix() uint32 {
	e := p.primary()
	for !p.eof() {
		t := p.cur()
		if t.Kind != TPunct {
			return e
		}
		switch t.Punct {
		case PLbrack:
			p.advance()
			ix := p.expr()
			p.expectPunct(PRbrack, "to close index")
			e = p.addNode(Node{Kind: NIndex, A: e, B: ix, Off: p.fa.Nodes[e].Off,
				End: p.prevEnd(), Line: p.fa.Nodes[e].Line, Col: p.fa.Nodes[e].Col})
		case PLparen:
			p.advance()
			a0 := uint32(len(p.fa.Nodes))
			nArgs := uint32(0)
			if !p.atPunct(PRparen) {
				for {
					p.assignment()
					nArgs++
					if !p.acceptPunct(PComma) {
						break
					}
				}
			}
			p.expectPunct(PRparen, "to close call args")
			e = p.addNode(Node{Kind: NCall, A: e, B: nArgs, C: a0,
				Off: p.fa.Nodes[e].Off, End: p.prevEnd(),
				Line: p.fa.Nodes[e].Line, Col: p.fa.Nodes[e].Col})
		case PDot, PArrow:
			p.advance()
			if p.cur().Kind != TIdent {
				p.errf(p.cur(), "expected member name")
				return e
			}
			nm := p.advance()
			e = p.addNode(Node{Kind: NMember, A: p.fa.intern(nm.Text), B: e,
				Flags: b2iu(t.Punct == PArrow), Off: p.fa.Nodes[e].Off, End: nm.End,
				Line: p.fa.Nodes[e].Line, Col: p.fa.Nodes[e].Col})
		case PInc, PDec:
			p.advance()
			e = p.addNode(Node{Kind: NPostfix, Flags: uint16(t.Punct), A: e,
				Off: p.fa.Nodes[e].Off, End: p.prevEnd(),
				Line: p.fa.Nodes[e].Line, Col: p.fa.Nodes[e].Col})
		default:
			return e
		}
	}
	return e
}

func b2iu(b bool) uint16 {
	if b {
		return 1
	}
	return 0
}

func (p *parser) primary() uint32 {
	t := p.cur()
	switch t.Kind {
	case TNumber:
		p.advance()
		return p.addNode(Node{Kind: NNumLit, A: p.fa.intern(t.Text), Off: t.Off,
			End: t.End, Line: t.Line, Col: t.Col})
	case TChar:
		p.advance()
		return p.addNode(Node{Kind: NCharLit, A: p.fa.intern(t.Text), Off: t.Off,
			End: t.End, Line: t.Line, Col: t.Col})
	case TStr:
		txt := t.Text
		end := t.End
		line := t.Line
		col := t.Col
		p.advance()
		for p.cur().Kind == TStr || p.strMacroHere() {
			nx := p.advance()
			txt += " " + nx.Text
			end = nx.End
		}
		return p.addNode(Node{Kind: NStrLit, A: p.fa.intern(txt), Off: t.Off,
			End: end, Line: line, Col: col})
	case TIdent:
		if nx := p.peek(1); nx.Kind == TStr && !cKeywords[t.Text] {
			// `PRIu64 "..."`: a string macro whose header was not read
			txt := t.Text
			end := t.End
			p.advance()
			for p.cur().Kind == TStr || p.strMacroHere() {
				n2 := p.advance()
				txt += " " + n2.Text
				end = n2.End
			}
			return p.addNode(Node{Kind: NStrLit, A: p.fa.intern(txt), Off: t.Off,
				End: end, Line: t.Line, Col: t.Col})
		}
		switch t.Text {
		case "true", "false":
			p.advance()
			fl := uint16(0)
			if t.Text == "true" {
				fl = 1
			}
			return p.addNode(Node{Kind: NBoolLit, Flags: fl, A: p.fa.intern(t.Text),
				Off: t.Off, End: t.End, Line: t.Line, Col: t.Col})
		case "nullptr":
			p.advance()
			return p.addNode(Node{Kind: NNullPtr, Off: t.Off, End: t.End,
				Line: t.Line, Col: t.Col})
		case "offsetof", "__builtin_offsetof":
			// offsetof(type, member-designator): only reachable spelled this
			// way when <stddef.h> was not found to expand it
			if nx := p.peek(1); nx.Kind == TPunct && nx.Punct == PLparen {
				p.advance()
				p.advance()
				if c := p.cur(); p.plainIdent(c) {
					p.scopes[0].setTypedef(c.Text, true)
				}
				tn := p.parseTypeName()
				depth := 1
				for !p.eof() && depth > 0 {
					c := p.cur()
					if c.Kind == TPunct {
						switch c.Punct {
						case PLparen:
							depth++
						case PRparen:
							depth--
						case PSemi, PLbrace, PRbrace:
							depth = 0
							continue
						}
					}
					if depth > 0 {
						p.advance()
					}
				}
				p.acceptPunct(PRparen)
				return p.addNode(Node{Kind: NSizeofT, A: p.addTypeD(tn), Off: t.Off,
					End: p.prevEnd(), Line: t.Line, Col: t.Col})
			}
		case "va_arg":
			if nx := p.peek(1); nx.Kind != TPunct || nx.Punct != PLparen {
				break
			}
			fallthrough
		case "__builtin_va_arg":
			p.advance()
			p.expectPunct(PLparen, "after __builtin_va_arg")
			e := p.assignment()
			p.expectPunct(PComma, "in __builtin_va_arg")
			tn := p.parseTypeName()
			p.expectPunct(PRparen, "to close __builtin_va_arg")
			return p.addNode(Node{Kind: NVaArg, A: e, B: p.addTypeD(tn), Off: t.Off,
				End: p.prevEnd(), Line: t.Line, Col: t.Col})
		}
		p.advance()
		return p.addNode(Node{Kind: NIdent, A: p.fa.intern(t.Text), Off: t.Off,
			End: t.End, Line: t.Line, Col: t.Col})
	case TPunct:
		if t.Punct == PLparen {

			nx := p.peek(1)
			if nx.Kind == TPunct && nx.Punct == PLbrace {
				p.advance()
				p.advance()
				b := p.block()
				p.expectPunct(PRparen, "to close statement expression")
				return p.addNode(Node{Kind: NStmtExpr, A: b, Off: t.Off,
					End: p.prevEnd(), Line: t.Line, Col: t.Col})
			}
			if nx.Kind == TIdent && p.isTypedefName(nx.Text) {

			}
			p.advance()
			e := p.expr()
			p.expectPunct(PRparen, "to close parenthesis")
			return p.addNode(Node{Kind: NParen, A: e, Off: t.Off, End: p.prevEnd(),
				Line: t.Line, Col: t.Col})
		}
	}
	p.errf(t, "unexpected token '"+t.Text+"' in expression")

	p.advance()
	return p.addNode(Node{Kind: NBad, Off: t.Off, End: t.End,
		Line: t.Line, Col: t.Col})
}

func (p *parser) condExprNode() uint32 { return p.assignment() }

func (p *parser) addNode(n Node) uint32 {
	return p.fa.addNode(n)
}

func (p *parser) initializer() uint32 {
	if !p.atPunct(PLbrace) {
		return p.assignment()
	}
	ob := p.advance()
	mk := p.addNode(Node{Kind: NInitList, Off: ob.Off, End: ob.End,
		Line: ob.Line, Col: ob.Col})
	p.fa.Nodes[mk].A = uint32(len(p.fa.Nodes))
	for !p.eof() && !p.atPunct(PRbrace) {
		item := InitD{}
		desig := false
		for {
			if p.atPunct(PDot) {
				p.advance()
				if p.cur().Kind == TIdent {
					item.FieldIdx = p.fa.intern(p.advance().Text)
				} else {
					p.errf(p.cur(), "expected field designator name")
				}
				desig = true
				continue
			}
			if p.atPunct(PLbrack) {
				p.advance()
				p.assignment()
				if p.acceptPunct(mustPunct("...")) {
					p.assignment()
				}
				p.expectPunct(PRbrack, "to close index designator")
				item.HasIndex = true
				desig = true
				continue
			}
			break
		}
		if desig {
			p.expectPunct(PEq, "after designator")
		}
		val := p.initializer()
		item.IsList = p.fa.Nodes[val].Kind == NInitList
		p.fa.Inits = append(p.fa.Inits, item)
		n := Node{Kind: NInitItem, A: uint32(len(p.fa.Inits)), B: val}
		p.addNode(n)
		if !p.acceptPunct(PComma) {
			break
		}
	}
	p.expectPunct(PRbrace, "to close initializer list")
	p.fa.Nodes[mk].B = uint32(len(p.fa.Nodes)) - p.fa.Nodes[mk].A
	if p.pos > 0 {
		p.fa.Nodes[mk].End = p.toks[p.pos-1].End
	}
	return mk
}

type bodyWalk struct {
	cyclomatic, cognitive, maxNesting  int32
	nLoops, nBranches, nSwitch, nCases int32
	nLabels, nGotos, nReturns          int32
	nDeref, nCast, nSizeof             int32
	nArith, nCmp, nShift               int32
	nLogical, nTernary, nAssign        int32
	nCompoundAssign, nIncdec           int32
	nFloatLit, nMagic, nNullCheck      int32
	nPtrLocals, nVla                   int32
	nTokens, nOperators, nOperands     int32
	nDistOps, nDistOper                int32
	hasVolatile, hasRestrict           int32
	nStaticAssert, nAtomic, nIntrinsic int32
	nLikely, nBuiltin                  int32
	nGetenv, nErrno                    int32
	maxLoopDepth                       int32

	callInLoop, allocInLoop, libmInLoop       int32
	strlenInLoop, ioInLoop, lockInLoop        int32
	switchInLoop, branchInLoop, divInLoop     int32
	retNull, retNeg, retZero, retVal, retVoid int32
	nStringLit, nAddrof, nArrow, nDot         int32
	nSubscript, nBitop                        int32
	nGlobalWrite                              int32

	locals []LocalRow
	lits   []LiteralRow
	addrs  []AddrTakenRow
	calls  []callSiteRec

	opSeen, operSeen     map[string]bool
	toctouChk, toctouOpn map[string]bool
	castIdents           []string

	// names a parameter or local has taken, so a write to one is not
	// mistaken for a write to the file-scope name it hides
	shadow map[string]bool
	// declared type of each parameter and local, for the checks that need to
	// know what a name IS rather than how it is spelled
	types    map[string]string
	nSignCmp int32

	// Names used as VALUES: a function handed to a callback parameter, stored
	// in a struct field, or listed in a file-scope table. A block's nodes are
	// visited in creation order -- operands before the call or `&` that owns
	// them -- so the use is recorded first and its role settled afterwards.
	refs      []identRef
	wantRefs  bool
	fileScope bool

	// Per-node flags for this walk, kept in one dense array shared by every
	// walk of the translation unit instead of three hash maps per function:
	// a slot belongs to the current walk iff its upper bits equal ep.
	mk []uint32
	ep uint32

	// facts three queries used to approximate from totals
	frameBytes   int64           // bytes of automatic storage declared, where the type has a known size
	frameUnsized int32           // automatic locals whose size is not known here (or is a VLA)
	frameMax     int32           // the largest single local
	divisors     []string        // plain-name divisors, one per division
	nDivByVar    int32           // divisions whose divisor is not a literal
	zeroTested   map[string]bool // names compared with a literal or used as a condition
	nForever     int32           // loops whose condition is a non-zero constant
	nForeverNoX  int32           // ... with no break/return/goto/exit inside
	discards     map[string]int32
	discardOrd   []string
}

// noteTested records every plain name in a condition: `if (n)`, `!n`,
// `n == 0`, `n > 0 && m`.
func (df *derivedFile) noteTested(w *bodyWalk, idx uint32, depth int) {
	if idx == 0 || depth > 8 || w.zeroTested == nil {
		return
	}
	nd := &df.ast.Nodes[idx]
	switch nd.Kind {
	case NIdent:
		w.zeroTested[df.ast.name(nd.A)] = true
	case NParen, NUnary:
		df.noteTested(w, nd.A, depth+1)
	case NBinary:
		df.noteTested(w, nd.A, depth+1)
		df.noteTested(w, nd.B, depth+1)
	}
}

// constTrue: `1`, `(1)`, `true` -- the condition of a loop meant to run forever.
func (df *derivedFile) constTrue(idx uint32) bool {
	for hop := 0; idx != 0 && hop < 4; hop++ {
		nd := &df.ast.Nodes[idx]
		switch nd.Kind {
		case NParen:
			idx = nd.A
		case NNumLit:
			v, ok := parseCInt(df.ast.name(nd.A))
			return ok && v != 0
		case NBoolLit:
			return nd.Flags == 1
		default:
			return false
		}
	}
	return false
}

// leavesLoop reports whether any node of the statement at idx can end the
// loop that owns it: break, return, goto, or a call that does not come back.
// A break belonging to a nested loop or switch counts too, so this errs
// toward "has an exit" and the query toward silence.
func (df *derivedFile) leavesLoop(idx uint32) bool {
	fa := df.ast
	if idx == 0 {
		return false
	}
	lo, hi := idx, idx+1
	if nd := &fa.Nodes[idx]; nd.Kind == NCompoundStmt {
		lo, hi = nd.A, nd.A+nd.B
	} else {
		// a single statement: its operands were created just before it
		lo = idx
		for lo > 0 && fa.Nodes[lo-1].Off >= nd.Off && fa.Nodes[lo-1].End <= nd.End &&
			fa.Nodes[lo-1].Kind != NCompoundStmt {
			lo--
		}
	}
	for i := lo; i < hi && int(i) < len(fa.Nodes); i++ {
		nd := &fa.Nodes[i]
		switch nd.Kind {
		case NBreak, NReturn, NGoto:
			return true
		case NCall:
			if c := &fa.Nodes[nd.A]; c.Kind == NIdent {
				switch fa.name(c.A) {
				case "exit", "_exit", "_Exit", "abort", "longjmp", "siglongjmp",
					"pthread_exit", "quick_exit", "thrd_exit", "__builtin_unreachable",
					"__builtin_trap":
					return true
				}
			}
		}
	}
	return false
}

// noteDivisor classifies the right operand of `/`, `%`, `/=`, `%=`. A literal
// or a sizeof cannot be zero by accident and is not counted at all.
func (df *derivedFile) noteDivisor(w *bodyWalk, idx uint32) {
	fa := df.ast
	for hop := 0; idx != 0 && hop < 4; hop++ {
		nd := &fa.Nodes[idx]
		switch nd.Kind {
		case NParen:
			idx = nd.A
			continue
		case NNumLit, NCharLit, NSizeofT, NSizeofE, NAlignofT:
			return
		case NIdent:
			w.nDivByVar++
			w.divisors = append(w.divisors, fa.name(nd.A))
			return
		}
		break
	}
	w.nDivByVar++
}

func (df *derivedFile) noteLoop(w *bodyWalk, cond, body uint32, forever bool) {
	if !forever && !df.constTrue(cond) {
		return
	}
	w.nForever++
	if !df.leavesLoop(body) {
		w.nForeverNoX++
	}
}

type identRef struct {
	node uint32
	name string
	line int32
}

const (
	mkVisited = 1
	mkCallee  = 2
	mkAmp     = 4
)

func (w *bodyWalk) has(idx uint32, bit uint32) bool {
	v := w.mk[idx]
	return v&^7 == w.ep && v&bit != 0
}

func (w *bodyWalk) set(idx uint32, bit uint32) {
	if w.mk[idx]&^7 != w.ep {
		w.mk[idx] = w.ep
	}
	w.mk[idx] |= bit
}

// newWalk returns a walker bound to the unit's mark array under a fresh epoch.
func (df *derivedFile) newWalk() *bodyWalk {
	n := len(df.ast.Nodes)
	var mk []uint32
	if df.tu != nil {
		if len(df.tu.marks) < n {
			df.tu.marks = make([]uint32, n)
			df.tu.markEp = 0
		}
		df.tu.markEp++
		mk = df.tu.marks
		return &bodyWalk{mk: mk, ep: df.tu.markEp << 3,
			opSeen: map[string]bool{}, operSeen: map[string]bool{}}
	}
	if len(df.marks) < n {
		df.marks = make([]uint32, n)
		df.markEp = 0
	}
	df.markEp++
	return &bodyWalk{mk: df.marks, ep: df.markEp << 3,
		opSeen: map[string]bool{}, operSeen: map[string]bool{}}
}

// takeRefs turns the recorded value-uses into addr_taken rows: `ref` for a
// bare name, `addr` for one under `&`. A name in callee position is a call,
// which the call graph already has.
func (w *bodyWalk) takeRefs() []AddrTakenRow {
	var out []AddrTakenRow
	for _, r := range w.refs {
		if w.has(r.node, mkCallee) {
			continue
		}
		kind := "ref"
		if w.has(r.node, mkAmp) {
			kind = "addr"
		}
		out = append(out, AddrTakenRow{Name: jtrunc(r.name, 80), Line: r.line, Kind: kind})
	}
	return out
}

type derivedFile struct {
	file    FileRow
	symbols []SymbolRow

	funcSymByNode map[uint32]int32
	curFuncNode   uint32
	lastFuncNode  uint32
	globalFileID  int32
	params        []ParamRow
	fields        []FieldRow
	locals        []LocalRow
	lits          []LiteralRow
	markers       []MarkerRow
	attrs         []AttributeRow
	imports       []ImportRow
	hazards       []HazardRow
	enums         []EnumMemberRow
	layout        []LayoutRowR
	ssize         []StructSizeRow
	decls         []DeclarationRow
	addrs         []AddrTakenRow
	secrets       []SecretCandidateRow
	allocs        []AllocSiteRow
	memops        []MemopRow
	macros        []MacroRow
	globals       []GlobalRow
	cfgs          []ConfigBlockRow
	locks         []LockRowR
	evops         []EventOpRow
	apiuses       []APIUseRow

	pendNames  []string
	pendLines  [][]int32
	pendSym    []int32
	pendFile   []int32
	pendModule []int32

	fnNames   []string
	fnSyms    []int32
	fnFiles   []int32
	fnStatic  []bool
	tuFuncs   map[string]bool
	tu        *c23tu
	marks     []uint32
	markEp    uint32
	tagSz     map[uint32][3]int32
	typeSz    map[string][3]int32
	lastTag   uint32
	fnModules []int32

	declared  map[string]bool
	macroSID  map[string]int32
	macroName []string

	ast     *FileAST
	pp      *preprocessor
	incRows []includeEdge
	incSeen map[int32]bool
	fnSpans [][3]int32
	srcRaw  []byte
	perr    []parseMsg
	offAt   []int32
	offAdd  []int32
	blank   []byte
	nlRaw   []int32
	srcID   uint32
	relPath string
	isTestF int32
	isGenF  int32
	modID   int32
	glSet   map[string]bool
}

// globalSet is every file-scope object name this translation unit can write:
// the file's own globals (added as they are derived, so the set is never
// stale) plus each `extern T name;` an included header declared. Without the
// second half `server.stat_x++` -- the dominant shape in a tree with one
// central state struct -- is invisible in every file but the defining one.
func (df *derivedFile) globalSet() map[string]bool {
	if df.glSet == nil {
		m := make(map[string]bool, len(df.globals)*2+16)
		for i := range df.globals {
			m[df.globals[i].Name] = true
		}
		fa := df.ast
		for ni := range fa.Nodes {
			n := &fa.Nodes[ni]
			if n.Kind != NDecl || n.File == 0 || n.File == df.srcID {
				continue
			}
			d := &fa.Decls[n.A-1]
			if d.Kind == 0 && d.NameIdx != 0 && d.Storage&FExtern != 0 {
				m[fa.name(d.NameIdx)] = true
			}
		}
		df.glSet = m
	}
	return df.glSet
}

func newDerivedFile(fa *FileAST, pp *preprocessor, raw []byte, fileID uint32) *derivedFile {
	return &derivedFile{ast: fa, pp: pp, srcRaw: raw, srcID: fileID,
		incSeen:  map[int32]bool{},
		declared: map[string]bool{}, macroSID: map[string]int32{},
		funcSymByNode: map[uint32]int32{}}
}

func (df *derivedFile) internSym(s *SymbolRow) int32 {

	df.symbols = append(df.symbols, *s)
	return int32(len(df.symbols))
}

// Token offsets index the buffer the LEXER read, which is the file with its BOM
// dropped, every backslash-newline spliced out and every CR folded away. srcRaw
// is the file as it sits on disk, so an offset is only valid there after the
// bytes removed before it are added back. Without this every text heuristic in
// a file was shifted two bytes per continuation line above it: a lock argument
// `&the_big_mutex` came back as `mutex_lock`.
func (df *derivedFile) buildOffMap() {
	raw := df.srcRaw
	base := int32(len(raw) - len(stripBOM(raw)))
	src := raw[base:]
	need := false
	for i := 0; i < len(src); i++ {
		c := src[i]
		if c == '\r' || (c == '\\' && i+1 < len(src) && (src[i+1] == '\n' || src[i+1] == '\r')) {
			need = true
			break
		}
	}
	if !need {
		if base != 0 {
			df.offAt, df.offAdd = []int32{0}, []int32{base}
		}
		return
	}
	// the same walk spliceSource does, keeping only where the gap changes
	last := int32(-1 << 30)
	k, oo := int32(0), int32(0)
	emit := func() {
		if d := oo - k + base; d != last {
			df.offAt = append(df.offAt, k)
			df.offAdd = append(df.offAdd, d)
			last = d
		}
		k++
	}
	n := len(src)
	for i := 0; i < n; {
		c := src[i]
		if c == '\\' && i+1 < n {
			if src[i+1] == '\n' {
				oo += 2
				i += 2
				continue
			}
			if src[i+1] == '\r' {
				adv := 2
				if i+2 < n && src[i+2] == '\n' {
					adv = 3
				}
				oo += int32(adv)
				i += adv
				continue
			}
		}
		emit()
		oo++
		i++
		if c == '\r' && i < n && src[i] == '\n' {
			oo++
			i++
		}
	}
}

// rawOff maps one lexer offset to its srcRaw offset.
func (df *derivedFile) rawOff(off int32) int32 {
	at := df.offAt
	if len(at) == 0 || off < at[0] {
		return off
	}
	lo, hi := 0, len(at)
	for lo < hi {
		mid := (lo + hi) / 2
		if at[mid] <= off {
			lo = mid + 1
		} else {
			hi = mid
		}
	}
	return off + df.offAdd[lo-1]
}

// rawRange maps a half-open lexer span. The end is mapped through its last
// byte so a continuation directly after the span is not pulled into it.
func (df *derivedFile) rawRange(off, end int32) (int32, int32) {
	if end <= off {
		r := df.rawOff(off)
		return r, r
	}
	return df.rawOff(off), df.rawOff(end-1) + 1
}

func (df *derivedFile) rawSpan(off, end int32, cap int) string {
	off, end = df.rawRange(off, end)
	if off < 0 || end < off || int(end) > len(df.srcRaw) {
		return ""
	}
	return jtrunc(squeeze(string(df.srcRaw[off:end]), ""), cap)
}

func (df *derivedFile) derive() {
	fa := df.ast

	for ni := range fa.Nodes {
		n := &fa.Nodes[ni]
		if n.File != 0 && n.File != df.srcID {
			continue
		}
		switch n.Kind {
		case NStructSpec, NUnionSpec, NEnumSpec:
			td := &fa.Tags[n.A-1]
			if td.Defined {
				df.deriveTag(td, n.A)
			}
		case NDecl:
			d := &fa.Decls[n.A-1]
			if df.insideFunc(n.Off) {
				continue
			}
			switch d.Kind {
			case 2:
				df.deriveTypedef(d, n)
			case 1:
				df.derivePrototype(d, n)
			default:
				if d.NameIdx != 0 {
					df.deriveGlobal(d, n)
				}
			}
		case NFuncDef:
			fd := &fa.Funcs[n.A-1]
			df.curFuncNode = uint32(ni)
			df.deriveFunction(fd, n)
		}
	}

	for ni := range fa.Nodes {
		n := &fa.Nodes[ni]
		if n.Kind != NDecl || (n.File != 0 && n.File != df.srcID) {
			continue
		}
		d := &fa.Decls[n.A-1]
		if d.Init == 0 || df.insideFunc(n.Off) {
			continue
		}
		w := df.newWalk()
		w.wantRefs, w.fileScope = true, true
		df.walkStmt(w, d.Init, 1, 0)
		for i := range w.lits {
			l := w.lits[i]
			l.FileID = int32(df.srcID)
			l.ID = int32(len(df.lits) + 1)
			df.lits = append(df.lits, l)
		}
		// every name in a file-scope initializer; postBuild keeps the ones
		// that turn out to be functions -- the dispatch-table entries
		for _, a := range w.takeRefs() {
			a.FileID = int32(df.srcID)
			a.ID = int32(len(df.addrs) + 1)
			df.addrs = append(df.addrs, a)
		}
	}

	for _, m := range df.pp.allMacros {
		if m.FileID != df.srcID {
			continue
		}
		df.deriveMacro(m)
	}

	scanMarkersRaw(df.srcRaw, func(kind string, line int32, text string) {
		df.markers = append(df.markers, MarkerRow{FileID: int32(df.srcID),
			Kind: kind, Line: line, Text: text})
	})

	for _, c := range df.pp.cfgs {
		if c.fileID != df.srcID || c.directive == "endif" {
			continue
		}
		expr := jtrunc(strings.TrimSpace(c.text()), 160)
		isCfg := b2i(strings.Contains(expr, "CONFIG_") ||
			strings.Contains(expr, "HAVE_") || strings.Contains(expr, "USE_"))
		df.cfgs = append(df.cfgs, ConfigBlockRow{FileID: int32(df.srcID),
			Directive: c.directive, Expr: expr, Line: c.line, IsConfig: isCfg})
	}

	for _, inc := range df.pp.includes {
		if inc.fromID != df.srcID {
			continue
		}
		df.imports = append(df.imports, ImportRow{FileID: int32(df.srcID),
			Target: inc.target, Kind: "include", Line: inc.line,
			IsExternal: 1, NNames: 1, IsRelative: b2i(!inc.sys)})
		if _, ok := df.incSeen[inc.line]; !ok {
			df.incSeen[inc.line] = true
			df.incRows = append(df.incRows, inc)
		}
	}

	df.deriveAttrs()
}

func (df *derivedFile) funcSpans() [][3]int32 {
	if df.fnSpans == nil {
		spans := make([][3]int32, 0, 16)
		for ni := range df.ast.Nodes {
			n := &df.ast.Nodes[ni]
			// Offsets are per file: a span from an included header's inline
			// function would swallow this file's own file-scope declarations.
			if n.Kind == NFuncDef && (n.File == 0 || n.File == df.srcID) {
				spans = append(spans, [3]int32{n.Off, n.End, int32(ni)})
			}
		}
		df.fnSpans = spans
	}
	return df.fnSpans
}

func (df *derivedFile) insideFunc(off int32) bool {
	spans := df.funcSpans()
	for i := range spans {
		if off >= spans[i][0] && off < spans[i][1] {
			return true
		}
	}
	return false
}

func (df *derivedFile) deriveAttrs() {
	for i := range df.ast.Attrs {
		a := &df.ast.Attrs[i]
		row := AttributeRow{FileID: int32(df.srcID), Name: jtrunc(a.Name, 80),
			Args: jtrunc(a.Args, 200), HasArgs: a.Args != "", Line: a.Line}

		best := uint32(0)
		spans := df.funcSpans()
		for i := range spans {
			if a.Off >= spans[i][0] && a.Off < spans[i][1] {
				best = uint32(spans[i][2])
				break
			}
		}
		if sid, ok := df.funcSymByNode[best]; ok {
			row.HasSymbolID = true
			row.SymbolID = sid
		}
		df.attrs = append(df.attrs, row)
	}
}

// aggBase reduces a member's type text to what names its aggregate:
// `const struct foo` -> `struct foo`, `volatile union` -> `union`.
func aggBase(t string) string {
	for _, w := range [...]string{"const", "volatile", "_Atomic", "register"} {
		t = replaceWord(t, w, " ")
	}
	return trimSpace(squeeze(t, ""))
}

func (df *derivedFile) deriveTag(td *TagD, tagIdx uint32) {
	fa := df.ast
	name := fa.name(td.NameIdx)
	if name == "" {
		name = "(anon@" + strconv.Itoa(int(td.Line)) + ")"
	}
	kind := td.Tag
	sym := SymbolRow{FileID: int32(df.srcID), Name: name, QualName: name,
		Kind: kind, LineStart: td.Line, IsPublic: 1, HasReturnType: false,
		LineEnd: td.LineEnd, NLines: td.LineEnd - td.Line + 1}
	localsym := df.internSym(&sym)

	switch kind {
	case "struct", "union":
		isUnion := kind == "union"
		var flds []fld
		ord := int32(0)
		// Child0..NChild is every node created inside the braces, so it also
		// holds the members of any struct or union DEFINED inline here.
		//   - behind an ANONYMOUS member (`struct { int x; };`) they are
		//     members of this type too (C11 6.7.2.1): they stay listed, at
		//     depth 1+, and are not laid out a second time -- the unnamed
		//     marker row after them carries the inner type's offset and size
		//   - behind a NAMED member (`union { ... } u;`) they belong to the
		//     inner type only and are not listed here at all
		// Before this, both kinds were laid out at consecutive offsets beside
		// the member that contains them, so every such struct had the wrong
		// size and the wrong padding.
		var nestN []int32 // how many inline definitions enclose node ci
		var hide []bool   // one of them is a named member
		var inUni []bool  // the innermost one is a union
		lastAnon := uint32(0)
		anonAt := map[uint32]uint32{}
		type span struct {
			lo, hi, at uint32
			union      bool
		}
		var spans []span
		for ci := uint32(0); ci < td.NChild; ci++ {
			sn := &fa.Nodes[td.Child0+ci]
			if sn.Kind != NStructSpec && sn.Kind != NUnionSpec {
				continue
			}
			if sn.A == 0 || sn.A == tagIdx {
				continue
			}
			it := &fa.Tags[sn.A-1]
			if !it.Defined || it.Child0 < td.Child0 || it.Child0+it.NChild > td.Child0+td.NChild {
				continue
			}
			spans = append(spans, span{it.Child0 - td.Child0,
				it.Child0 - td.Child0 + it.NChild, ci, sn.Kind == NUnionSpec})
			if it.NameIdx == 0 {
				anonAt[ci] = sn.A
			}
		}
		if len(spans) > 0 {
			nestN = make([]int32, td.NChild)
			hide = make([]bool, td.NChild)
			inUni = make([]bool, td.NChild)
			for _, sp := range spans {
				for k := sp.lo; k < sp.hi; k++ {
					nestN[k]++
				}
			}
			// spans arrive innermost-first (a spec node follows its members),
			// so the first one to claim a node is its innermost
			seen := make([]bool, td.NChild)
			for _, sp := range spans {
				for k := sp.lo; k < sp.hi; k++ {
					if !seen[k] {
						seen[k] = true
						inUni[k] = sp.union
					}
				}
				// the member declared with this inner type is the next field
				// at the spec's own level; unnamed means anonymous
				named := true
				for k := sp.at + 1; k < td.NChild; k++ {
					fn := &fa.Nodes[td.Child0+k]
					if fn.Kind != NField || nestN[k] != nestN[sp.at] {
						continue
					}
					named = fa.Fields[fn.A-1].NameIdx != 0
					break
				}
				if named {
					for k := sp.lo; k < sp.hi; k++ {
						hide[k] = true
					}
				}
			}
		}
		for ci := uint32(0); ci < td.NChild; ci++ {
			if a, ok := anonAt[ci]; ok && (nestN == nil || nestN[ci] == 0) {
				lastAnon = a
			}
			if hide != nil && hide[ci] {
				continue
			}
			fn := &fa.Nodes[td.Child0+ci]
			if fn.Kind != NField {
				continue
			}
			fdp := &fa.Fields[fn.A-1]
			fname := fa.name(fdp.NameIdx)
			ftype := fa.name(fdp.PtrTypeIdx)
			df.fields = append(df.fields, FieldRow{SymbolID: localsym,
				Ordinal: ord, Name: jtrunc(fname, 80), Type: jtrunc(ftype, 160),
				Line: fdp.Line, IsConst: b2i(fdp.IsConst), IsMutable: b2i(!fdp.IsConst),
				IsNullable: b2i(fdp.Ptr > 0), IsCollection: b2i(fdp.ArrayLen != 0),
				TypeDepth: fdp.Ptr})
			f := fld{ordinal: ord, ftype: fa.name(fdp.TypeIdx),
				fname: fname, ptr: fdp.Ptr, alen: fdp.ArrayLen,
				isFnptr: b2i(fdp.IsFnptr), depth: 0, inUnion: 0, line: fdp.Line}
			if nestN != nil && nestN[ci] > 0 {
				f.depth = nestN[ci]
				f.inUnion = b2i(inUni[ci])
			}
			if fdp.Ptr == 0 && !fdp.IsFnptr && f.depth == 0 {
				switch base := aggBase(f.ftype); {
				case base == "struct" || base == "union":
					if v, ok := df.tagSz[lastAnon]; ok {
						f.kn, f.ksz, f.kal, f.kex = true, v[0], v[1], v[2]
					}
				case base == "enum" || strings.HasPrefix(base, "enum "):
					f.kn, f.ksz, f.kal, f.kex = true, 4, 4, 1
				default:
					if v, ok := df.typeSz[base]; ok {
						f.kn, f.ksz, f.kal, f.kex = true, v[0], v[1], v[2]
					}
				}
			}
			flds = append(flds, f)
			ord++
		}
		lay, tot, tail, ex, mal := layoutStruct(flds, isUnion)
		tpad := tail
		for i := range lay {
			if i < len(flds) {
				tpad += lay[i][3]
			}
			df.layout = append(df.layout, LayoutRowR{SymbolID: localsym,
				Ordinal: lay[i][0], ByteOff: lay[i][1], ByteSize: lay[i][2],
				PadBefore: lay[i][3], Exact: lay[i][4], PtrDepth: flds[i].ptr,
				ArrayLen: flds[i].alen, IsFnptr: flds[i].isFnptr,
				Depth: flds[i].depth, InUnion: flds[i].inUnion})
		}
		lines64 := int32(0)
		if tot > 0 {
			lines64 = (tot + 63) / 64
		}
		df.ssize = append(df.ssize, StructSizeRow{SymbolID: localsym,
			TotalSize: tot, TailPad: tail, TotalPad: tpad, MaxAlign: mal,
			Exact: ex, NLines64: lines64})
		if df.tagSz == nil {
			df.tagSz = map[uint32][3]int32{}
			df.typeSz = map[string][3]int32{}
		}
		df.tagSz[tagIdx] = [3]int32{tot, mal, ex}
		df.lastTag = tagIdx
		if !isUnion && ex == 1 {
			if best := packedSize(flds); best > 0 && best < tot {
				df.hazards = append(df.hazards, HazardRow{SymbolID: localsym,
					Pattern: "packed_size", Category: "shape", N: best})
			}
		}
		if td.NameIdx != 0 {
			df.typeSz[kind+" "+fa.name(td.NameIdx)] = [3]int32{tot, mal, ex}
		}
	case "enum":
		ord := int32(0)
		for ci := uint32(0); ci < td.NChild; ci++ {
			en := &fa.Nodes[td.Child0+ci]
			if en.Kind != NEnumerator {
				continue
			}
			ed := &fa.Enums[en.A-1]
			em := EnumMemberRow{SymbolID: localsym, Ordinal: ord,
				Name: jtrunc(fa.name(ed.NameIdx), 80), HasValue: ed.HasValue,
				NFields: 0}
			ord++
			if ed.HasValue {
				em.Value = jtrunc(fa.name(ed.ValueIdx), 60)
			}
			df.enums = append(df.enums, em)
		}
	}
}

func (df *derivedFile) deriveTypedef(d *DeclNode, n *Node) {
	name := df.ast.name(d.NameIdx)
	if name == "" {
		return
	}
	ty := df.ast.name(d.TypeIdx)
	// `typedef struct foo foo_t;` and `typedef struct {...} foo_t;` give the
	// struct's size a second name, which is the one members are declared with
	if d.Ptr == 0 && d.ArrayLen == 0 && df.typeSz != nil {
		switch base := aggBase(ty); {
		case base == "struct" || base == "union":
			if v, ok := df.tagSz[df.lastTag]; ok {
				df.typeSz[name] = v
			}
		case base == "enum" || strings.HasPrefix(base, "enum "):
			df.typeSz[name] = [3]int32{4, 4, 1}
		default:
			if v, ok := df.typeSz[base]; ok {
				df.typeSz[name] = v
			}
		}
	}
	df.symbols = append(df.symbols, SymbolRow{FileID: int32(df.srcID),
		Name: name, QualName: name, Kind: "typedef", LineStart: n.Line,
		ReturnType: jtrunc(ty, 120), HasReturnType: true, IsPublic: 1,
		LineEnd: n.Line, NLines: 1})
}

func (df *derivedFile) derivePrototype(d *DeclNode, n *Node) {
	name := df.ast.name(d.NameIdx)
	if name == "" {
		return
	}
	df.declared[name] = true
	df.decls = append(df.decls, DeclarationRow{FileID: int32(df.srcID),
		Name: name, Line: n.Line})
}

func (df *derivedFile) deriveGlobal(d *DeclNode, n *Node) {
	name := df.ast.name(d.NameIdx)
	if name == "" {
		return
	}
	g := GlobalRow{FileID: int32(df.srcID), Name: name,
		Type:       jtrunc(squeeze(df.ast.name(d.TypeIdx), ""), 120),
		Line:       n.Line,
		IsStatic:   b2i(d.Storage&FStatic != 0),
		IsConst:    b2i(d.Storage&FConstexpr != 0 || strings.Contains(df.ast.name(d.TypeIdx), "const")),
		IsVolatile: b2i(strings.Contains(df.ast.name(d.TypeIdx), "volatile")),
		IsAtomic:   b2i(strings.Contains(df.ast.name(d.TypeIdx), "_Atomic")),
		IsArray:    b2i(d.ArrayLen != 0), PtrDepth: d.Ptr,
		HasInit: b2i(d.Init != 0)}
	df.globals = append(df.globals, g)
	if df.glSet != nil {
		df.glSet[name] = true
	}
}

func (df *derivedFile) deriveMacro(m *ppMacro) {
	nParams := int32(0)
	for _, p := range m.Params {
		if strings.TrimSpace(p) != "" {
			nParams++
		}
	}
	sig := jtrunc("define "+m.Name+" "+m.Raw, 200)
	sym := SymbolRow{FileID: int32(df.srcID), Name: m.Name, QualName: m.Name,
		Kind: "macro", LineStart: m.Line, Signature: sig, HasSignature: true,
		LineEnd: m.Line, NLines: 1}
	sid := df.internSym(&sym)
	body := jtrunc(m.Raw, 500)
	ml := int32(len([]rune(m.Raw)))
	df.macroName = append(df.macroName, m.Name)
	if _, ok := df.macroSID[m.Name]; !ok {
		df.macroSID[m.Name] = sid
	}
	df.macros = append(df.macros, MacroRow{SymbolID: sid,
		IsFunctionlike: b2i(m.IsFn), NParams: nParams, Body: body,
		HasBody: true, BodyLen: ml,
		IsMultiline: b2i(m.Multi),
		NUses:       m.Uses})
}

func magicFor(tok string) bool {
	v, err := strconv.ParseInt(stripNumSuffix(strings.ReplaceAll(tok, "'", "")), 0, 64)
	if err != nil {
		return false
	}
	return magicOK[v]
}

func isFloatTok(t string) bool {
	body := stripNumSuffix(strings.ReplaceAll(t, "'", ""))
	if len(body) > 1 && (body[0] == '0' && (body[1] == 'x' || body[1] == 'X')) {
		return strings.ContainsAny(body[2:], "pP.")
	}
	return strings.ContainsAny(body, ".eE")
}

func numKind(t string) string {
	body := strings.ToLower(t)
	if strings.HasPrefix(body, "0x") {
		return "hex"
	}
	if strings.HasPrefix(body, "0b") {
		return "bin"
	}
	if len(body) > 1 && body[0] == '0' && !isFloatTok(t) {
		return "oct"
	}
	if isFloatTok(t) {
		return "float"
	}
	return "int"
}

func (df *derivedFile) deriveFunction(fd *FuncDef, n *Node) {
	fa := df.ast
	df.lastFuncNode = df.curFuncNode
	name := fa.name(fd.NameIdx)
	sig := fa.name(fd.SigIdx)
	ret := fa.name(fd.RetIdx)

	qual := name
	if fd.Storage&FStatic != 0 {
		qual = df.relPath + ":" + name
	}
	sym := &SymbolRow{FileID: int32(df.srcID), Name: name,
		QualName: jtrunc(qual, 400), Kind: "function", LineStart: n.Line,
		Signature: jtrunc(sig, 400), HasSignature: true,
		ReturnType: jtrunc(ret, 120), HasReturnType: true,
		LineEnd: fd.LineEnd, NLines: fd.LineEnd - n.Line + 1,
		Visibility: "extern"}
	if fd.Storage&FStatic != 0 {
		sym.Visibility = "static"
	}
	sym.IsStatic = b2i(fd.Storage&FStatic != 0)
	sym.IsInline = b2i(fd.Storage&FInline != 0)
	sym.IsPublic = b2i(fd.Storage&FStatic == 0)
	sym.IsEntrypoint = b2i(name == "main" || name == "LLVMFuzzerTestOneInput")
	sym.IsVariadic = b2i(fd.Variadic)
	sym.NParams = int32(fd.NParams)
	sym.IsTest = df.isTestF
	sym.IsGenerated = df.isGenF

	w := df.newWalk()
	w.wantRefs = true
	w.toctouChk, w.toctouOpn = map[string]bool{}, map[string]bool{}
	w.shadow, w.types = map[string]bool{}, map[string]string{}
	w.zeroTested, w.discards = map[string]bool{}, map[string]int32{}
	for pi := uint32(0); pi < fd.NParams; pi++ {
		pd := &fa.Params[fa.Nodes[fd.Param0+pi].A-1]
		if nm := fa.name(pd.NameIdx); nm != "" {
			w.shadow[nm] = true
			w.types[nm] = fa.name(pd.TypeIdx)
		}
	}
	if fd.Body != 0 {
		df.walkStmt(w, fd.Body, 1, 0)
	}
	sym.Cyclomatic = w.cyclomatic + 1
	sym.Cognitive = w.cognitive
	sym.MaxNesting = w.maxNesting
	sym.NLoops = w.nLoops
	sym.NBranches = w.nBranches
	sym.NSwitch = w.nSwitch
	sym.NCases = w.nCases
	sym.NLabels = w.nLabels
	sym.NGotos = w.nGotos
	sym.NReturns = w.nReturns
	sym.NDeref = w.nDeref
	sym.NCast = w.nCast
	sym.NSizeof = w.nSizeof
	sym.NArith = w.nArith
	sym.NCmp = w.nCmp
	sym.NShift = w.nShift
	sym.NTernary = w.nTernary
	sym.NMagic = w.nMagic
	sym.NFloatLit = w.nFloatLit
	sym.NPtrLocals = w.nPtrLocals
	sym.MaxLoopDepth = w.maxLoopDepth
	sym.NStaticAssert = w.nStaticAssert
	sym.NVolatile = w.hasVolatile
	sym.NIntrinsic = w.nIntrinsic
	sym.NAtomic = w.nAtomic
	sym.NLikely = w.nLikely
	sym.NBuiltin = w.nBuiltin
	sym.NGetenv = w.nGetenv
	sym.NErrno = w.nErrno
	sym.NVla = w.nVla
	sym.NOperators = w.nOperators
	sym.NOperands = w.nOperands
	sym.NTokens = w.nOperators + w.nOperands
	sym.NGlobalWrite = w.nGlobalWrite
	sym.NDistinctOperators = w.nDistOps
	sym.NDistinctOperands = w.nDistOper
	sym.NAssign = w.nAssign
	sym.NCompoundAssign = w.nCompoundAssign
	sym.NIncdec = w.nIncdec
	sym.NLogical = w.nLogical
	sym.NAddrof = w.nAddrof
	sym.NArrow = w.nArrow
	sym.NMemberAcc = w.nArrow + w.nDot
	sym.NSubscript = w.nSubscript
	sym.NBitop = w.nBitop
	sym.NStringLit = w.nStringLit
	sym.NNullCheck = w.nNullCheck
	sym.CallInLoop = w.callInLoop
	sym.AllocInLoop = w.allocInLoop
	sym.IOInLoop = w.ioInLoop
	sym.LockInLoop = w.lockInLoop
	sym.BranchInLoop = w.branchInLoop
	r := &sym.SymbolRare
	r.SwitchInLoop = w.switchInLoop
	r.LibmInLoop = w.libmInLoop
	r.DivInLoop = w.divInLoop
	r.StrlenInLoop = w.strlenInLoop
	r.RetNull = w.retNull
	r.RetNeg = w.retNeg
	r.RetZero = w.retZero
	r.RetVal = w.retVal
	r.RetVoid = w.retVoid

	if fd.Body != 0 {
		b0, b1 := df.rawRange(fa.Nodes[fd.Body].Off, fa.Nodes[fd.Body].End)
		if b0 >= 0 && int(b1) <= len(df.srcRaw) && b1 >= b0 {
			sym.Sloc, sym.NCommentLines, _ = classifyLines(df.srcRaw[b0:b1])
		}
	}

	nDyn := int32(0)
	for _, c := range w.calls {
		if c.isCall && c.fnptr {
			nDyn++
		}
	}
	sym.NCalls = int32(len(w.calls))
	sym.NDynamicCalls = nDyn
	sym.NFnptrCalls = nDyn
	sym.NCallsites = sym.NCalls

	// internSym stores a COPY. The slot is reserved here because params, locals
	// and call heuristics need the id, and the finished row is written back at
	// the end of this function -- everything assigned to sym below this line
	// was silently discarded before that write-back existed.
	sid := df.internSym(sym)
	sym.ID = sid
	df.funcSymByNode[df.lastFuncNode] = sid

	np := int32(0)
	sawEllipsis := false
	for pi := uint32(0); pi < fd.NParams; pi++ {
		pn := &fa.Nodes[fd.Param0+pi]
		pd := &fa.Params[pn.A-1]
		pname := fa.name(pd.NameIdx)
		ptype := fa.name(pd.TypeIdx)
		if ptype == "" && pname == "void" {
			continue
		}
		if pname == "..." || pd.Variadic {
			pa := ParamRow{SymbolID: sid, Pos: np, IsVariadic: 1,
				IsMutable: 1, Type: "..."}
			df.params = append(df.params, pa)
			sym.IsVariadic = 1
			sawEllipsis = true
			continue
		}
		pa := ParamRow{SymbolID: sid, Pos: np,
			Type:       jtrunc(ptype, 120),
			IsRef:      b2i(pd.Ptr > 0),
			IsMutable:  b2i(!pd.IsConst),
			IsNullable: b2i(pd.Ptr > 0),
			TypeDepth:  pd.Ptr}
		if pname != "" {
			pa.Name = jtrunc(pname, 80)
			pa.HasName = true
		}
		df.params = append(df.params, pa)
		if pd.Ptr > 0 {
			sym.NPtrParams++
		}
		np++
	}
	sym.NParams = np
	// `...` ends the parameter list in the parser without a node of its own,
	// so the row is made here. n_params stays the count of named slots.
	if fd.Variadic && !sawEllipsis {
		df.params = append(df.params, ParamRow{SymbolID: sid, Pos: np,
			IsVariadic: 1, IsMutable: 1, Type: "..."})
	}

	sym.NLocals = int32(len(w.locals))
	for i := range w.locals {
		w.locals[i].SymbolID = sid
		w.locals[i].Ordinal = int32(i)
		df.locals = append(df.locals, w.locals[i])
	}
	nLit := int32(0)
	for i := range w.lits {
		lk := w.lits[i].Kind
		if lk == "int" || lk == "hex" || lk == "oct" || lk == "bin" {
			if nLit >= 200 {
				continue
			}
			nLit++
		}
		w.lits[i].SymbolID = sid
		w.lits[i].HasSym = true
		w.lits[i].FileID = int32(df.srcID)
		w.lits[i].ID = int32(len(df.lits) + 1)
		df.lits = append(df.lits, w.lits[i])
	}
	for i := range w.addrs {
		w.addrs[i].SymbolID = sid
		w.addrs[i].HasSym = true
		w.addrs[i].FileID = int32(df.srcID)
		w.addrs[i].ID = int32(len(df.addrs) + 1)
		w.addrs[i].Kind = "addr"
		df.addrs = append(df.addrs, w.addrs[i])
	}
	// a function named as a value: `signal(SIGINT, onInt)`, `c->read = rd;`
	for _, a := range w.takeRefs() {
		a.SymbolID = sid
		a.HasSym = true
		a.FileID = int32(df.srcID)
		a.ID = int32(len(df.addrs) + 1)
		a.Kind = "fn" + a.Kind
		df.addrs = append(df.addrs, a)
	}

	extra := df.applyCallHeuristics(sid, w)
	sym.NMemcpy = extra.nMemcpy
	sym.NAlloc = extra.nAlloc
	sym.NAllocsite = extra.nAllocsite
	sym.NFree = extra.nFree
	sym.NLockAcquire = extra.nLockAcq
	sym.NLockRelease = extra.nLockRel
	sym.NEpoll = extra.nEpoll
	sym.NUring = extra.nUring
	sym.NKqueue = extra.nKqueue
	sym.NEventCreate = extra.nEventCreate
	sym.NEventWait = extra.nEventWait
	sym.NMemory = extra.nMemcpy + extra.nAlloc + extra.nFree
	r.NEvTimeoutIndefinite = extra.nTimeoutInf
	r.NEvTimeoutZero = extra.nTimeoutZero
	r.NEvBatchOne = extra.nBatchOne
	r.NKqTimerZeroData = extra.nKqTimerZero

	df.textHeuristics(sym, w, fd, n)

	byName := map[string][]int32{}
	var order []string
	for _, c := range w.calls {
		if !c.isCall || c.fnptr {
			continue
		}
		if byName[c.name] == nil {
			order = append(order, c.name)
		}
		byName[c.name] = append(byName[c.name], c.line)
	}
	for _, nm := range order {
		df.pendNames = append(df.pendNames, nm)
		df.pendLines = append(df.pendLines, byName[nm])
		df.pendSym = append(df.pendSym, sid)
		df.pendFile = append(df.pendFile, df.globalFileID)
		df.pendModule = append(df.pendModule, df.modID)
	}
	df.fnNames = append(df.fnNames, name)
	df.fnSyms = append(df.fnSyms, sid)
	// the GLOBAL file id: srcID is 1 for the root of every translation unit,
	// so comparing it across files made every .c "the same file"
	df.fnFiles = append(df.fnFiles, df.globalFileID)
	df.fnStatic = append(df.fnStatic, fd.Storage&FStatic != 0)
	df.fnModules = append(df.fnModules, df.modID)
	df.symbols[sid-1] = *sym
}

func (df *derivedFile) textHeuristics(sym *SymbolRow, w *bodyWalk, fd *FuncDef, n *Node) {
	if fd.Body == 0 {
		return
	}
	fa := df.ast
	b0, b1 := df.rawRange(fa.Nodes[fd.Body].Off, fa.Nodes[fd.Body].End)
	if b0 < 0 || b1 > int32(len(df.srcRaw)) || b1 < b0 {
		return
	}
	rawBody := df.srcRaw[b0:b1]
	blankBody := rawBody
	if df.blank != nil && b1 <= int32(len(df.blank)) {
		blankBody = df.blank[b0:b1]
	}
	r := &sym.SymbolRare

	if df.nlRaw != nil {
		if doc, nd := leadingComment(df.srcRaw, df.nlRaw, int(n.Line)); doc {
			sym.HasDoc = 1
			df.file.DocLines += int32(nd)
		}
	}
	sym.NBodyBytes = int32(runeLen(rawBody))
	if h0 := df.rawOff(n.Off); h0 >= 0 && h0 <= b0 {
		def := df.srcRaw[h0:b1]
		if df.blank != nil && b1 <= int32(len(df.blank)) {
			def = df.blank[h0:b1]
		}
		if hasStr(def, "restrict") {
			sym.NRestrict = int32(countWord(def, "restrict") +
				countWord(def, "__restrict") + countWord(def, "__restrict__"))
		}
	}

	if hasStr(blankBody, "8_t") || hasStr(blankBody, "16_t") || hasStr(blankBody, "32_t") {
		r.NNarrowCast = int32(countNarrowCasts(blankBody))
	}
	known := map[string]bool{}
	for i := range w.locals {
		known[w.locals[i].Name] = true
	}
	for pi := uint32(0); pi < fd.NParams; pi++ {
		pn := &fa.Nodes[fd.Param0+pi]
		pd := &fa.Params[pn.A-1]
		if nm := fa.name(pd.NameIdx); nm != "" {
			known[nm] = true
		}
	}
	r.NSignCmp = w.nSignCmp
	if hasStr(blankBody, "rand") {
		r.NWeakRandom = int32(countWeakRandom(blankBody))
	}
	if hasStr(blankBody, "<<") || hasStr(blankBody, ">>") {
		r.NShiftVar = int32(countShiftVar(blankBody))
	}
	if hasStr(blankBody, "realloc") {
		r.NReallocSelf = int32(countReallocSelf(blankBody))
	}
	if hasStr(blankBody, "assert") {
		r.NAssertSide = int32(countAssertSide(blankBody))
	}
	if hasStr(blankBody, "free") {
		n := int32(0)
		scanFrees(blankBody, func(fh freeHit) {
			if fh.afterPl < len(blankBody) && derefAfter(blankBody[fh.afterPl:], fh.name) {
				n++
			}
		})
		r.NFreeThenUse = n
	}
	nCC := int32(0)
	for _, cn := range w.castIdents {
		if known[cn] {
			for i := range w.locals {
				if w.locals[i].Name == cn && w.locals[i].IsConst == 1 {
					nCC++
					break
				}
			}
			for pi := uint32(0); pi < fd.NParams; pi++ {
				pn := &fa.Nodes[fd.Param0+pi]
				pd := &fa.Params[pn.A-1]
				if fa.name(pd.NameIdx) == cn && pd.IsConst {
					nCC++
					break
				}
			}
		}
	}
	r.NConstCast = nCC
	r.NToctou = w.toctou()
	if fd.Variadic {
		nf := int32(0)
		for _, c := range w.calls {
			for _, p := range printfFns {
				if c.name == p {
					nf++
					break
				}
			}
		}
		r.NVariadicFmt = nf
	}

	nEv := 0
	for i := range df.evops {
		if df.evops[i].SymbolID == sym.ID {
			nEv++
		}
	}
	if nEv > 0 {
		fl := scanEvFlags(blankBody)
		r.NEtReg, r.NOneshotReg, r.NWriteReady = i32(fl.etReg), i32(fl.oneshotReg), i32(fl.writeReady)
		r.NErrFlag, r.NRearm, r.NDereg = i32(fl.errFlag), i32(fl.rearm), i32(fl.dereg)
		r.NEagain, r.NEintr = i32(fl.eagain), i32(fl.eintr)
		r.NUringRes, r.NUringSqpoll = i32(fl.uringRes), i32(fl.uringSqpoll)
		r.NNonblockSet, r.NEventDestroy = i32(fl.nonblockSet), i32(fl.eventDestroy)
		r.NUringSqe, r.NUringSeen, r.NUringUdata = i32(fl.uringSqe), i32(fl.uringSeen), i32(fl.uringUdata)
		r.NUringLink, r.NUringStreamOps = i32(fl.uringLink), i32(fl.uringStreamOps)
		r.NUringTeardown, r.NKqTimer, r.NKqReceipt = i32(fl.uringTeardown), i32(fl.kqTimer), i32(fl.kqReceipt)
		r.NUringRing, r.NUringBarrier = i32(fl.uringRing), i32(fl.uringBarrier)
	}

	{
		usedNS := map[string]bool{}
		for i := range df.apiuses {
			if df.apiuses[i].SymbolID == sym.ID {
				usedNS[df.apiuses[i].NS] = true
			}
		}
		if len(usedNS) > 0 {
			for _, gn := range guardNames {
				if !nsIntersects(usedNS, guardNS[gn]) {
					continue
				}
				v := int32(countGuard(gn, blankBody))
				switch gn {
				case "ret_neg":
					r.NRetNegCheck = v
				case "domain":
					r.NDomainGuard = v
				case "uchar":
					r.NUcharCast = v
				case "errno_zero":
					r.NErrnoZero = v
				case "endptr":
					r.NEndptr = v
				case "va_end":
					r.NVaEnd = v
				case "map_failed":
					r.NMapFailed = v
				case "monotonic":
					r.NMonotonicClock = v
				case "stacksz_array":
					r.NStackszArray = v
				case "ptr_ovf_check":
					r.NPtrOvfCheck = v
				case "calloc_sizeof_first":
					r.NCallocTransposed = v
				case "va_arg_array":
					r.NVaArgArr = v
				}
			}
		}
	}

	emitHz := func(pat, cat string, n int) {
		if n > 0 {
			df.hazards = append(df.hazards, HazardRow{SymbolID: sym.ID,
				Pattern: pat, Category: cat, N: int32(n)})
		}
	}
	emitHz("ptr_cast", "integer", countPtrCast(blankBody))
	if hasStr(blankBody, "<<") || hasStr(blankBody, ">>") {
		emitHz("shift", "integer", countStr(blankBody, "<<")+countStr(blankBody, ">>"))
	}
	if hasStr(blankBody, "sizeof") {
		emitHz("mul_sizeof", "integer", countMulSizeof(blankBody))
	}
	emitHz("fixed_buffer", "memory", countFixedBuffer(blankBody))
	emitHz("vla", "memory", countVLAPat(blankBody))
	emitHz("signed_cmp", "integer", countSignedCmp(blankBody))

	// Tree-derived facts with no column of their own. They ride in the hazards
	// table under categories nothing sums into a score: `shape` for what the
	// body does, `discard` for each callee whose result this body drops.
	unguarded := 0
	for _, d := range w.divisors {
		if !w.zeroTested[d] {
			unguarded++
		}
	}
	emitHz("frame_bytes", "shape", int(min(w.frameBytes, 1<<30)))
	emitHz("frame_unsized", "shape", int(w.frameUnsized))
	emitHz("frame_max_local", "shape", int(w.frameMax))
	emitHz("div_by_var", "shape", int(w.nDivByVar))
	emitHz("div_unguarded", "shape", unguarded)
	emitHz("loop_forever", "shape", int(w.nForever))
	emitHz("loop_forever_noexit", "shape", int(w.nForeverNoX))
	for _, nm := range w.discardOrd {
		emitHz(jtrunc(nm, 80), "discard", int(w.discards[nm]))
	}
}

type callSiteRec struct {
	name    string
	line    int32
	fnptr   bool
	isCall  bool
	node    uint32
	argsOff int32
	argsEnd int32
	argsTxt string
}

func (w *bodyWalk) noteOp(s string) {
	w.nOperators++
	if !w.opSeen[s] {
		w.opSeen[s] = true
		w.nDistOps++
	}
}

func (w *bodyWalk) noteOper(s string) {
	w.nOperands++
	if !w.operSeen[s] {
		w.operSeen[s] = true
		w.nDistOper++
	}
}

func (df *derivedFile) walkStmt(w *bodyWalk, idx uint32, scopeDepth int32, loopDepth int32) {
	fa := df.ast
	if idx == 0 || int(idx) >= len(fa.Nodes) {
		return
	}

	if w.has(idx, mkVisited) {
		return
	}
	w.set(idx, mkVisited)
	n := &fa.Nodes[idx]
	if scopeDepth > w.maxNesting {
		w.maxNesting = scopeDepth
	}
	if loopDepth > w.maxLoopDepth {
		w.maxLoopDepth = loopDepth
	}
	walkExpr := func(e uint32) { df.walkStmt(w, e, scopeDepth, loopDepth) }

	switch n.Kind {
	case NCompoundStmt:
		a, b := n.A, n.B
		for i := uint32(0); i < b; i++ {
			df.walkStmt(w, a+i, scopeDepth+1, loopDepth)
		}

	case NDeclStmt:
		a, b := n.A, n.B
		for i := uint32(0); i < b; i++ {
			c := &fa.Nodes[a+i]
			if c.Kind != NDecl {
				continue
			}
			d := &fa.Decls[c.A-1]
			typ := fa.name(d.TypeIdx)
			isConst := strings.Contains(typ, "const") || d.Storage&FConstexpr != 0
			lr := LocalRow{Line: c.Line,
				Name:       jtrunc(fa.name(d.NameIdx), 80),
				Type:       jtrunc(squeeze(typ, ""), 120),
				IsConst:    b2i(isConst),
				IsMutable:  b2i(!isConst),
				HasInit:    b2i(d.Init != 0),
				InLoop:     b2i(loopDepth > 0),
				ScopeDepth: b2i8(scopeDepth)}
			w.locals = append(w.locals, lr)
			if w.shadow != nil && d.Storage&FExtern == 0 {
				w.shadow[lr.Name] = true
			}
			if w.types != nil {
				if d.ArrayLen != 0 {
					w.types[lr.Name] = typ + "[]"
				} else {
					w.types[lr.Name] = typ
				}
			}
			if strings.Contains(typ, "volatile") {
				w.hasVolatile++
			}
			if d.Storage&(FStatic|FExtern|FThreadLoc) == 0 {
				f := fld{ftype: typ, ptr: d.Ptr, alen: d.ArrayLen}
				if d.Ptr == 0 && df.typeSz != nil {
					if v, ok := df.typeSz[aggBase(typ)]; ok {
						f.kn, f.ksz, f.kal, f.kex = true, v[0], v[1], v[2]
					}
				}
				if sz, _, ex := f.size(); ex == 1 && sz > 0 {
					w.frameBytes += int64(sz)
					w.frameMax = max(w.frameMax, sz)
				} else {
					w.frameUnsized++
				}
			}
			if d.Ptr > 0 {
				w.nPtrLocals++
			}
			if d.ArrayLen == -1 {
				w.nVla++
			}
			if d.Init != 0 {
				df.walkStmt(w, d.Init, scopeDepth, loopDepth)
			}
		}

	case NExprStmt:
		if n.A != 0 {
			// a call whose whole statement is the call: its result is dropped
			if c := &fa.Nodes[n.A]; c.Kind == NCall && w.discards != nil {
				if cal := &fa.Nodes[c.A]; cal.Kind == NIdent {
					nm := fa.name(cal.A)
					if df.tuFunc(nm) {
						if w.discards[nm] == 0 {
							w.discardOrd = append(w.discardOrd, nm)
						}
						w.discards[nm]++
					}
				}
			}
			walkExpr(n.A)
		}

	case NIf:
		w.cyclomatic++
		w.nBranches++
		if loopDepth > 0 {
			w.branchInLoop++
		}
		if scopeDepth-1 > 1 {
			w.cognitive += scopeDepth - 1
		} else {
			w.cognitive++
		}
		w.noteOper("if")
		df.noteTested(w, n.A, 0)
		walkExpr(n.A)
		df.walkStmt(w, n.B, scopeDepth, loopDepth)
		if n.C != 0 {
			df.walkStmt(w, n.C, scopeDepth, loopDepth)
		}

	case NSwitch:
		w.nSwitch++
		w.nBranches++
		if loopDepth > 0 {
			w.switchInLoop++
		}
		if scopeDepth-1 > 1 {
			w.cognitive += scopeDepth - 1
		} else {
			w.cognitive++
		}
		w.noteOper("switch")
		walkExpr(n.A)
		df.walkStmt(w, n.B, scopeDepth, loopDepth)

	case NCase:
		if n.A != 0 {
			w.cyclomatic++
			w.nBranches++
			w.nCases++
			if loopDepth > 0 {
				w.branchInLoop++
			}
			w.noteOper("case")
			walkExpr(n.A)
		}
		df.walkStmt(w, n.B, scopeDepth, loopDepth)

	case NWhile:
		w.nLoops++
		w.cyclomatic++
		if scopeDepth-1 > 1 {
			w.cognitive += scopeDepth - 1
		} else {
			w.cognitive++
		}
		w.noteOper("while")
		df.noteTested(w, n.A, 0)
		df.noteLoop(w, n.A, n.B, false)
		walkExpr(n.A)
		df.walkStmt(w, n.B, scopeDepth, loopDepth+1)

	case NDo:
		w.nLoops++
		w.cyclomatic++
		if scopeDepth-1 > 1 {
			w.cognitive += scopeDepth - 1
		} else {
			w.cognitive++
		}
		w.noteOper("do")
		df.noteTested(w, n.B, 0)
		df.noteLoop(w, n.B, n.A, false)
		df.walkStmt(w, n.A, scopeDepth, loopDepth+1)
		walkExpr(n.B)

	case NFor:
		w.nLoops++
		w.cyclomatic++
		if scopeDepth-1 > 1 {
			w.cognitive += scopeDepth - 1
		} else {
			w.cognitive++
		}
		w.noteOper("for")
		df.noteTested(w, n.B, 0)
		df.noteLoop(w, n.B, n.D, n.B == 0)
		if n.A != 0 {
			df.walkStmt(w, n.A, scopeDepth, loopDepth)
		}
		if n.B != 0 {
			walkExpr(n.B)
		}
		if n.C != 0 {
			walkExpr(n.C)
		}
		df.walkStmt(w, n.D, scopeDepth, loopDepth+1)

	case NReturn:
		w.nReturns++
		w.noteOper("return")
		if n.A == 0 {
			w.retVoid++
		} else {
			switch df.retShape(n.A) {
			case 1:
				w.retNull++
			case 2:
				w.retNeg++
			case 3:
				w.retZero++
			default:
				w.retVal++
			}
		}
		if n.A != 0 {
			walkExpr(n.A)
		}

	case NGoto:
		w.nGotos++
		w.noteOper("goto")

	case NLabel:
		w.nLabels++
		if n.B != 0 {
			df.walkStmt(w, n.B, scopeDepth, loopDepth)
		}

	case NStaticAssert:
		w.nStaticAssert++

	case NAsm, NBreak, NContinue:

	case NBinary:
		op := punctTable[n.Flags&0xFF]
		w.noteOp(op)
		if n.Flags == uint16(PSlash) && loopDepth > 0 {
			w.divInLoop++
		}
		if n.Flags == uint16(PSlash) || n.Flags == uint16(PMod) {
			df.noteDivisor(w, n.B)
		}
		switch n.Flags {
		case uint16(PAmp), uint16(PPipe), uint16(PCaret), uint16(PLsh), uint16(PRsh):
			w.nBitop++
		}
		switch n.Flags {
		case uint16(PAmpAmp), uint16(PPipePip):
			w.cyclomatic++
			w.nLogical++
		case uint16(PPlus), uint16(PMinus), uint16(PStar), uint16(PSlash), uint16(PMod):
			w.nArith++
		case uint16(PEqEq), uint16(PNotEq), uint16(PLt), uint16(PGt), uint16(PLe), uint16(PGe):
			w.nCmp++
		case uint16(PLsh), uint16(PRsh):
			w.nShift++
		}

		if n.Flags == uint16(PEqEq) || n.Flags == uint16(PNotEq) {
			if df.isNullLit(n.A) || df.isNullLit(n.B) {
				w.nNullCheck++
			}
		}
		switch n.Flags {
		case uint16(PLt), uint16(PGt), uint16(PLe), uint16(PGe):
			if (df.signedOperand(w, n.A) && df.sizeOperand(w, n.B, 0)) ||
				(df.sizeOperand(w, n.A, 0) && df.signedOperand(w, n.B)) {
				w.nSignCmp++
			}
		}
		walkExpr(n.A)
		walkExpr(n.B)

	case NAssign:
		op := punctTable[n.Flags&0xFF]
		w.noteOp(op)
		if n.Flags == uint16(PEq) {
			w.nAssign++
		} else {
			w.nCompoundAssign++
		}

		switch n.Flags {
		case uint16(PAmpEq), uint16(PPipeEq), uint16(PCaretEq), uint16(PLshEq), uint16(PRshEq):
			w.nBitop++
		}
		if n.Flags == uint16(PSlashEq) && loopDepth > 0 {
			w.divInLoop++
		}
		if n.Flags == uint16(PSlashEq) || n.Flags == uint16(PModEq) {
			df.noteDivisor(w, n.B)
		}
		if df.writesGlobal(w, n.A) {
			w.nGlobalWrite++
		}
		walkExpr(n.A)
		walkExpr(n.B)

	case NCond:
		df.noteTested(w, n.A, 0)
		w.nTernary++
		w.cyclomatic++
		w.noteOp("?")
		walkExpr(n.A)
		walkExpr(n.B)
		walkExpr(n.C)

	case NUnary:
		switch n.Flags {
		case uint16(PStar):
			w.nDeref++
			w.noteOp("*")
		case uint16(PInc), uint16(PDec):
			w.nIncdec++
			w.noteOp(punctTable[n.Flags])
			if df.writesGlobal(w, n.A) {
				w.nGlobalWrite++
			}
		case uint16(PAmp):
			w.noteOp("&")
			if o := fa.Nodes[n.A]; o.Kind == NIdent {
				w.nAddrof++
				w.set(n.A, mkAmp)
				na := fa.name(o.A)
				if na != "" && !df.isFunctionName(na) {
					w.addrs = append(w.addrs, AddrTakenRow{Name: jtrunc(na, 80),
						Line: int32(fa.Nodes[n.A].Line)})
				}
			}
		case uint16(PTilde):
			w.nBitop++
		default:
			w.noteOp(punctTable[n.Flags&0xFF])
		}
		if n.A != 0 {
			walkExpr(n.A)
		}

	case NPostfix:
		w.nIncdec++
		w.noteOp(punctTable[n.Flags&0xFF])
		if df.writesGlobal(w, n.A) {
			w.nGlobalWrite++
		}
		walkExpr(n.A)

	case NCall:
		callee := fa.Nodes[n.A]
		name := ""
		fnptr := false
		if callee.Kind == NIdent {
			name = fa.name(callee.A)
			w.set(n.A, mkCallee)
		} else {
			fnptr = true
		}
		argsOff, argsEnd := int32(0), int32(0)
		argsTxt := ""
		if n.B > 0 {
			first := &fa.Nodes[n.C]
			last := &fa.Nodes[n.C+n.B-1]
			argsOff, argsEnd = first.Off, last.End
			if first.Off <= callee.Off {
				// Every token of a macro expansion carries the INVOCATION's
				// offsets, so this call's argument span is the macro's name.
				// Render the arguments from the tree instead.
				var sb strings.Builder
				for i := uint32(0); i < n.B; i++ {
					if i > 0 {
						sb.WriteString(", ")
					}
					df.exprText(&sb, n.C+i, 0)
				}
				argsTxt = sb.String()
			} else if e := n.End - 1; e > argsEnd {
				// written out in this file: the closing paren bounds the list
				// even when the last argument is itself a macro invocation
				if r := df.rawOff(e); r >= 0 && int(r) < len(df.srcRaw) && df.srcRaw[r] == ')' {
					argsEnd = e
				}
			}
		}
		w.calls = append(w.calls, callSiteRec{name: name, fnptr: fnptr,
			isCall: true, node: idx, argsOff: argsOff, argsEnd: argsEnd,
			argsTxt: argsTxt, line: int32(n.Line)})
		if loopDepth > 0 {
			w.callInLoop++
			if allocatesName(name) {
				w.allocInLoop++
			}
			switch name {
			case "sqrt", "exp", "log", "log2", "log10", "pow", "sin", "cos",
				"tan", "atan", "atan2", "tgamma", "lgamma", "erf", "fmod",
				"cbrt", "hypot", "floor", "ceil", "round":
				w.libmInLoop++
			case "strlen", "strnlen":
				w.strlenInLoop++
			}
			switch hazardFuncs[name] {
			case "io":
				w.ioInLoop++
			case "concurrency":
				w.lockInLoop++
			}
		}
		if n.B > 0 {
			arg0 := &fa.Nodes[n.C]
			if arg0.Kind == NIdent {
				an := fa.name(arg0.A)
				switch name {
				case "access", "faccessat", "stat", "lstat":
					w.toctouChk[an] = true
				case "open", "openat", "fopen", "creat":
					w.toctouOpn[an] = true
				}
			}
		}
		if name == "getenv" || name == "secure_getenv" {
			w.nGetenv++
		}
		if isIntrinsicCall(name) {
			w.nIntrinsic++
		}
		if strings.HasPrefix(name, "__atomic_") || strings.HasPrefix(name, "__sync_") ||
			strings.HasPrefix(name, "__c11_atomic_") || strings.HasPrefix(name, "atomic_") {
			w.nAtomic++
		}
		if name == "__builtin_expect" || name == "__builtin_expect_with_probability" ||
			name == "likely" || name == "unlikely" {
			w.nLikely++
		}
		if strings.HasPrefix(name, "__builtin_") {
			w.nBuiltin++
		}
		walkExpr(n.A)
		for i := uint32(0); i < n.B; i++ {
			walkExpr(n.C + i)
		}

	case NMember:
		if n.Flags&1 != 0 {
			w.nDeref++
			w.nArrow++
		} else {
			w.nDot++
		}
		w.noteOp(".")
		walkExpr(n.B)

	case NIndex:
		w.nSubscript++
		w.noteOp("[")
		walkExpr(n.A)
		walkExpr(n.B)

	case NCast:
		w.nCast++
		w.noteOp("cast")
		if n.B != 0 && fa.Nodes[n.B].Kind == NIdent {
			w.castIdents = append(w.castIdents, fa.name(fa.Nodes[n.B].A))
		}
		walkExpr(n.B)

	case NSizeofT, NAlignofT:
		w.nSizeof++
		w.noteOper("sizeof")

	case NSizeofE:
		w.nSizeof++
		w.noteOper("sizeof")
		walkExpr(n.A)

	case NVaArg:
		w.noteOper("__builtin_va_arg")
		walkExpr(n.A)

	case NIdent:
		nm := fa.name(n.A)
		if nm == "errno" {
			w.nErrno++
		}
		w.noteOper(nm)
		if w.wantRefs && (w.fileScope || (!w.shadow[nm] && df.tuFunc(nm))) {
			w.refs = append(w.refs, identRef{node: idx, name: nm, line: n.Line})
		}

	case NNumLit:
		tok := fa.name(n.A)
		w.noteOper(tok)
		kind := numKind(tok)
		magic := !magicFor(tok)
		if kind == "float" {
			w.nFloatLit++
			magic = true
		}
		if magic {
			w.nMagic++
		}
		w.lits = append(w.lits, LiteralRow{Kind: kind, Value: jtrunc(tok, 40),
			Line: int32(n.Line), IsMagic: b2i(magic)})

	case NCharLit:
		tok := fa.name(n.A)
		w.noteOper(tok)
		w.lits = append(w.lits, LiteralRow{Kind: "char", Value: jtrunc(tok, 40),
			Line: int32(n.Line)})

	case NStrLit:
		tok := fa.name(n.A)
		w.nStringLit++
		w.noteOper(tok)
		w.lits = append(w.lits, LiteralRow{Kind: "string",
			Value: jtrunc(dequoteLit(tok), 200), Line: int32(n.Line)})
		if secretShaped(dequoteLit(tok)) {
			w.lits = append(w.lits, LiteralRow{Kind: "_secret",
				Value: jtrunc(dequoteLit(tok), 200), Line: int32(n.Line)})
		}

	case NStmtExpr:
		if n.A != 0 {
			df.walkStmt(w, n.A, scopeDepth, loopDepth)
		}

	case NParen:
		w.noteOp("(")
		if n.A != 0 {
			walkExpr(n.A)
		}

	case NComma:
		w.noteOp(",")
		walkExpr(n.A)
		walkExpr(n.B)

	case NCompoundLit:
		walkExpr(n.B)

	case NGeneric:
		w.noteOper("_Generic")
		walkExpr(n.A)
		for i := uint32(0); i < n.C; i++ {
			asc := &fa.Assocs[n.B+i]
			if asc.Expr != 0 {
				walkExpr(asc.Expr)
			}
		}

	case NInitList:
		a, b := n.A, n.B
		for i := uint32(0); i < b; i++ {
			it := &fa.Nodes[a+i]
			if it.B != 0 {
				walkExpr(it.B)
			}
		}

	case NBad:

	}
}

func b2i8(v int32) int32 { return v }

// exprText renders an expression from the tree. It exists for the one case a
// source span cannot serve: text that came out of a macro expansion.
func (df *derivedFile) exprText(sb *strings.Builder, idx uint32, depth int) {
	fa := df.ast
	if idx == 0 || int(idx) >= len(fa.Nodes) || depth > 24 || sb.Len() > 400 {
		return
	}
	n := &fa.Nodes[idx]
	sub := func(i uint32) { df.exprText(sb, i, depth+1) }
	typ := func(i uint32) {
		if i == 0 || int(i) > len(fa.Types) {
			return
		}
		td := &fa.Types[i-1]
		sb.WriteString(squeeze(fa.name(td.TextIdx), ""))
	}
	switch n.Kind {
	case NIdent, NNumLit, NCharLit, NStrLit, NBoolLit:
		sb.WriteString(fa.name(n.A))
	case NNullPtr:
		sb.WriteString("nullptr")
	case NParen:
		sb.WriteByte('(')
		sub(n.A)
		sb.WriteByte(')')
	case NBinary, NAssign:
		sub(n.A)
		sb.WriteByte(' ')
		sb.WriteString(punctTable[n.Flags&0xFF])
		sb.WriteByte(' ')
		sub(n.B)
	case NComma:
		sub(n.A)
		sb.WriteString(", ")
		sub(n.B)
	case NUnary:
		sb.WriteString(punctTable[n.Flags&0xFF])
		sub(n.A)
	case NPostfix:
		sub(n.A)
		sb.WriteString(punctTable[n.Flags&0xFF])
	case NCond:
		sub(n.A)
		sb.WriteString(" ? ")
		sub(n.B)
		sb.WriteString(" : ")
		sub(n.C)
	case NMember:
		sub(n.B)
		if n.Flags&1 != 0 {
			sb.WriteString("->")
		} else {
			sb.WriteByte('.')
		}
		sb.WriteString(fa.name(n.A))
	case NIndex:
		sub(n.A)
		sb.WriteByte('[')
		sub(n.B)
		sb.WriteByte(']')
	case NCall:
		sub(n.A)
		sb.WriteByte('(')
		for i := uint32(0); i < n.B; i++ {
			if i > 0 {
				sb.WriteString(", ")
			}
			sub(n.C + i)
		}
		sb.WriteByte(')')
	case NCast:
		sb.WriteByte('(')
		typ(n.A)
		sb.WriteByte(')')
		sub(n.B)
	case NSizeofT:
		sb.WriteString("sizeof(")
		typ(n.A)
		sb.WriteByte(')')
	case NAlignofT:
		sb.WriteString("alignof(")
		typ(n.A)
		sb.WriteByte(')')
	case NSizeofE:
		sb.WriteString("sizeof ")
		sub(n.A)
	case NCompoundLit:
		sb.WriteByte('(')
		typ(n.A)
		sb.WriteString("){...}")
	default:
		sb.WriteString("...")
	}
}

func (df *derivedFile) isNullLit(idx uint32) bool {
	if idx == 0 {
		return false
	}
	nd := df.ast.Nodes[idx]
	if nd.Kind == NIdent {
		nm := df.ast.name(nd.A)
		return nm == "NULL" || nm == "nullptr"
	}
	if nd.Kind == NParen {
		return df.isNullLit(nd.A)
	}
	return false
}

// signedIntType reports whether a declared type is a signed integer OBJECT:
// no pointer, no array, not unsigned, not plain char.
func signedIntType(t string) bool {
	if t == "" || strings.ContainsAny(t, "*[(") {
		return false
	}
	seen := false
	for _, f := range strings.Fields(t) {
		switch f {
		case "const", "volatile", "register", "static", "auto", "signed":
			if f == "signed" {
				seen = true
			}
		case "int", "long", "short", "ssize_t", "ptrdiff_t", "off_t", "intptr_t",
			"intmax_t", "int8_t", "int16_t", "int32_t", "int64_t", "__int64":
			seen = true
		default:
			return false
		}
	}
	return seen
}

// unsignedSizeType reports whether a declared type is an unsigned integer
// object of at least int rank -- the types a signed operand is converted TO.
func unsignedSizeType(t string) bool {
	if t == "" || strings.ContainsAny(t, "*[(") {
		return false
	}
	wide := false
	for _, f := range strings.Fields(t) {
		switch f {
		case "const", "volatile", "register", "static", "auto", "int", "long":
		case "unsigned", "size_t", "uintptr_t", "uintmax_t", "uint32_t",
			"uint64_t", "u_int", "u_long", "uint", "ulong", "rsize_t":
			wide = true
		default:
			// unsigned char/short promote to int: the compare stays signed
			return false
		}
	}
	return wide
}

func (df *derivedFile) signedOperand(w *bodyWalk, idx uint32) bool {
	if idx == 0 || w.types == nil {
		return false
	}
	nd := &df.ast.Nodes[idx]
	switch nd.Kind {
	case NParen:
		return df.signedOperand(w, nd.A)
	case NIdent:
		return signedIntType(w.types[df.ast.name(nd.A)])
	}
	return false
}

// sizeOperand: sizeof, strlen and friends, a name declared with an unsigned
// size type, or arithmetic on one of those.
func (df *derivedFile) sizeOperand(w *bodyWalk, idx uint32, depth int) bool {
	if idx == 0 || depth > 6 || w.types == nil {
		return false
	}
	fa := df.ast
	nd := &fa.Nodes[idx]
	switch nd.Kind {
	case NSizeofT, NSizeofE:
		return true
	case NParen:
		return df.sizeOperand(w, nd.A, depth+1)
	case NIdent:
		return unsignedSizeType(w.types[fa.name(nd.A)])
	case NCall:
		if c := &fa.Nodes[nd.A]; c.Kind == NIdent {
			switch fa.name(c.A) {
			case "strlen", "strnlen", "wcslen", "strspn", "strcspn", "fread", "fwrite":
				return true
			}
		}
	case NBinary:
		switch nd.Flags {
		case uint16(PPlus), uint16(PMinus), uint16(PStar), uint16(PSlash):
			return df.sizeOperand(w, nd.A, depth+1) || df.sizeOperand(w, nd.B, depth+1)
		}
	}
	return false
}

func (df *derivedFile) isGlobalName(name string) bool {
	return df.globalSet()[name]
}

// writesGlobal reports whether the lvalue at idx is rooted at a file-scope
// name: `g = v`, `g.f = v`, `g[i] = v`, `g->f = v`. A write through `*p` is
// not counted -- that is the pointee, which may or may not be shared.
func (df *derivedFile) writesGlobal(w *bodyWalk, idx uint32) bool {
	fa := df.ast
	for hop := 0; idx != 0 && hop < 32; hop++ {
		nd := &fa.Nodes[idx]
		switch nd.Kind {
		case NIdent:
			nm := fa.name(nd.A)
			return nm != "" && !w.shadow[nm] && df.isGlobalName(nm)
		case NMember:
			idx = nd.B
		case NIndex, NParen:
			idx = nd.A
		default:
			return false
		}
	}
	return false
}

func (df *derivedFile) retShape(idx uint32) int {
	nd := df.ast.Nodes[idx]
	switch nd.Kind {
	case NIdent:
		nm := df.ast.name(nd.A)
		if nm == "NULL" || nm == "nullptr" {
			return 1
		}
	case NNullPtr:
		return 1
	case NParen:
		if nd.A != 0 {
			return df.retShape(nd.A)
		}
	case NUnary:
		if nd.Flags == uint16(PMinus) && nd.A != 0 {
			c := df.ast.Nodes[nd.A]
			if c.Kind == NNumLit {
				tx := df.ast.name(c.A)
				if len(tx) > 0 {
					digits := true
					for i := 0; i < len(tx); i++ {
						if tx[i] < '0' || tx[i] > '9' {
							digits = false
							break
						}
					}
					if digits {
						return 2
					}
				}
			}
		}
	case NNumLit:
		if df.ast.name(nd.A) == "0" {
			return 3
		}
	case NBoolLit:
		return 0
	}
	return 0
}

// tuFunc reports whether name is a function this translation unit defines or
// declares anywhere, headers included.
func (df *derivedFile) tuFunc(name string) bool {
	if df.tuFuncs == nil {
		// one set per translation unit, shared by every file derived from it
		if df.tu != nil && df.tu.funcSet != nil {
			df.tuFuncs = df.tu.funcSet
			return df.tuFuncs[name]
		}
		fa := df.ast
		m := make(map[string]bool, len(fa.Funcs)*2+16)
		for i := range fa.Funcs {
			m[fa.name(fa.Funcs[i].NameIdx)] = true
		}
		for i := range fa.Decls {
			if d := &fa.Decls[i]; d.Kind == 1 && d.NameIdx != 0 {
				m[fa.name(d.NameIdx)] = true
			}
		}
		df.tuFuncs = m
		if df.tu != nil {
			df.tu.funcSet = m
		}
	}
	return df.tuFuncs[name]
}

func (df *derivedFile) isFunctionName(name string) bool {
	for i := range df.fnNames {
		if df.fnNames[i] == name {
			return true
		}
	}
	return false
}

func dequoteLit(s string) string {
	s = strings.TrimPrefix(s, "u8")
	s = strings.TrimPrefix(s, "u")
	s = strings.TrimPrefix(s, "U")
	s = strings.TrimPrefix(s, "L")
	if len(s) >= 2 && s[0] == '"' {
		s = s[1 : len(s)-1]
	}
	return s
}

func splitTop(s string) []string {
	var out []string
	depth := 0
	start := 0
	for i := 0; i < len(s); i++ {
		switch s[i] {
		case '(', '[':
			depth++
		case ')', ']':
			depth--
		case ',':
			if depth == 0 {
				out = append(out, strings.TrimSpace(s[start:i]))
				start = i + 1
			}
		}
	}
	if strings.TrimSpace(s[start:]) != "" || len(out) > 0 {
		out = append(out, strings.TrimSpace(s[start:]))
	}
	return out
}

func (df *derivedFile) applyCallHeuristics(sid int32, w *bodyWalk) (extra symExtra) {
	type hazKey struct {
		pat, cat string
	}
	hazN := map[hazKey]int32{}
	hazFirst := map[hazKey]int32{}
	apiRows := 0
	for _, c := range w.calls {
		if !c.isCall {
			continue
		}
		argText := c.argsTxt
		if argText == "" {
			argText = df.rawSpan(c.argsOff, c.argsEnd, 200)
		} else {
			argText = jtrunc(argText, 200)
		}
		args := splitTop(argText)
		name := c.name

		if memopFnSet[name] {
			dst, src, sarg := memopArgs(name, args)
			sizeBuf := ""
			if op, ok := sizeofOperand(sarg); ok {
				if nm, ok2 := firstIdent(op); ok2 {
					sizeBuf = nm
				}
			} else if nm, ok2 := firstIdent(sarg); ok2 && sarg != "" && sarg[0] != '-' &&
				!(len(sarg) > 0 && sarg[0] >= '0' && sarg[0] <= '9') {
				sizeBuf = nm
			}
			dt := lastPathTail(dst)
			if dt == "" {
				dt = dst
			}
			df.memops = append(df.memops, MemopRow{ID: int32(len(df.memops) + 1),
				SymbolID: sid, FileID: int32(df.srcID), Fn: name,
				Dst: jtrunc(dst, 120), Src: jtrunc(src, 120),
				SizeArg: jtrunc(sarg, 120), SizeBuf: jtrunc(sizeBuf, 80),
				DstTail: jtrunc(dt, 80), Line: c.line})
			extra.nMemcpy++
		}

		if isAllocName(name) {
			if libcAllocNames[name] {
				extra.nAlloc++
			}
			// A release is not an allocation site. Recording `zfree(p)` here
			// made 7 in 10 "allocation sites" frees, each with no sizeof.
			if !c.fnptr && !isFreeName(name) {
				sz := jtrunc(squeeze(argText, ""), 150)
				df.allocs = append(df.allocs, AllocSiteRow{ID: int32(len(df.allocs) + 1),
					SymbolID: sid, FileID: int32(df.srcID), Fn: name,
					SizeExpr: sz, Line: c.line})
				extra.nAllocsite++
				if !strings.Contains(sz, "sizeof") {
					extra.nAllocNoSizeof++
				}
			}
		}
		if name == "free" && !c.fnptr {
			extra.nFree++
		}

		if _, acq, rel := lockShape(name); acq || rel {
			if (name == "flock" || name == "lockf") &&
				(strings.Contains(argText, "LOCK_UN") || strings.Contains(argText, "F_ULOCK")) {
				acq, rel = false, true
			}
			if acq {
				extra.nLockAcq++
			}
			if rel {
				extra.nLockRel++
			}
			// The lock is the FIRST argument in every API this matches
			// (pthread_mutex_timedlock(&m, &ts), spin_lock_irqsave(&l, f));
			// taking the last recorded the flags, the timeout or NULL.
			if len(args) > 0 {
				// the whole lvalue, not its first identifier: `&q->lock` and
				// `&q->stats_lock` are two locks, and both began with `q`
				if nm := lockArgName(args[0]); nm != "" {
					df.locks = append(df.locks, LockRowR{ID: int32(len(df.locks) + 1),
						SymbolID: sid, FileID: int32(df.srcID), Name: jtrunc(nm, 80),
						Line: c.line})
				}
			}
		}

		if isEventOp(name) {
			a := jtrunc(squeeze(argText, ""), 180)
			df.evops = append(df.evops, EventOpRow{ID: int32(len(df.evops) + 1),
				SymbolID: sid, FileID: int32(df.srcID), Family: eventFamily(name),
				Fn: name, Args: a, Line: c.line})
			switch {
			case strings.HasPrefix(name, "epoll_"):
				extra.nEpoll++
			case strings.HasPrefix(name, "io_uring_"):
				extra.nUring++
			default:
				extra.nKqueue++
			}
			if eventCreateFn[name] {
				extra.nEventCreate++
			}
			if eventWaitFn[name] {
				extra.nEventWait++
			}
			ti, hasT := evTimeoutIdx[name]
			if hasT {
				parts := splitTop(argText)
				bi := evBatchIdx[name]
				if len(parts) > bi && parts[bi] == "1" {
					extra.nBatchOne++
				}
				if len(parts) > ti && len(parts) > bi && parts[bi] != "0" {
					tmo := parts[ti]
					if tmo == "NULL" || tmo == "-1" {
						extra.nTimeoutInf++
					} else if tmo == "0" {
						extra.nTimeoutZero++
					}
				}
			} else if name == "EV_SET" {
				parts := splitTop(argText)
				if len(parts) > 5 && strings.Contains(parts[2], "EVFILT_TIMER") &&
					parts[5] == "0" {
					extra.nKqTimerZero++
				}
			}
		}

		if ns := funcToNS[name]; ns != "" && apiRows < 12 {
			df.apiuses = append(df.apiuses, APIUseRow{ID: int32(len(df.apiuses) + 1),
				SymbolID: sid, FileID: int32(df.srcID), NS: ns, Fn: name, Line: c.line})
			apiRows++
		}

		if cat := hazardFuncs[name]; cat != "" {
			k := hazKey{name, cat}
			if hazN[k] == 0 {
				hazFirst[k] = c.line
			}
			hazN[k]++
		}
	}

	for _, c := range w.calls {
		if !c.isCall {
			continue
		}
		k := hazKey{c.name, hazardFuncs[c.name]}
		if k.cat == "" || hazN[k] == 0 {
			continue
		}
		df.hazards = append(df.hazards, HazardRow{SymbolID: sid,
			Pattern: jtrunc(k.pat, 80), Category: k.cat, N: hazN[k],
			FirstLine: hazFirst[k]})
		hazN[k] = 0
	}

	for _, l := range w.lits {
		if l.Kind == "_secret" {
			df.secrets = append(df.secrets, SecretCandidateRow{ID: int32(len(df.secrets) + 1),
				SymbolID: sid, HasSym: true, FileID: int32(df.srcID),
				Value: l.Value, Line: l.Line})
		}
	}
	return
}

type symExtra struct {
	nMemcpy, nAlloc, nAllocsite, nAllocNoSizeof int32
	nFree, nLockAcq, nLockRel                   int32
	nEpoll, nUring, nKqueue                     int32
	nEventCreate, nEventWait                    int32
	nTimeoutInf, nTimeoutZero                   int32
	nBatchOne, nKqTimerZero                     int32
}

func (w *bodyWalk) toctou() int32 {
	n := int32(0)
	for k := range w.toctouChk {
		if w.toctouOpn[k] {
			n++
		}
	}
	return n
}

var memopFnSet = map[string]bool{"memcpy": true, "memmove": true, "strcpy": true,
	"strncpy": true, "strcat": true, "strncat": true, "memset": true,
	"sprintf": true, "snprintf": true}

func memopArgs(fn string, args []string) (dst, src, sizeArg string) {
	switch fn {
	case "memset":
		if len(args) > 0 {
			dst = args[0]
		}
		if len(args) > 2 {
			sizeArg = args[2]
		}
	case "snprintf", "sprintf":
		if len(args) > 0 {
			dst = args[0]
		}
		if len(args) > 1 {
			sizeArg = args[1]
		}
		if len(args) > 2 {
			src = args[2]
		}
	default:
		if len(args) > 0 {
			dst = args[0]
		}
		if len(args) > 1 {
			src = args[1]
		}
		if len(args) > 2 {
			sizeArg = args[2]
		}
	}
	return
}

func sizeofOperand(s string) (string, bool) {
	s = strings.TrimSpace(s)
	if !strings.HasPrefix(s, "sizeof") {
		return "", false
	}
	rest := strings.TrimSpace(s[6:])
	if strings.HasPrefix(rest, "(") {
		inner := strings.TrimSuffix(strings.TrimPrefix(rest, "("), ")")
		inner = strings.TrimSpace(inner)
		if strings.Contains(inner, "struct ") || strings.Contains(inner, "union ") ||
			strings.Contains(inner, "enum ") {
			return "", false
		}
		return inner, true
	}
	if rest == "" {
		return "", false
	}
	return rest, true
}

func lockArgName(arg string) string {
	a := strings.TrimSpace(arg)
	for len(a) > 0 && (a[0] == '&' || a[0] == '(' || a[0] == ' ') {
		a = a[1:]
	}
	for len(a) > 0 && (a[len(a)-1] == ')' || a[len(a)-1] == ' ') && strings.Count(a, "(") < strings.Count(a, ")") {
		a = a[:len(a)-1]
	}
	if _, ok := firstIdent(a); !ok {
		return ""
	}
	return strings.ReplaceAll(a, " ", "")
}

func isEventOp(fn string) bool {
	if strings.HasPrefix(fn, "epoll_") || strings.HasPrefix(fn, "io_uring_") {
		return true
	}
	return eventWaitFn[fn] || eventCreateFn[fn] || fn == "kqueue" || fn == "EV_SET"
}

func isFreeName(name string) bool {
	if name == "free" {
		return true
	}
	return len(name) > 4 && strings.EqualFold(name[len(name)-4:], "free")
}

// allocatesName is isAllocName without the release half: the libc allocators
// and anything a project named after one (zmalloc, xcalloc, sdsdup ...).
func allocatesName(name string) bool {
	return isAllocName(name) && !isFreeName(name)
}

func isAllocName(name string) bool {
	if libcAllocNames[name] {
		return true
	}
	low := strings.ToLower(name)
	for _, suf := range [...]string{"alloc", "free", "strdup", "memdup"} {
		if len(low) > len(suf) && strings.HasSuffix(low, suf) {
			return true
		}
	}
	return false
}

func isIntrinsicCall(name string) bool {
	for _, p := range builtinPrefixes {
		if strings.HasPrefix(name, p) {
			return true
		}
	}
	return false
}

func secretShaped(s string) bool {
	if len(s) < 12 || strings.Contains(s, " ") {
		return false
	}
	low := strings.ToLower(s)
	for _, k := range [...]string{"passw", "secret", "api_key", "apikey",
		"token", "credential", "private_key", "auth_key", "access_key"} {
		if strings.Contains(low, k) {
			return true
		}
	}
	return false
}

var markerKinds = [...]string{"TODO", "FIXME", "XXX", "HACK", "BUG", "NOTE",
	"WARNING", "OPTIMIZE", "REVIEW", "DEPRECATED", "SAFETY", "PANIC", "UNSAFE"}

func scanMarkersRaw(src []byte, fn func(kind string, line int32, text string)) {
	ln := int32(1)
	start := 0
	for i := 0; i <= len(src); i++ {
		if i == len(src) || src[i] == '\n' {
			line := src[start:i]
			scanMarkerLine(line, ln, fn)
			ln++
			start = i + 1
		}
	}
}

func scanMarkerLine(line []byte, n int32, fn func(kind string, line int32, text string)) {

	sigil := false
	for i := 0; i+1 < len(line); i++ {
		if line[i] == '/' && line[i+1] == '/' {
			sigil = true
			break
		}
		if line[i] == '-' && line[i+1] == '-' {
			sigil = true
			break
		}
	}
	for _, c := range line {
		if c == '#' || c == '*' {
			sigil = true
			break
		}
	}
	if !sigil {
		return
	}
	textDone := false
	for p := 0; p < len(line); {
		if !c23IsIdentStart(line[p]) {
			p++
			continue
		}

		if p > 0 && c23IsIdentPart(line[p-1]) {
			p = identEndAt(line, p)
			continue
		}
		e := identEndAt(line, p)
		w := string(line[p:e])
		hit := ""
		for _, k := range markerKinds {
			if w == k {
				hit = k
				break
			}
		}
		if hit != "" {

			if e < len(line) && c23IsIdentPart(line[e]) {
				p = e
				continue
			}
			q := e
			for q < len(line) && (line[q] == ' ' || line[q] == '\t') {
				q++
			}
			if q < len(line) && (line[q] == ':' || line[q] == '-' || line[q] == '(') {
				if !textDone {
					fn(hit, n, jtrunc(strings.TrimSpace(string(line)), 200))
					textDone = true
				}
				return
			}
		}
		p = e
	}
}

func identEndAt(b []byte, i int) int {
	for i < len(b) && c23IsIdentPart(b[i]) {
		i++
	}
	return i
}

var (
	PAmpEq   = mustPunct("&=")
	PPipeEq  = mustPunct("|=")
	PCaretEq = mustPunct("^=")
	PLshEq   = mustPunct("<<=")
	PRshEq   = mustPunct(">>=")
)

type c23tu struct {
	root      int32
	fa        *FileAST
	pp        *preprocessor
	lexErrs   []lexErr
	perrs     []parseError
	claimed   []int32
	files     []*derivedFile
	ferrs     []int32
	fmissing  []int32
	redundant bool
	funcSet   map[string]bool
	marks     []uint32
	markEp    uint32
}

type c23run struct {
	g       *Graph
	abs     string
	claim   map[string]int32
	nPanics int32
	paths   map[string]int32
	absID   map[string]int32
	quiet   bool
}

func (e *extractor) runC23(abs string, g *Graph, parseIDs []int32, quiet bool, step int) []*fileOut {
	cr := &c23run{g: g, abs: abs, quiet: quiet,
		claim: map[string]int32{}, paths: map[string]int32{},
		absID: map[string]int32{}}
	for _, id := range parseIDs {
		f := &g.Files[id-1]
		cr.paths[filepath.Join(abs, filepath.FromSlash(f.Path()))] = id
	}
	for i := range g.Files {
		ap := filepath.Join(abs, filepath.FromSlash(g.Files[i].Path()))
		if _, ok := cr.absID[ap]; !ok {
			cr.absID[ap] = g.Files[i].ID
		}
	}
	tus := make([]*c23tu, len(parseIDs))
	workers := min(defaultParseWorkers, len(parseIDs))
	if workers < 1 {
		workers = 1
	}
	// The first unit to include a header owns it, so claims are made in unit
	// order. Nothing else is ordered: a worker whose unit is parsed leaves it
	// for whoever reaches its turn and takes the next one, and a claimed unit
	// is derived by any free worker. Waiting for the turn in place, as this
	// used to, left a third of PostgreSQL's worker time idle behind whichever
	// unit happened to be large.
	//
	// A unit stays live -- tree, region and all -- from the start of its
	// parse to the end of its derivation, and the live set is bounded both by
	// count and by root-file bytes so one slow unit cannot pile the rest up.
	n := len(parseIDs)
	var (
		wg      sync.WaitGroup
		mu      sync.Mutex
		cond    = sync.NewCond(&mu)
		next    int // next unit to parse
		turn    int // next unit to claim
		live    int
		liveSz  int64
		ready   []int // claimed, waiting to be derived
		parsed  = make([]bool, n)
		regOf   = make([]*ppRegion, n)
		regFree []*ppRegion
	)
	charge := func(i int) int64 {
		return min(fileCharge(int64(g.Files[parseIDs[i]-1].Bytes)), parseByteBudget)
	}
	maxLive := 2 * workers
	retire := func(i int) {
		regFree = append(regFree, regOf[i])
		regOf[i] = nil
		live--
		liveSz -= charge(i)
	}
	for k := 0; k < workers; k++ {
		wg.Go(func() {
			mu.Lock()
			for {
				if k := len(ready); k > 0 {
					i := ready[k-1]
					ready = ready[:k-1]
					mu.Unlock()
					tu := tus[i]
					cr.deriveTU(tu)
					putFA(tu.fa)
					tu.fa = nil
					tu.pp = nil
					mu.Lock()
					retire(i)
					cond.Broadcast()
					continue
				}
				if next < n && (live == 0 ||
					(live < maxLive && liveSz+charge(next) <= parseByteBudget)) {
					i := next
					next++
					live++
					liveSz += charge(i)
					var reg *ppRegion
					if k := len(regFree); k > 0 {
						reg = regFree[k-1]
						regFree = regFree[:k-1]
					}
					mu.Unlock()
					if reg == nil {
						reg = newPPRegion()
					}
					tu := cr.parseTU(parseIDs[i], reg)
					mu.Lock()
					tus[i], regOf[i], parsed[i] = tu, reg, true
					for ; turn < n && parsed[turn]; turn++ {
						if t := tus[turn]; t != nil {
							cr.claimTU(t)
							ready = append(ready, turn)
						} else {
							retire(turn)
						}
					}
					cond.Broadcast()
					continue
				}
				if next >= n && live == 0 {
					mu.Unlock()
					return
				}
				cond.Wait()
			}
		})
	}
	wg.Wait()

	ppSrcMu.Lock()
	ppSrcCache = map[string][]byte{}
	ppSrcMu.Unlock()

	var nFail int32
	outs := make([]*fileOut, len(g.Files))
	for a := range tus {
		tu := tus[a]
		if tu == nil {
			nFail++
			continue
		}
		if len(tu.files) == 0 && !tu.redundant {
			nFail++
		}
		for k, df := range tu.files {
			fid := df.file.ID
			fl := &g.Files[fid-1]
			fl.NParseErrors = tu.ferrs[k]
			fl.NMissingNodes = tu.fmissing[k]
			fl.DocLines = df.file.DocLines
			if outs[fid-1] == nil {
				outs[fid-1] = e.fileOutFrom(cr, df)
			}
		}
	}
	g.FilesFailed += nFail
	return outs
}

// claimTU gives a parsed unit every file it read that no earlier unit has
// taken. Callers hold the scheduler lock and call in unit order.
func (cr *c23run) claimTU(tu *c23tu) {
	if tu.pp == nil {
		return
	}
	for _, src := range tu.pp.files {
		p := src.path
		if id, ok := cr.paths[p]; ok {
			if _, cdone := cr.claim[p]; !cdone {
				cr.claim[p] = tu.root
				tu.claimed = append(tu.claimed, id)
			} else if id == tu.root {
				tu.redundant = true
			}
		}
	}
}

func (cr *c23run) parseTU(fid int32, reg *ppRegion) (tu *c23tu) {
	tu = &c23tu{root: fid}
	defer func() {
		if r := recover(); r != nil {
			atomic.AddInt32(&cr.nPanics, 1)
			tu = &c23tu{root: fid}
		}
	}()
	f := &cr.g.Files[fid-1]
	abs := filepath.Join(cr.abs, filepath.FromSlash(f.Path()))
	if _, ok := ppCachedRead(abs); !ok {
		return nil
	}
	tu.fa = getFA()
	tu.pp = newPP(abs, nil, &tu.lexErrs, reg)
	toks := tu.pp.run(abs)
	srcs := map[uint32][]byte{}
	for id, fsrc := range tu.pp.fileByID {
		srcs[id] = fsrc.raw
	}
	// one node per 2-3 tokens is typical; sizing up front replaces a dozen
	// regrow-and-copy steps with one allocation
	if want := len(toks)/2 + 64; cap(tu.fa.Nodes) < want {
		tu.fa.Nodes = make([]Node, 0, want)
	}
	p := newParser(tu.fa, toks, srcs, 0, &tu.perrs)
	p.parseTU()
	putTokBuf(toks)
	tu.pp.out = nil
	for _, fsrc := range tu.pp.fileByID {
		fsrc.toks = nil
	}
	return tu
}

func (cr *c23run) deriveTU(tu *c23tu) {
	tu.files = make([]*derivedFile, 0, len(tu.claimed))
	tu.ferrs = make([]int32, 0, len(tu.claimed))
	tu.fmissing = make([]int32, 0, len(tu.claimed))
	for _, fid := range tu.claimed {
		df, nerrs, nmiss := cr.deriveOne(tu, fid)
		if df != nil {
			tu.files = append(tu.files, df)
			tu.ferrs = append(tu.ferrs, nerrs)
			tu.fmissing = append(tu.fmissing, nmiss)
		}
	}
	for _, df := range tu.files {
		df.ast = nil
		df.pp = nil
		df.tu, df.tuFuncs, df.glSet = nil, nil, nil
	}
	tu.marks, tu.funcSet = nil, nil
}

func (cr *c23run) deriveOne(tu *c23tu, fid int32) (df *derivedFile, nerrs int32, nmiss int32) {
	defer func() {
		if r := recover(); r != nil {
			atomic.AddInt32(&cr.nPanics, 1)
			df = nil
		}
	}()
	f := &cr.g.Files[fid-1]
	abs := filepath.Join(cr.abs, filepath.FromSlash(f.Path()))
	raw, ok := ppCachedRead(abs)
	if !ok {
		if d, err := os.ReadFile(abs); err == nil {
			raw = d
		} else {
			return nil, 0, 0
		}
	}
	var srcID uint32
	srcID = 1
	for _, fsrc := range tu.pp.files {
		if fsrc.path == abs {
			srcID = fsrc.id
			break
		}
	}
	bp := blankPool.Get().(*[]byte)
	blank := blankBuf(*bp, raw)
	nRaw := nlOffsets(raw)
	df = newDerivedFile(tu.fa, tu.pp, raw, srcID)
	df.blank = blank
	df.nlRaw = nRaw
	df.tu = tu
	df.buildOffMap()
	df.file = FileRow{ID: fid, Path: f.Path(), ModuleID: f.ModuleID}
	df.globalFileID = fid
	df.isTestF = f.IsTest
	df.isGenF = f.IsGenerated
	df.modID = f.ModuleID
	nErrs := 0
	keep := func(line int32, msg string) {
		if len(df.perr) < 200 {
			df.perr = append(df.perr, parseMsg{fileID: fid, line: line, msg: msg})
		}
	}
	if srcID == 1 {
		for i := range tu.lexErrs {
			// An #error is the code declining a configuration, not text the
			// parser failed to read: nothing near it is missing or merged. It
			// is listed, and kept out of the count that warns about that.
			if !strings.HasPrefix(tu.lexErrs[i].msg, "#error") {
				nErrs++
			}
			keep(tu.lexErrs[i].line, tu.lexErrs[i].msg)
		}
	}
	for i := range tu.perrs {
		if tu.perrs[i].file == srcID {
			nErrs++
			keep(tu.perrs[i].line, tu.perrs[i].msg)
		}
	}
	df.file.NParseErrors = int32(nErrs)
	df.file.NMissing = int32(len(tu.pp.cond))
	df.derive()
	*bp = blank
	blankPool.Put(bp)
	return df, int32(nErrs), int32(len(tu.pp.cond))
}

func (e *extractor) fileOutFrom(cr *c23run, df *derivedFile) *fileOut {
	fo := &fileOut{symA: newSymBlockN(len(df.symbols)), perr: df.perr}
	block := fo.symA
	incCtx := map[int32]string{}
	sysCtx := map[int32]bool{}
	for _, inc := range df.incRows {
		if _, ok := incCtx[inc.line]; !ok {
			incCtx[inc.line] = inc.resolved
			sysCtx[inc.line] = inc.sys
		}
	}
	gfid := df.globalFileID
	for i := range df.symbols {
		s := &df.symbols[i]
		w := symW{ID: int32(i + 1), FileID: gfid, ModuleID: df.modID,
			Name: s.Name, QualName: s.QualName, Kind: s.Kind,
			LineStart: s.LineStart, Signature: s.Signature,
			ReturnType: s.ReturnType, HasSignature: s.HasSignature,
			HasReturnType: s.HasReturnType, NParams: s.NParams,
			IsPublic: s.IsPublic, IsStatic: s.IsStatic,
			IsAbstract: s.IsAbstract, IsOverride: s.IsOverride,
			IsTest: s.IsTest, IsEntrypoint: s.IsEntrypoint,
			IsGenerated: s.IsGenerated, Sloc: s.Sloc,
			NCommentLines: s.NCommentLines, HasDoc: s.HasDoc,
			Cyclomatic: s.Cyclomatic, Cognitive: s.Cognitive,
			MaxNesting: s.MaxNesting, NTokens: s.NTokens,
			NOperators: s.NOperators, NOperands: s.NOperands,
			NDistinctOperators: s.NDistinctOperators,
			NDistinctOperands:  s.NDistinctOperands,
			NLoops:             s.NLoops, NBranches: s.NBranches, NReturns: s.NReturns,
			NSwitch: s.NSwitch, NCases: s.NCases, NLabels: s.NLabels,
			NGotos: s.NGotos, MaxLoopDepth: s.MaxLoopDepth,
			CallInLoop: s.CallInLoop, AllocInLoop: s.AllocInLoop,
			IOInLoop: s.IOInLoop, LockInLoop: s.LockInLoop,
			BranchInLoop: s.BranchInLoop, NLocals: s.NLocals,
			NCmp: s.NCmp, NArith: s.NArith, NShift: s.NShift,
			NFloatLit: s.NFloatLit, NMagic: s.NMagic,
			NNullCheck: s.NNullCheck, NCalls: s.NCalls,
			NDynamicCalls:    s.NDynamicCalls,
			NUnresolvedCalls: s.NUnresolvedCalls, FanIn: s.FanIn,
			FanOut: s.FanOut, NCallsites: s.NCallsites,
			IsRecursive: s.IsRecursive, RiskScore: s.RiskScore,
			NMemory: s.NMemory, NAlloc: s.NAlloc, NIO: s.NIO,
			NStdio: s.NStdio, NExec: s.NExec, NLibm: s.NLibm,
			NInteger: s.NInteger, NConcurrency: s.NConcurrency,
			NReentrancy: s.NReentrancy, IsInline: s.IsInline,
			IsVariadic: s.IsVariadic, NPtrLocals: s.NPtrLocals,
			NDeref: s.NDeref, NCast: s.NCast, NSizeof: s.NSizeof,
			NIntrinsic: s.NIntrinsic, NAtomic: s.NAtomic,
			NRestrict: s.NRestrict, NLikely: s.NLikely, NBuiltin: s.NBuiltin}
		if fo.symN == 0 {
			fo.symLo = block.count()
		}
		r := rowRare(&s.SymbolRare)
		if r != (symRare{}) {
			w.rare = block.addRare(r)
			fo.rareN++
		} else {
			w.rare = -1
		}
		block.add(&w)
		fo.symN++
	}
	for i := range df.params {
		p := &df.params[i]
		fo.params = append(fo.params, paramW{SymbolID: p.SymbolID, Pos: p.Pos,
			Name: p.Name, HasName: p.HasName, Type: p.Type,
			IsVariadic: p.IsVariadic, IsRef: p.IsRef, IsMutable: p.IsMutable,
			IsNullable: p.IsNullable, TypeDepth: p.TypeDepth})
	}
	for i := range df.fields {
		p := &df.fields[i]
		fo.fields = append(fo.fields, fieldW{SymbolID: p.SymbolID,
			Ordinal: p.Ordinal, Name: p.Name, Type: p.Type,
			Visibility: p.Visibility, Line: p.Line, IsStatic: p.IsStatic,
			IsConst: p.IsConst, IsMutable: p.IsMutable,
			IsNullable: p.IsNullable, IsCollection: p.IsCollection,
			IsUntyped: p.IsUntyped, HasDefault: p.HasDefault,
			TypeDepth: p.TypeDepth})
	}
	for i := range df.locals {
		p := &df.locals[i]
		fo.locals = append(fo.locals, localW{SymbolID: p.SymbolID,
			Ordinal: p.Ordinal, Name: p.Name, Type: p.Type, Line: p.Line,
			IsConst: p.IsConst, IsMutable: p.IsMutable, IsUntyped: p.IsUntyped,
			HasInit: p.HasInit, InLoop: p.InLoop, ScopeDepth: p.ScopeDepth})
	}
	for i := range df.lits {
		p := &df.lits[i]
		fo.literals = append(fo.literals, literalW{ID: p.ID,
			SymbolID: p.SymbolID, HasSym: p.HasSym, FileID: gfid,
			Kind: p.Kind, Value: p.Value, Line: p.Line, IsMagic: p.IsMagic})
	}
	for i := range df.markers {
		p := &df.markers[i]
		fo.markers = append(fo.markers, markerW{ID: p.ID, FileID: gfid,
			SymbolID: p.SymbolID, HasSym: p.HasSym, Kind: p.Kind,
			Line: p.Line, Text: p.Text})
	}
	for i := range df.imports {
		p := &df.imports[i]
		im := importW{FileID: gfid, Target: p.Target, Kind: p.Kind,
			Line: p.Line, IsExternal: p.IsExternal, IsRelative: p.IsRelative,
			NNames: p.NNames}
		if resolved, ok := incCtx[p.Line]; ok && resolved != "" {
			if tid, ok2 := cr.absID[filepath.Clean(resolved)]; ok2 {
				im.TargetID, im.HasTargetID = tid, true
				im.IsExternal = 0
			}
		}
		fo.imports = append(fo.imports, im)
	}
	for i := range df.hazards {
		p := &df.hazards[i]
		fo.hazards = append(fo.hazards, hazardW{SymbolID: p.SymbolID,
			Pattern: p.Pattern, Category: p.Category, N: p.N,
			FirstLine: p.FirstLine})
	}
	for i := range df.enums {
		p := &df.enums[i]
		fo.enums = append(fo.enums, enumW{SymbolID: p.SymbolID,
			Ordinal: p.Ordinal, Name: p.Name, Value: p.Value,
			HasValue: p.HasValue, NFields: p.NFields})
	}
	for i := range df.layout {
		p := &df.layout[i]
		fo.layout = append(fo.layout, LayoutRow{SymbolID: p.SymbolID,
			Ordinal: p.Ordinal, ByteOff: p.ByteOff, ByteSize: p.ByteSize,
			PadBefore: p.PadBefore, Exact: p.Exact, PtrDepth: p.PtrDepth,
			ArrayLen: p.ArrayLen, IsFnptr: p.IsFnptr, Depth: p.Depth,
			InUnion: p.InUnion})
	}
	for i := range df.ssize {
		p := &df.ssize[i]
		fo.ssize = append(fo.ssize, StructSize{SymbolID: p.SymbolID,
			TotalSize: p.TotalSize, TailPad: p.TailPad, TotalPad: p.TotalPad,
			MaxAlign: p.MaxAlign, Exact: p.Exact, NLines64: p.NLines64})
	}
	for i := range df.decls {
		p := &df.decls[i]
		fo.decls = append(fo.decls, declW{ID: p.ID, FileID: gfid,
			Name: p.Name, Line: p.Line})
	}
	for i := range df.addrs {
		p := &df.addrs[i]
		fo.addrs = append(fo.addrs, addrW{ID: p.ID, SymbolID: p.SymbolID,
			HasSym: p.HasSym, FileID: gfid, Name: p.Name, Line: p.Line,
			Kind: p.Kind})
	}
	for i := range df.secrets {
		p := &df.secrets[i]
		fo.secrets = append(fo.secrets, secretW{ID: p.ID, SymbolID: p.SymbolID,
			HasSym: p.HasSym, FileID: gfid, Value: p.Value, Line: p.Line})
	}
	for i := range df.allocs {
		p := &df.allocs[i]
		fo.allocs = append(fo.allocs, allocW{ID: p.ID, SymbolID: p.SymbolID,
			FileID: gfid, Fn: p.Fn, SizeExpr: p.SizeExpr, Line: p.Line})
	}
	for i := range df.memops {
		p := &df.memops[i]
		fo.memops = append(fo.memops, memopW{ID: p.ID, SymbolID: p.SymbolID,
			FileID: gfid, Fn: p.Fn, Dst: p.Dst, Src: p.Src,
			SizeArg: p.SizeArg, SizeBuf: p.SizeBuf, DstTail: p.DstTail,
			Line: p.Line})
	}
	for i := range df.macros {
		p := &df.macros[i]
		fo.macros = append(fo.macros, macroW{SymbolID: p.SymbolID,
			IsFunctionlike: p.IsFunctionlike, NParams: p.NParams,
			Body: p.Body, HasBody: p.HasBody, BodyLen: p.BodyLen,
			IsMultiline: p.IsMultiline, NUses: p.NUses})
		fo.macroNames = append(fo.macroNames, df.macroName[i])
	}
	for i := range df.globals {
		p := &df.globals[i]
		fo.globals = append(fo.globals, globalW{ID: p.ID, FileID: gfid,
			ModuleID: df.modID, Name: p.Name, Type: p.Type, Line: p.Line,
			IsStatic: p.IsStatic, IsConst: p.IsConst, IsVolatile: p.IsVolatile,
			IsAtomic: p.IsAtomic, IsArray: p.IsArray, PtrDepth: p.PtrDepth,
			HasInit: p.HasInit})
	}
	for i := range df.cfgs {
		p := &df.cfgs[i]
		fo.cfgs = append(fo.cfgs, cfgW{ID: p.ID, FileID: gfid,
			Directive: p.Directive, Expr: p.Expr, Line: p.Line,
			IsConfig: p.IsConfig})
	}
	for i := range df.locks {
		p := &df.locks[i]
		fo.locks = append(fo.locks, lockW{ID: p.ID, SymbolID: p.SymbolID,
			FileID: gfid, Name: p.Name, Line: p.Line})
	}
	for i := range df.attrs {
		p := &df.attrs[i]
		fo.attrs = append(fo.attrs, attrW{ID: p.ID, SymbolID: p.SymbolID,
			HasSymbolID: p.HasSymbolID, FileID: gfid, Name: p.Name,
			Args: p.Args, HasArgs: p.HasArgs, Line: p.Line})
	}
	for i := range df.evops {
		p := &df.evops[i]
		fo.evops = append(fo.evops, evopW{ID: p.ID, SymbolID: p.SymbolID,
			FileID: gfid, Family: p.Family, Fn: p.Fn, Args: p.Args,
			Line: p.Line})
	}
	for i := range df.apiuses {
		p := &df.apiuses[i]
		fo.apiuses = append(fo.apiuses, apiuseW{ID: p.ID, SymbolID: p.SymbolID,
			FileID: gfid, NS: p.NS, Fn: p.Fn, Line: p.Line})
	}
	for i := range df.pendNames {
		fo.pendSID = append(fo.pendSID, df.pendSym[i])
		fo.pendFID = append(fo.pendFID, df.pendFile[i])
		fo.pendMID = append(fo.pendMID, df.pendModule[i])
		fo.pendName = append(fo.pendName, df.pendNames[i])
		fo.pendLine = append(fo.pendLine, df.pendLines[i])
	}
	for k, nm := range df.fnNames {
		fo.funcs = append(fo.funcs, fnSite{name: nm, sid: df.fnSyms[k],
			fid: df.fnFiles[k], mid: df.fnModules[k], static: df.fnStatic[k]})
	}
	return fo
}

func rowRare(s *SymbolRare) symRare {
	return symRare{
		SwitchInLoop:         s.SwitchInLoop,
		LibmInLoop:           s.LibmInLoop,
		DivInLoop:            s.DivInLoop,
		StrlenInLoop:         s.StrlenInLoop,
		RetNull:              s.RetNull,
		RetNeg:               s.RetNeg,
		RetZero:              s.RetZero,
		RetVal:               s.RetVal,
		RetVoid:              s.RetVoid,
		NFnptrCalls:          s.NFnptrCalls,
		NMacroCalls:          s.NMacroCalls,
		NExternalCalls:       s.NExternalCalls,
		NFree:                s.NFree,
		NConstCast:           s.NConstCast,
		NToctou:              s.NToctou,
		NLockAcquire:         s.NLockAcquire,
		NLockRelease:         s.NLockRelease,
		NNarrowCast:          s.NNarrowCast,
		NSignCmp:             s.NSignCmp,
		NVariadicFmt:         s.NVariadicFmt,
		NMemcpy:              s.NMemcpy,
		NAllocsite:           s.NAllocsite,
		NGlobalWrite:         s.NGlobalWrite,
		NErrno:               s.NErrno,
		NWeakRandom:          s.NWeakRandom,
		NShiftVar:            s.NShiftVar,
		NReallocSelf:         s.NReallocSelf,
		NVla:                 s.NVla,
		NGetenv:              s.NGetenv,
		NAssertSide:          s.NAssertSide,
		NFreeThenUse:         s.NFreeThenUse,
		NEpoll:               s.NEpoll,
		NUring:               s.NUring,
		NKqueue:              s.NKqueue,
		NEventWait:           s.NEventWait,
		NEtReg:               s.NEtReg,
		NOneshotReg:          s.NOneshotReg,
		NWriteReady:          s.NWriteReady,
		NErrFlag:             s.NErrFlag,
		NRearm:               s.NRearm,
		NDereg:               s.NDereg,
		NEagain:              s.NEagain,
		NEintr:               s.NEintr,
		NUringRes:            s.NUringRes,
		NUringRing:           s.NUringRing,
		NUringBarrier:        s.NUringBarrier,
		NUringSqpoll:         s.NUringSqpoll,
		NNonblockSet:         s.NNonblockSet,
		NEventCreate:         s.NEventCreate,
		NEventDestroy:        s.NEventDestroy,
		NUringSqe:            s.NUringSqe,
		NUringSeen:           s.NUringSeen,
		NUringUdata:          s.NUringUdata,
		NUringLink:           s.NUringLink,
		NUringStreamOps:      s.NUringStreamOps,
		NUringTeardown:       s.NUringTeardown,
		NEvTimeoutIndefinite: s.NEvTimeoutIndefinite,
		NEvTimeoutZero:       s.NEvTimeoutZero,
		NEvBatchOne:          s.NEvBatchOne,
		NKqTimer:             s.NKqTimer,
		NKqTimerZeroData:     s.NKqTimerZeroData,
		NKqReceipt:           s.NKqReceipt,
		NRetNegCheck:         s.NRetNegCheck,
		NDomainGuard:         s.NDomainGuard,
		NUcharCast:           s.NUcharCast,
		NErrnoZero:           s.NErrnoZero,
		NEndptr:              s.NEndptr,
		NVaEnd:               s.NVaEnd,
		NMapFailed:           s.NMapFailed,
		NMonotonicClock:      s.NMonotonicClock,
		NStackszArray:        s.NStackszArray,
		NPtrOvfCheck:         s.NPtrOvfCheck,
		NCallocTransposed:    s.NCallocTransposed,
		NVaArgArr:            s.NVaArgArr,
	}
}

// ===================================================================
// DYN PROJECT (dynajs) -- everything below this line is project-specific.
// Above it is github.com/corporatepiyush/codegraph go/codegraph_c.go; to
// take a newer upstream, replace the part above and keep the import union.
// ===================================================================

// DYN PROJECT: the dynajs-only part of the code graph. codegraph_c.go beside
// this file is the upstream tool, copied verbatim from
// https://github.com/corporatepiyush/codegraph (go/codegraph_c.go); everything
// project-specific lives HERE and registers itself from init(), so taking a
// newer upstream is a one-file copy.
//
// What this adds:
//   - 23 queries and 4 metrics about the JS runtime boundary, the untrusted
//     frontier and the module structure (the catalogue the Python
//     bench/codegraph.py carried as DYN_QUERIES / DYN_METRICS);
//   - the JS-API call table they are computed from (dynAPI);
//   - --defects, the line-level checker the gate runs (codegraph_defects.go).
func init() {
	queries = append(queries, dynQueries...)
	metricsCat = append(metricsCat, dynMetrics...)
	for _, d := range dynDefines() {
		predefMacros = append(predefMacros, ppMacro{Name: d[0],
			Body: ppPredefBody(d[1]), Raw: d[1]})
		if d[0] == "CONFIG_IO_URING" {
			// liburing.h is not installed where the graph usually runs, and
			// its iteration macro opens a loop the parser must see as one.
			const body = "for (head = 0; cqe; head++)"
			predefMacros = append(predefMacros, ppMacro{Name: "io_uring_for_each_cqe",
				IsFn: true, Params: []string{"ring", "head", "cqe"},
				Body: ppPredefBody(body), Raw: body})
		}
	}
}

// ------------------------------------------------------------ configuration

// The upstream tool runs a real preprocessor, and every native module in this
// tree sits inside `#if defined(CONFIG_NATIVE_MODULES) && defined(
// CONFIG_NATIVE_MODULE_X)`. With nothing defined, 53 files -- the whole dyna:*
// layer, which is the untrusted frontier -- contribute ZERO functions (measured:
// 4,383 functions seen of 9,500), and every query answers about the engine
// core alone while looking complete.
//
// CODEGRAPH_DEFINES names the configuration to analyse ("A B=2 C"); build.sh
// passes the -D set of the build it gates, so the graph is of the code that
// ships. Without it the default is the full build: CONFIG_NATIVE_MODULES,
// CONFIG_TLS, and every CONFIG_NATIVE_MODULE_* the tree itself tests for --
// found by scanning, so a module added later is not silently left out.
var dynModuleMacro = regexp.MustCompile(`\bCONFIG_NATIVE_MODULE_[A-Z0-9_]+\b`)

func dynScanRoot() string {
	skipNext := map[string]bool{"--module": true, "--limit": true, "--json": true,
		"--csv": true, "--dump": true, "--save": true, "--save-ast": true,
		"--load-ast": true, "--defects-baseline": true, "--update-baseline": true,
		"--jobs": true}
	args := os.Args[1:]
	for i := 0; i < len(args); i++ {
		a := args[i]
		if strings.HasPrefix(a, "-") {
			if skipNext[a] {
				i++
			}
			continue
		}
		if st, err := os.Stat(a); err == nil && st.IsDir() {
			return a
		}
		break
	}
	return "."
}

func dynDefines() [][2]string {
	if env := strings.TrimSpace(os.Getenv("CODEGRAPH_DEFINES")); env != "" {
		var out [][2]string
		for _, f := range strings.Fields(env) {
			name, val, has := strings.Cut(f, "=")
			if !has {
				val = "1"
			}
			if name != "" {
				out = append(out, [2]string{name, val})
			}
		}
		return out
	}
	seen := map[string]bool{"CONFIG_NATIVE_MODULES": true, "CONFIG_TLS": true}
	filepath.WalkDir(dynScanRoot(), func(path string, d os.DirEntry, err error) error {
		if err != nil {
			return nil
		}
		if d.IsDir() {
			if n := d.Name(); n != "." && (strings.HasPrefix(n, ".") || n == "third_party" ||
				n == "test262" || n == "node_modules") {
				return filepath.SkipDir
			}
			return nil
		}
		if ext := filepath.Ext(path); ext != ".c" && ext != ".h" && ext != ".inc" {
			return nil
		}
		if src, err := os.ReadFile(path); err == nil {
			for _, m := range dynModuleMacro.FindAll(src, -1) {
				seen[string(m)] = true
			}
		}
		return nil
	})
	names := make([]string, 0, len(seen))
	for n := range seen {
		names = append(names, n)
	}
	// CODEGRAPH_EXTRA_DEFINES adds to the scanned set instead of replacing it:
	// the gate's second pass reads the io_uring backend with
	// CODEGRAPH_EXTRA_DEFINES=CONFIG_IO_URING, which swaps dyna-aio.c out for
	// dyna-aio-uring.c (the two define the same functions).
	for _, f := range strings.Fields(os.Getenv("CODEGRAPH_EXTRA_DEFINES")) {
		if name, _, _ := strings.Cut(f, "="); name != "" {
			seen[name] = true
		}
	}
	names = names[:0]
	for n := range seen {
		names = append(names, n)
	}
	slices.Sort(names)
	out := make([][2]string, len(names))
	for i, n := range names {
		out[i] = [2]string{n, "1"}
	}
	return out
}

// ---------------------------------------------------------------- api table

// One function's calls to the JS runtime API, by callee name. Counted from the
// raw text of the function body: the names are macros as often as functions,
// and the call graph only resolves the latter.
type dynAPITable struct {
	bySym map[int32]map[string]int32
	// the raw text of each function body, `{` to `}`, and the line it opens on:
	// what the shape queries in codegraph_dyn_shapes.go read
	body     map[int32][]byte
	bodyLine map[int32]int32
	files    int
	names    int
	miss     int
}

var (
	dynAPIOnce  sync.Once
	dynAPICache *dynAPITable
	dynAPICall  = regexp.MustCompile(`\b(JS_[A-Za-z0-9_]*|js_[a-z0-9_]+)\s*\(`)
)

// dynBodyEnd returns the offset one past the `}` closing the first `{` at or
// after start, skipping comments, strings and character literals. len(src) if
// the body never closes (a parse the upstream tool already diagnosed).
func dynBodyEnd(src []byte, start int) (open, end int) {
	i, n := start, len(src)
	depth := 0
	open = -1
	for i < n {
		c := src[i]
		switch {
		case c == '/' && i+1 < n && src[i+1] == '/':
			for i < n && src[i] != '\n' {
				i++
			}
			continue
		case c == '/' && i+1 < n && src[i+1] == '*':
			i += 2
			for i+1 < n && !(src[i] == '*' && src[i+1] == '/') {
				i++
			}
			i += 2
			continue
		case c == '"' || c == '\'':
			q := c
			i++
			for i < n && src[i] != q {
				if src[i] == '\\' {
					i++
				}
				if i < n && src[i] == '\n' && q == '\'' {
					break
				}
				i++
			}
			i++
			continue
		case c == ';' && depth == 0 && open < 0:
			// a prototype, not a definition: no body to read
			return -1, i + 1
		case c == '{':
			if depth == 0 && open < 0 {
				open = i
			}
			depth++
		case c == '}':
			depth--
			if depth == 0 && open >= 0 {
				return open, i + 1
			}
		}
		i++
	}
	return open, n
}

func (g *Graph) dynAPI() *dynAPITable {
	dynAPIOnce.Do(func() {
		t := &dynAPITable{bySym: map[int32]map[string]int32{},
			body: map[int32][]byte{}, bodyLine: map[int32]int32{}}
		root := g.Meta["root"]
		byFile := map[int32][]*Symbol{}
		for _, s := range g.Symbols {
			if s.Kind() == "function" && s.LineStart > 0 {
				byFile[s.FileID] = append(byFile[s.FileID], s)
			}
		}
		for i := range g.Files {
			f := &g.Files[i]
			fns := byFile[f.ID]
			if len(fns) == 0 || f.Parsed == 0 || f.IsTest != 0 {
				continue
			}
			src, err := os.ReadFile(filepath.Join(root, f.Path()))
			if err != nil {
				t.miss++
				continue
			}
			t.files++
			lineOff := []int{0}
			for j, b := range src {
				if b == '\n' {
					lineOff = append(lineOff, j+1)
				}
			}
			for _, s := range fns {
				ls := int(s.LineStart)
				if ls < 1 || ls > len(lineOff) {
					continue
				}
				open, end := dynBodyEnd(src, lineOff[ls-1])
				if open < 0 || end > len(src) {
					continue
				}
				t.body[s.ID] = src[open:end]
				t.bodyLine[s.ID] = int32(ls + bytes.Count(src[lineOff[ls-1]:open], []byte{'\n'}))
				counts := map[string]int32{}
				for _, m := range dynAPICall.FindAllSubmatch(src[open:end], -1) {
					name := string(m[1])
					if strings.HasPrefix(name, "JS_VALUE_GET_") {
						continue
					}
					counts[name]++
				}
				if len(counts) > 0 {
					t.bySym[s.ID] = counts
					t.names += len(counts)
				}
			}
		}
		dynAPICache = t
	})
	return dynAPICache
}

func (t *dynAPITable) sum(id int32, pred func(string) bool) int32 {
	n := int32(0)
	for name, c := range t.bySym[id] {
		if pred(name) {
			n += c
		}
	}
	return n
}

func dynPrefix(ps ...string) func(string) bool {
	return func(s string) bool {
		for _, p := range ps {
			if strings.HasPrefix(s, p) {
				return true
			}
		}
		return false
	}
}

func dynOneOf(ns ...string) func(string) bool {
	return func(s string) bool { return slices.Contains(ns, s) }
}

// ------------------------------------------------------------------ helpers

func dynSort(rows [][]fval, cmp func(a, b []fval) int) {
	slices.SortStableFunc(rows, cmp)
}

func dynCmpI(a, b int64) int {
	switch {
	case a < b:
		return -1
	case a > b:
		return 1
	}
	return 0
}

// descending on the listed integer columns, in order
func dynDesc(cols ...int) func(a, b []fval) int {
	return func(a, b []fval) int {
		for _, c := range cols {
			if d := dynCmpI(b[c].i, a[c].i); d != 0 {
				return d
			}
		}
		return 0
	}
}

func (g *Graph) dynFns(p params) []*Symbol {
	var out []*Symbol
	for _, s := range g.Symbols {
		if s.Kind() == "function" && likeMatch(g.modName(s.ModuleID), p.mod) {
			out = append(out, s)
		}
	}
	return out
}

// Every function reachable from one that reads input, through resolved call
// edges. Calls through a function pointer are not edges, so this is a floor.
func (g *Graph) dynTainted() []bool {
	t := make([]bool, len(g.Symbols)+1)
	out := g.edgeCSR(false)
	var stack []int32
	for _, s := range g.Symbols {
		if s.Kind() == "function" && s.NIO > 0 {
			t[s.ID] = true
			stack = append(stack, s.ID)
		}
	}
	for len(stack) > 0 {
		id := stack[len(stack)-1]
		stack = stack[:len(stack)-1]
		for _, j := range out.of(id) {
			c := g.Edges[j].CalleeID
			if c >= 1 && int(c) < len(t) && !t[c] {
				t[c] = true
				stack = append(stack, c)
			}
		}
	}
	return t
}

type dynCloneKey struct{ sloc, cyclo, params, returns, branches int32 }

// Functions of 20+ lines grouped by an identical structural signature. A group
// only matters when it spans more than one file.
func (g *Graph) dynCloneGroups() [][]*Symbol {
	m := map[dynCloneKey][]*Symbol{}
	for _, s := range g.Symbols {
		if s.Kind() != "function" || s.Sloc < 20 {
			continue
		}
		k := dynCloneKey{s.Sloc, s.Cyclomatic, s.NParams, s.NReturns, s.NBranches}
		m[k] = append(m[k], s)
	}
	var out [][]*Symbol
	for _, v := range m {
		if len(v) > 1 {
			slices.SortFunc(v, func(a, b *Symbol) int { return dynCmpI(int64(a.ID), int64(b.ID)) })
			out = append(out, v)
		}
	}
	return out
}

// ------------------------------------------------------------------ queries

func dynUntrustedReach(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "io", "mem", "alloc", "cyclo", "fan_in", "at"}
	t := g.dynTainted()
	var rows [][]fval
	for _, s := range g.Symbols {
		if !t[s.ID] || !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.NIO), fI32(s.NMemory),
			fI32(s.NAlloc), fI32(s.Cyclomatic), fI32(s.FanIn), fStr(g.at(s))))
	}
	dynSort(rows, func(a, b []fval) int {
		if d := dynCmpI(b[2].i+b[3].i, a[2].i+a[3].i); d != 0 {
			return d
		}
		return dynCmpI(b[1].i, a[1].i)
	})
	return cols, limit(rows, p)
}

var (
	dynIsCreate = dynPrefix("JS_New", "JS_Dup")
	dynIsFree   = dynPrefix("JS_FreeValue", "js_free_")
)

func dynRefcountRows(g *Graph, p params, keep func(al, fr int32) bool) [][]fval {
	api := g.dynAPI()
	var rows [][]fval
	for _, s := range g.dynFns(p) {
		al, fr := api.sum(s.ID, dynIsCreate), api.sum(s.ID, dynIsFree)
		if !keep(al, fr) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(al), fI32(fr), fI32(al-fr),
			fI32(s.NReturns), fStr(s.ReturnType()), fStr(g.at(s))))
	}
	return rows
}

var dynRefcountCols = []string{"name", "alloc_dup", "free_n", "drift", "n_returns", "return_type", "at"}

func dynRefcountLeak(g *Graph, p params) ([]string, [][]fval) {
	rows := dynRefcountRows(g, p, func(al, fr int32) bool { return al > 0 || fr > 0 })
	dynSort(rows, dynDesc(3, 4))
	return dynRefcountCols, limit(rows, p)
}

func dynOverFree(g *Graph, p params) ([]string, [][]fval) {
	rows := dynRefcountRows(g, p, func(al, fr int32) bool { return fr > al })
	dynSort(rows, func(a, b []fval) int { return dynCmpI(a[3].i, b[3].i) })
	return dynRefcountCols, limit(rows, p)
}

func dynDefineVsSet(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "set_calls", "on_frontier", "at"}
	api, t := g.dynAPI(), g.dynTainted()
	isSet := dynPrefix("JS_SetProperty")
	var rows [][]fval
	for _, s := range g.Symbols {
		n := api.sum(s.ID, isSet)
		if n == 0 || !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		on := 0
		if t[s.ID] {
			on = 1
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(n), fInt(on), fStr(g.at(s))))
	}
	dynSort(rows, dynDesc(1, 2))
	return cols, limit(rows, p)
}

func dynThrowingPath(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "n_returns", "n_calls", "return_type", "at"}
	api := g.dynAPI()
	isThrow := dynPrefix("JS_Throw", "JS_IsException")
	var rows [][]fval
	for _, s := range g.dynFns(p) {
		if !strings.HasPrefix(s.ReturnType(), "JSValue") || s.NReturns <= 1 ||
			api.sum(s.ID, isThrow) != 0 {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.NReturns), fI32(s.NCalls),
			fStr(s.ReturnType()), fStr(g.at(s))))
	}
	dynSort(rows, dynDesc(1, 2))
	return cols, limit(rows, p)
}

func dynLazyStatic(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "type", "is_static", "is_const", "has_init", "at"}
	var rows [][]fval
	for i := range g.Globals {
		gl := &g.Globals[i]
		if gl.IsConst != 0 || !(gl.IsStatic == 0 || gl.HasInit == 0) {
			continue
		}
		if gl.FileID < 1 || int(gl.FileID) > len(g.Files) ||
			!likeMatch(g.modName(gl.ModuleID), p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(gl.Name()), fStr(gl.Type()), fI32(gl.IsStatic),
			fI32(gl.IsConst), fI32(gl.HasInit),
			fStr(g.Files[gl.FileID-1].Path()+":"+strconv.Itoa(int(gl.Line)))))
	}
	dynSort(rows, func(a, b []fval) int {
		if d := dynCmpI(b[2].i, a[2].i); d != 0 {
			return d
		}
		if d := dynCmpI(a[4].i, b[4].i); d != 0 {
			return d
		}
		return strings.Compare(a[5].s, b[5].s)
	})
	return cols, limit(rows, p)
}

// The distinct cross-module (caller module, callee module) pairs.
func (g *Graph) dynModEdges() map[[2]int32]bool {
	me := map[[2]int32]bool{}
	for i := range g.Edges {
		e := &g.Edges[i]
		a, b := g.sym(e.CallerID), g.sym(e.CalleeID)
		if a == nil || b == nil || a.ModuleID <= 0 || b.ModuleID <= 0 || a.ModuleID == b.ModuleID {
			continue
		}
		me[[2]int32{a.ModuleID, b.ModuleID}] = true
	}
	return me
}

func dynModuleCycles(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"cycle", "a_calls_out", "b_calls_in"}
	me := g.dynModEdges()
	n := len(g.Modules) + 1
	adj := make([][]int32, n)
	nOut, nIn := make([]int32, n), make([]int32, n)
	for k := range me {
		adj[k[0]] = append(adj[k[0]], k[1])
		nOut[k[0]]++
		nIn[k[1]]++
	}
	reach := make([][]bool, n)
	for m := 1; m < n; m++ {
		seen := make([]bool, n)
		stack := append([]int32(nil), adj[m]...)
		for len(stack) > 0 {
			x := stack[len(stack)-1]
			stack = stack[:len(stack)-1]
			if seen[x] {
				continue
			}
			seen[x] = true
			stack = append(stack, adj[x]...)
		}
		reach[m] = seen
	}
	var rows [][]fval
	for a := 1; a < n; a++ {
		for b := a + 1; b < n; b++ {
			if !reach[a][b] || !reach[b][a] || !likeMatch(g.modName(int32(a)), p.mod) {
				continue
			}
			rows = append(rows, mkrow(fStr(g.modName(int32(a))+" <-> "+g.modName(int32(b))),
				fI32(nOut[a]), fI32(nIn[b])))
		}
	}
	dynSort(rows, func(x, y []fval) int { return strings.Compare(x[0].s, y[0].s) })
	return cols, limit(rows, p)
}

func dynFeatureEnvy(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "own_module", "envied_module", "cross_edges", "own_edges", "at"}
	out := g.edgeCSR(false)
	var rows [][]fval
	for _, s := range g.dynFns(p) {
		if s.ModuleID <= 0 {
			continue
		}
		own := int32(0)
		cross := map[int32]int32{}
		for _, j := range out.of(s.ID) {
			e := &g.Edges[j]
			if e.SameModule != 0 {
				own++
				continue
			}
			if c := g.sym(e.CalleeID); c != nil && c.ModuleID > 0 {
				cross[c.ModuleID]++
			}
		}
		for mod, n := range cross {
			if n >= 4 && n >= 2*own+2 {
				rows = append(rows, mkrow(fStr(s.Name()), fStr(g.modName(s.ModuleID)),
					fStr(g.modName(mod)), fI32(n), fI32(own), fStr(g.at(s))))
			}
		}
	}
	dynSort(rows, func(a, b []fval) int {
		if d := dynCmpI(b[3].i, a[3].i); d != 0 {
			return d
		}
		if d := strings.Compare(a[0].s, b[0].s); d != 0 {
			return d
		}
		return strings.Compare(a[2].s, b[2].s)
	})
	return cols, limit(rows, p)
}

func dynShotgun(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "file_breadth", "fan_in", "module", "at"}
	in := g.edgeCSR(true)
	var rows [][]fval
	seen := map[int32]bool{}
	for _, s := range g.Symbols {
		if !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		clear(seen)
		for _, j := range in.of(s.ID) {
			if c := g.sym(g.Edges[j].CallerID); c != nil {
				seen[c.FileID] = true
			}
		}
		if len(seen) < 8 {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fInt(len(seen)), fI32(s.FanIn),
			fStr(g.modName(s.ModuleID)), fStr(g.at(s))))
	}
	dynSort(rows, dynDesc(1, 2))
	return cols, limit(rows, p)
}

func dynGodFunctions(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "sloc", "cyclomatic", "n_switch", "fan_in", "n_returns", "at"}
	var rows [][]fval
	for _, s := range g.dynFns(p) {
		if s.Sloc < 300 || s.Cyclomatic < 25 {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.Sloc), fI32(s.Cyclomatic),
			fI32(s.NSwitch), fI32(s.FanIn), fI32(s.NReturns), fStr(g.at(s))))
	}
	dynSort(rows, func(a, b []fval) int { return dynCmpI(b[1].i*b[2].i, a[1].i*a[2].i) })
	return cols, limit(rows, p)
}

func dynMiddleMen(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "sloc", "fan_in", "fan_out", "module", "at"}
	var rows [][]fval
	for _, s := range g.dynFns(p) {
		if s.Sloc > 10 || s.FanOut != 1 || s.Cyclomatic > 2 || s.FanIn < 2 || s.IsStatic != 0 {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.Sloc), fI32(s.FanIn),
			fI32(s.FanOut), fStr(g.modName(s.ModuleID)), fStr(g.at(s))))
	}
	dynSort(rows, dynDesc(2))
	return cols, limit(rows, p)
}

func dynDeadExported(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "sloc", "n_calls", "n_transitive", "at"}
	addr := make([]bool, len(g.Symbols)+1)
	for i := range g.Addr {
		a := &g.Addr[i]
		if a.HasSym && a.SymbolID >= 1 && int(a.SymbolID) < len(addr) {
			addr[a.SymbolID] = true
		}
	}
	named := g.namedAsValue()
	trans := make([]int32, len(g.Symbols)+1)
	has := make([]bool, len(g.Symbols)+1)
	for i := range g.Reach {
		r := &g.Reach[i]
		if r.SymbolID >= 1 && int(r.SymbolID) < len(trans) {
			trans[r.SymbolID], has[r.SymbolID] = r.NTransitive, true
		}
	}
	var rows [][]fval
	for _, s := range g.dynFns(p) {
		if s.IsStatic != 0 || s.FanIn != 0 || addr[s.ID] || named(s) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.Sloc), fI32(s.NCalls),
			fOptInt(trans[s.ID], has[s.ID]), fStr(g.at(s))))
	}
	dynSort(rows, dynDesc(1))
	return cols, limit(rows, p)
}

func dynHiddenCoupling(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"f1", "f2", "shared", "mod1", "mod2"}
	in := g.edgeCSR(true)
	shared := map[[2]int32]int32{}
	var callers []int32
	for id := int32(1); int(id) <= len(g.Symbols); id++ {
		callers = callers[:0]
		for _, j := range in.of(id) {
			c := g.sym(g.Edges[j].CallerID)
			if c != nil && c.ModuleID > 0 {
				callers = append(callers, c.ID)
			}
		}
		slices.Sort(callers)
		callers = slices.Compact(callers)
		if len(callers) < 2 {
			continue
		}
		for x := 0; x < len(callers); x++ {
			a := g.sym(callers[x])
			for y := x + 1; y < len(callers); y++ {
				if b := g.sym(callers[y]); a.ModuleID != b.ModuleID {
					shared[[2]int32{a.ID, b.ID}]++
				}
			}
		}
	}
	var rows [][]fval
	for k, n := range shared {
		if n < 4 {
			continue
		}
		a, b := g.sym(k[0]), g.sym(k[1])
		m1, m2 := g.modName(a.ModuleID), g.modName(b.ModuleID)
		if !likeMatch(m1, p.mod) && !likeMatch(m2, p.mod) {
			continue
		}
		rows = append(rows, mkrow(fStr(a.Name()), fStr(b.Name()), fI32(n), fStr(m1), fStr(m2)))
	}
	dynSort(rows, func(a, b []fval) int {
		if d := dynCmpI(b[2].i, a[2].i); d != 0 {
			return d
		}
		if d := strings.Compare(a[0].s, b[0].s); d != 0 {
			return d
		}
		return strings.Compare(a[1].s, b[1].s)
	})
	return cols, limit(rows, p)
}

func dynInstability(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "afferent", "efferent", "instability_pct", "distance_pct"}
	n := len(g.Modules) + 1
	ce, ca := make([]int32, n), make([]int32, n)
	for k := range g.dynModEdges() {
		ce[k[0]]++
		ca[k[1]]++
	}
	var rows [][]fval
	for m := 1; m < n; m++ {
		name := g.modName(int32(m))
		if !likeMatch(name, p.mod) {
			continue
		}
		inst, dist := 0.0, 0.0
		if tot := ca[m] + ce[m]; tot > 0 {
			pct := 100.0 * float64(ce[m]) / float64(tot)
			inst, dist = math.Round(pct), math.Round(math.Abs(pct-100.0))
		}
		rows = append(rows, mkrow(fStr(name), fI32(ca[m]), fI32(ce[m]),
			fInt(int(inst)), fInt(int(dist))))
	}
	dynSort(rows, func(a, b []fval) int {
		if d := dynCmpI(a[4].i, b[4].i); d != 0 {
			return d
		}
		if d := dynCmpI(a[3].i, b[3].i); d != 0 {
			return d
		}
		return strings.Compare(a[0].s, b[0].s)
	})
	return cols, limit(rows, p)
}

func dynMarkerClusters(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"module", "markers", "todo", "fixme", "hack", "files"}
	type agg struct {
		all, todo, fixme, hack int32
		files                  map[int32]bool
	}
	by := map[int32]*agg{}
	for i := range g.Markers {
		mk := &g.Markers[i]
		if mk.FileID < 1 || int(mk.FileID) > len(g.Files) {
			continue
		}
		mod := g.Files[mk.FileID-1].ModuleID
		if !likeMatch(g.modName(mod), p.mod) {
			continue
		}
		a := by[mod]
		if a == nil {
			a = &agg{files: map[int32]bool{}}
			by[mod] = a
		}
		a.all++
		switch mk.Kind() {
		case "TODO":
			a.todo++
		case "FIXME":
			a.fixme++
		case "HACK":
			a.hack++
		}
		a.files[mk.FileID] = true
	}
	var rows [][]fval
	for mod, a := range by {
		name := g.modName(mod)
		if name == "" {
			name = "(no module)"
		}
		rows = append(rows, mkrow(fStr(name), fI32(a.all), fI32(a.todo), fI32(a.fixme),
			fI32(a.hack), fInt(len(a.files))))
	}
	dynSort(rows, func(a, b []fval) int {
		if d := dynCmpI(b[1].i, a[1].i); d != 0 {
			return d
		}
		return strings.Compare(a[0].s, b[0].s)
	})
	return cols, limit(rows, p)
}

// A resource taken with `take` and released with `give`: the rows where the
// function takes more than it gives back. skipRet drops functions whose return
// type shows the resource is handed to the caller.
func dynPairRows(g *Graph, p params, take, give func(string) bool, skipRet string,
	withRet bool) [][]fval {
	api := g.dynAPI()
	var rows [][]fval
	for _, s := range g.dynFns(p) {
		t := api.sum(s.ID, take)
		if t == 0 {
			continue
		}
		f := api.sum(s.ID, give)
		if f >= t || (skipRet != "" && strings.Contains(s.ReturnType(), skipRet)) {
			continue
		}
		if withRet {
			rows = append(rows, mkrow(fStr(s.Name()), fI32(t), fI32(f), fI32(s.NReturns),
				fStr(s.ReturnType()), fStr(g.at(s))))
		} else {
			rows = append(rows, mkrow(fStr(s.Name()), fI32(t), fI32(f), fI32(s.NReturns),
				fStr(g.at(s))))
		}
	}
	return rows
}

func dynGapThenReturns(a, b []fval) int {
	if d := dynCmpI(b[1].i-b[2].i, a[1].i-a[2].i); d != 0 {
		return d
	}
	return dynCmpI(b[3].i, a[3].i)
}

func dynCStringLeak(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "to_cstring", "free_cstring", "n_returns", "return_type", "at"}
	rows := dynPairRows(g, p, dynOneOf("JS_ToCString", "JS_ToCStringLen"),
		dynOneOf("JS_FreeCString"), "const char", true)
	dynSort(rows, dynGapThenReturns)
	return cols, limit(rows, p)
}

func dynAtomLeak(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "new_atom", "free_atom", "n_returns", "return_type", "at"}
	rows := dynPairRows(g, p, dynPrefix("JS_NewAtom"), dynOneOf("JS_FreeAtom"), "JSAtom", true)
	dynSort(rows, dynGapThenReturns)
	return cols, limit(rows, p)
}

func dynPropEnumLeak(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "get_props", "free_props", "n_returns", "at"}
	rows := dynPairRows(g, p,
		dynOneOf("JS_GetPropertyNames", "JS_GetOwnPropertyNames", "JS_GetPropertyEnum"),
		dynOneOf("JS_FreePropertyEnum"), "", false)
	dynSort(rows, dynDesc(1, 3))
	return cols, limit(rows, p)
}

// Functions that call something in `call` and nothing in `guard`.
func dynUnguardedRows(g *Graph, p params, call, guard func(string) bool) [][]fval {
	api := g.dynAPI()
	var rows [][]fval
	for _, s := range g.dynFns(p) {
		n := api.sum(s.ID, call)
		if n == 0 || api.sum(s.ID, guard) != 0 {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(n), fI32(s.NReturns), fStr(g.at(s))))
	}
	dynSort(rows, dynDesc(1, 2))
	return rows
}

func dynUncheckedJSCall(g *Graph, p params) ([]string, [][]fval) {
	rows := dynUnguardedRows(g, p,
		dynOneOf("JS_Call", "JS_CallConstructor", "JS_Eval", "JS_EvalThis"),
		dynOneOf("JS_IsException", "JS_GetException"))
	return []string{"name", "js_calls", "n_returns", "at"}, limit(rows, p)
}

func dynOpaqueUnvalidated(g *Graph, p params) ([]string, [][]fval) {
	rows := dynUnguardedRows(g, p, dynOneOf("JS_GetOpaque"), dynPrefix("JS_Throw"))
	return []string{"name", "get_opaque_calls", "n_returns", "at"}, limit(rows, p)
}

func dynDivergentClones(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"f1", "f2", "sloc", "cyclo", "params", "at1", "at2"}
	var rows [][]fval
	for _, grp := range g.dynCloneGroups() {
		for x := 0; x < len(grp); x++ {
			a := grp[x]
			if !likeMatch(g.modName(a.ModuleID), p.mod) {
				continue
			}
			for y := x + 1; y < len(grp); y++ {
				if b := grp[y]; a.FileID != b.FileID {
					rows = append(rows, mkrow(fStr(a.Name()), fStr(b.Name()), fI32(a.Sloc),
						fI32(a.Cyclomatic), fI32(a.NParams), fStr(g.at(a)), fStr(g.at(b))))
				}
			}
		}
	}
	dynSort(rows, func(a, b []fval) int {
		if d := dynDesc(2, 3)(a, b); d != 0 {
			return d
		}
		if d := strings.Compare(a[5].s, b[5].s); d != 0 {
			return d
		}
		return strings.Compare(a[6].s, b[6].s)
	})
	return cols, limit(rows, p)
}

func dynDeepNesting(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "nesting", "branches", "cyclo", "sloc", "at"}
	var rows [][]fval
	for _, s := range g.dynFns(p) {
		if s.MaxNesting < 5 && s.NBranches < 20 {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.MaxNesting), fI32(s.NBranches),
			fI32(s.Cyclomatic), fI32(s.Sloc), fStr(g.at(s))))
	}
	dynSort(rows, dynDesc(1, 3))
	return cols, limit(rows, p)
}

func dynStackPressure(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "locals", "sizeofs", "nesting", "sloc", "at"}
	var rows [][]fval
	for _, s := range g.dynFns(p) {
		if s.NLocals < 15 || s.NSizeof < 4 {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fI32(s.NLocals), fI32(s.NSizeof),
			fI32(s.MaxNesting), fI32(s.Sloc), fStr(g.at(s))))
	}
	dynSort(rows, dynDesc(1, 2))
	return cols, limit(rows, p)
}

// ------------------------------------------------------------------ metrics

func dynFrontierModules(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"module", "frontier_fns", "io", "mem", "alloc", "fnptr_calls", "unresolved"}
	t := g.dynTainted()
	type agg struct{ fns, io, mem, alloc, fnptr, unres int32 }
	by := map[int32]*agg{}
	for _, s := range g.Symbols {
		if !t[s.ID] || s.ModuleID <= 0 || !likeMatch(g.modName(s.ModuleID), p.mod) {
			continue
		}
		a := by[s.ModuleID]
		if a == nil {
			a = &agg{}
			by[s.ModuleID] = a
		}
		a.fns++
		a.io += s.NIO
		a.mem += s.NMemory
		a.alloc += s.NAlloc
		a.fnptr += g.rv(s).NFnptrCalls
		a.unres += s.NUnresolvedCalls
	}
	var rows [][]fval
	for mod, a := range by {
		rows = append(rows, mkrow(fStr(g.modName(mod)), fI32(a.fns), fI32(a.io), fI32(a.mem),
			fI32(a.alloc), fI32(a.fnptr), fI32(a.unres)))
	}
	dynSort(rows, func(a, b []fval) int {
		if d := dynDesc(1, 3)(a, b); d != 0 {
			return d
		}
		return strings.Compare(a[0].s, b[0].s)
	})
	return cols, limit(rows, p)
}

func dynNativeAPIDensity(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"module", "fns", "exported", "fnptr_calls", "js_api_calls"}
	api := g.dynAPI()
	any := func(string) bool { return true }
	type agg struct{ fns, exported, fnptr, js int32 }
	by := map[int32]*agg{}
	for _, s := range g.dynFns(p) {
		if s.ModuleID <= 0 {
			continue
		}
		a := by[s.ModuleID]
		if a == nil {
			a = &agg{}
			by[s.ModuleID] = a
		}
		a.fns++
		if s.IsStatic == 0 {
			a.exported++
		}
		a.fnptr += g.rv(s).NFnptrCalls
		a.js += api.sum(s.ID, any)
	}
	var rows [][]fval
	for mod, a := range by {
		rows = append(rows, mkrow(fStr(g.modName(mod)), fI32(a.fns), fI32(a.exported),
			fI32(a.fnptr), fI32(a.js)))
	}
	dynSort(rows, func(a, b []fval) int {
		if d := dynDesc(4)(a, b); d != 0 {
			return d
		}
		return strings.Compare(a[0].s, b[0].s)
	})
	return cols, limit(rows, p)
}

func dynRefactoringHotspots(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"module", "fns", "god_fns", "deep_nesting", "total_cyclo", "avg_cyclo"}
	type agg struct{ fns, god, deep, cyclo int32 }
	by := map[int32]*agg{}
	for _, s := range g.dynFns(p) {
		a := by[s.ModuleID]
		if a == nil {
			a = &agg{}
			by[s.ModuleID] = a
		}
		a.fns++
		if s.Sloc >= 200 && s.Cyclomatic >= 20 {
			a.god++
		}
		if s.MaxNesting >= 5 {
			a.deep++
		}
		a.cyclo += s.Cyclomatic
	}
	var rows [][]fval
	for mod, a := range by {
		name := g.modName(mod)
		if name == "" {
			name = "(root)"
		}
		rows = append(rows, mkrow(fStr(name), fI32(a.fns), fI32(a.god), fI32(a.deep),
			fI32(a.cyclo), fF(math.Round(10*float64(a.cyclo)/float64(a.fns))/10)))
	}
	dynSort(rows, func(a, b []fval) int {
		if d := dynDesc(2, 4)(a, b); d != 0 {
			return d
		}
		return strings.Compare(a[0].s, b[0].s)
	})
	return cols, limit(rows, p)
}

func dynCloneDensity(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"module", "clone_pairs", "cloned_fns"}
	type agg struct {
		pairs int32
		fns   map[int32]bool
	}
	by := map[int32]*agg{}
	for _, grp := range g.dynCloneGroups() {
		for x := 0; x < len(grp); x++ {
			a := grp[x]
			if !likeMatch(g.modName(a.ModuleID), p.mod) {
				continue
			}
			for y := x + 1; y < len(grp); y++ {
				if a.FileID == grp[y].FileID {
					continue
				}
				v := by[a.ModuleID]
				if v == nil {
					v = &agg{fns: map[int32]bool{}}
					by[a.ModuleID] = v
				}
				v.pairs++
				v.fns[a.ID] = true
			}
		}
	}
	var rows [][]fval
	for mod, a := range by {
		name := g.modName(mod)
		if name == "" {
			name = "(root)"
		}
		rows = append(rows, mkrow(fStr(name), fI32(a.pairs), fInt(len(a.fns))))
	}
	dynSort(rows, func(a, b []fval) int {
		if d := dynDesc(1)(a, b); d != 0 {
			return d
		}
		return strings.Compare(a[0].s, b[0].s)
	})
	return cols, limit(rows, p)
}

// ---------------------------------------------------------------- catalogue

var dynQueries = []qentry{
	{"untrusted-reach", "Transitive closure: every function attacker bytes can reach",
		"ANSWERS the real attack surface: not just the functions that CALL read/\n     recv, but everything downstream of them through resolved edges.\nACT this is the ranking to review before shipping a parser -- a bounds\n     bug in any row here is a reachable memory-safety bug. The fuzz\n     targets must cover the files named in this list (fuzz_parsers,\n     fuzz_stdlib exist because of it).\nMISLEADS fnptr edges are invisible: the aio/reactor dispatch and every\n     JS_CFUNC-registered native are reached through function pointers,\n     so this is a FLOOR, not the frontier. Compare against\n     graph-blindspots before budgeting from it.",
		dynUntrustedReach},
	{"refcount-leak-shape", "JSValue refcount drift: more JS_New*/JS_DupValue than JS_FreeValue",
		"ANSWERS the CLAUDE.md law -- 'every value from an API call is owned; free\n     once on every path' -- as a measurement, not a habit. Positive drift\n     with early returns and a non-JSValue return type is the leak shape:\n     a value created and then abandoned on an error path.\nACT a function that RETURNS a JSValue is exempt by design (the caller\n     owns it now); a void/int function with drift > 0 and returns > 1\n     needs a free on the paths that do not hand the value out.\nMISLEADS counts are per function, not per path: a function that creates\n     two values and frees one on every path can show 0 drift and still\n     leak. Cross-check each row against its return statements.",
		dynRefcountLeak},
	{"over-free-shape", "More JS_FreeValue than JS_New*/JS_DupValue: double-free risk",
		"ANSWERS the mirror of refcount-leak-shape: a function that frees more\n     than it demonstrably creates inside itself. Two honest causes: the\n     value was created OUTSIDE (caller-owned, must not free at all) or\n     the creator is behind a macro the text pass cannot see.\nACT for each row: read the ownership comment. A negative drift on a\n     function that also creates the value on one path is the double-\n     free shape.\nMISLEADS JS_FreeValueRT vs JS_FreeValue are both counted; a function\n     freeing a borrowed JSValueConst is exactly the bug this exists to\n     find, so treat every negative row as a review item.",
		dynOverFree},
	{"define-vs-set", "Every JS_SetProperty*/JS_SetPropertyStr caller, frontier-flagged",
		"ANSWERS where a peer-supplied key can retarget a prototype: JS_SetProperty\n     and JS_SetPropertyStr walk the prototype chain, so a key of\n     '__proto__' re-points the object. The repo rule is DEFINE for every\n     decoded/mapped key. The on_frontier column comes from the taint\n     closure; fnptr blindness undercounts it (registered natives are\n     invisible), so 0 does not clear a row.\nACT walk the rows with the most calls first; a Set with a key built\n     from input bytes (JSON keys, header names, map entries) must be\n     JS_DefinePropertyValue. Internal fixed keys (error objects, option\n     structs) are fine with Set.\nMISLEADS the pass counts names, not arguments: a SetPropertyStr with a\n     string LITERAL argument is a false positive for the prototype\n     attack but still worth the walk (a later edit may hoist the key).",
		dynDefineVsSet},
	{"throwing-path-hygiene", "JSValue-returning functions with several returns and NO JS_Throw",
		"ANSWERS where an error path returns a non-exception value: a native\n     method that returns JS_UNDEFINED on failure instead of throwing\n     reads as success to the caller and silently drops the failure.\nACT read the return statements; every failure path should throw a named\n     error (the repo's distinct-refusal rule), never return quietly.\nMISLEADS many honest functions return JS_EXCEPTION after a nested call\n     already threw, with no JS_Throw of their own -- the count can be\n     zero while every path is correct.",
		dynThrowingPath},
	{"lazy-static-races", "Mutable file-scope state: lazily-initialised statics and shared globals",
		"ANSWERS the data-race class the codebase has hit before: a non-const\n     static without an initialiser is a lazily-initialised static, and\n     that is a data race even when every thread writes identical bytes.\n     A non-static mutable global is shared state with no owner.\nACT prefer a compile-time const table; where a runtime probe is genuinely\n     required, make the flag atomic.\nMISLEADS a global only ever touched before thread creation is fine -- the\n     scanner cannot see WHEN the write happens.",
		dynLazyStatic},
	{"module-cycle-pairs", "Mutually-reachable module pairs: layering violations",
		"ANSWERS which modules cannot be reasoned about in isolation because\n     each reaches the other through resolved calls. A module dependency\n     cycle is the one smell that blocks architectural reasoning outright.\nACT break the most-loaded edge of the cycle (the pair with the highest\n     out/in counts); a k-cycle of three shows as three rows here.\nMISLEADS pairs are transitive, not direct: A->B->C->A reports (A,B),\n     (A,C), (B,C). Each pair still names a real circular dependency.\n     fnptr edges are invisible, so dispatch-table back-edges do not show.",
		dynModuleCycles},
	{"feature-envy", "Functions whose cross-module calls dominate their own-module calls",
		"ANSWERS misplaced helpers: a function that calls into ONE foreign module\n     far more than into its own is implemented on the wrong side of the\n     boundary (the JDeodorant move-method shape).\nACT move the function into the envied module, or move the missing helper\n     into the envious one; rank by cross_edges.\nMISLEADS header-only helpers and shared utility modules (dyna-nat, cutils)\n     are called by design; a function that envies a utility module is fine.\n     Only module pairs that carry real domain logic are findings.",
		dynFeatureEnvy},
	{"shotgun-surgery", "Callees with fan-in spread over the most files",
		"ANSWERS the functions whose signature or contract change ripples into the\n     most translation units: the static proxy for shotgun surgery.\nACT wrap in an accessor or push into the owning module BEFORE the next\n     contract change; each row is a change-amplifier.\nMISLEADS fan-in breadth is not churn: a stable, never-changing helper\n     with 20 call sites is a good abstraction, not a smell. Judge by\n     whether the contract is still evolving (markers nearby).",
		dynShotgun},
	{"god-functions", "Huge AND complex AND central: the waiver-audit list",
		"ANSWERS which functions are simultaneously large, deep and depended-upon.\n     The repo rule: some functions are CORRECTLY enormous (a bytecode\n     dispatch loop, GC machinery); everything else is an Extract-Function\n     target, and the test is whether a sub-pass extracts without an asm diff.\nACT exempt the dispatch loops BY NAME, then rank the rest by cyclomatic\n     x sloc and decompose.\nMISLEADS a 500-line switch is often fine; a 300-line if-chain is not. The\n     scanner cannot tell them apart -- n_switch is the discriminator.",
		dynGodFunctions},
	{"middle-men", "Exported one-line forwarders called by many",
		"ANSWERS pure pass-throughs on the API surface: short, trivial, widely\n     called, and forwarding to exactly one callee.\nACT public ABI wrappers (JS_* forwarding to static internals) are\n     intentional middle men and exempt BY DESIGN; the finding is only\n     actionable when caller and callee share a module -- then the wrapper\n     adds a hop without a boundary.\nMISLEADS a wrapper that adds a check, a conversion or a log line is not\n     a middle man; this list over-includes on purpose.",
		dynMiddleMen},
	{"dead-exported-api", "Non-static functions nothing in the tree references",
		"ANSWERS the exported surface with no internal caller, no address taken,\n     and no transitive caller: either JS-facing only (registered through\n     JS_CFUNC_DEF lists the scanner cannot see) or genuinely dead.\nACT one grep of the registration tables per row settles it; dead rows are\n     free untrusted-surface removal.\nMISLEADS JS-facing natives are the expected majority -- the query exists\n     to make the few non-registered rows findable.",
		dynDeadExported},
	{"hidden-coupling", "Cross-module function pairs sharing the most callees",
		"ANSWERS implicit coupling: two functions in different modules that call\n     the same set of helpers change together when any of those helpers\n     changes contract -- invisible in the module graph, visible in the\n     call graph (the static proxy for logical coupling).\nACT pairs that hand-roll the same call sequence (JS_NewObject + SetProperty\n     runs) are both a duplication and a cascade risk: extract the shared\n     helper once.\nMISLEADS shared UTILITY callees (memcpy, JS_FreeValue) create spurious pairs;\n     require shared >= 4 so one common helper does not read as coupling.",
		dynHiddenCoupling},
	{"instability-distance", "Martin's instability I and distance D per module",
		"ANSWERS which modules are stable (I~0: everyone depends on them) and which\n     are unstable (I~1: they depend on everyone). With no abstract types\n     in C, D = |1 - I|: the D~0.5 band is the danger zone -- a module with\n     heavy in- AND out-edges is the change-preventer hub.\nACT I=0 with D=1 (the core runtime) means every change to it is shotgun\n     surgery; its only relief is an interface boundary (fnptr table). D~0.5\n     modules need a split, not more edits.\nMISLEADS edge counts are static call sites; a module reached only through\n     function pointers reads stabler than it is.",
		dynInstability},
	{"marker-clusters", "TODO/FIXME/HACK density per module: where the rot lives",
		"ANSWERS which modules accumulate the deferred-work markers; density is\n     the honest 'this is not finished' signal, and it concentrates where\n     the risky parsers live.\nACT rank by markers BEFORE starting new work in a module: the existing\n     debt is part of the effort estimate.\nMISLEADS a stale marker stays forever; density alone does not say how old\n     the debt is.",
		dynMarkerClusters},
	{"cstring-leak-risk", "JS_ToCString/JS_ToCStringLen without matching JS_FreeCString on exit",
		"ANSWERS where C string conversion buffers allocated by the engine\n     are leaked on early return paths or missing cleanup. In QuickJS,\n     every `JS_ToCString*` MUST be paired with `JS_FreeCString`.\nACT audit the non-paired return paths; ensure `JS_FreeCString` is\n     invoked before every return or error exit, or the pointer is\n     explicitly passed to an owned out-parameter (e.g. `powned`).\nMISLEADS functions that return `const char *` or hand ownership to an\n     out-parameter (e.g. `dyn_crypto_data`) show a positive difference\n     by design. Check whether the pointer escapes.",
		dynCStringLeak},
	{"atom-leak-risk", "JS_NewAtom* calls with fewer JS_FreeAtom releases",
		"ANSWERS functions that allocate runtime atom table entries without\n     releasing them. Atoms are reference-counted in QuickJS and\n     leaked atoms stay permanently in the runtime atom table.\nACT ensure every `JS_NewAtom*` has a corresponding `JS_FreeAtom` on\n     error paths, or is passed to an API function that adopts/defines\n     the property.\nMISLEADS atoms passed to property lookup/definition APIs that consume\n     or store them internally may not need local `JS_FreeAtom`.",
		dynAtomLeak},
	{"prop-enum-leak-risk", "Property enumeration without JS_FreePropertyEnum cleanup",
		"ANSWERS functions calling `JS_GetPropertyNames` or `JS_GetOwnPropertyNames`\n     that fail to call `JS_FreePropertyEnum`. Calling raw `js_free` on\n     the `JSPropertyEnum` array frees the array buffer but LEAKS all\n     contained `JSAtom` entries.\nACT replace manual `free`/`js_free` with `JS_FreePropertyEnum(ctx, tab, ntab)`\n     on all error and success exit paths.\nMISLEADS functions that manually loop over `tab[i].atom` with `JS_FreeAtom`\n     before calling `js_free` will appear here; prefer `JS_FreePropertyEnum`.",
		dynPropEnumLeak},
	{"unchecked-js-call", "JS_Call / JS_Eval invocations without JS_IsException check",
		"ANSWERS C functions executing JavaScript code (`JS_Call`, `JS_Eval`)\n     that never check if the execution raised a pending JS exception.\n     Unhandled exceptions stay on the JSContext and corrupt subsequent\n     operations.\nACT check `if (JS_IsException(res))` immediately after the call and\n     either handle or propagate it to the caller.\nMISLEADS fire-and-forget logging or cleanup callbacks that explicitly\n     clear exceptions via other helpers.",
		dynUncheckedJSCall},
	{"opaque-unvalidated", "JS_GetOpaque callers without error checking or JS_Throw",
		"ANSWERS functions calling `JS_GetOpaque` (which returns NULL without\n     throwing on class mismatch) and not invoking any `JS_Throw*`.\n     Dereferencing the returned pointer directly causes a NULL crash.\nACT use `JS_GetOpaque2` (which throws a TypeError on mismatch) or\n     check `if (!ptr) return -1;` before dereferencing struct fields.\nMISLEADS helper functions whose caller already validated the class ID.",
		dynOpaqueUnvalidated},
	{"divergent-clones", "Near-duplicate functions across different files (structural clones)",
		"ANSWERS pairs of functions across different files with identical\n     sloc, cyclomatic complexity, parameter count, and branch count:\n     the static proxy for copy-paste code drift.\nACT factor the duplicated logic into a shared helper in `dyna-nat.h`\n     or `cutils.h`.\nMISLEADS multi-architecture SIMD kernel variants (SSE4.2 vs NEON vs\n     AVX2) intentionally share exact structural signatures by design.",
		dynDivergentClones},
	{"deeply-nested-control-flow", "Functions with deep nesting (depth >= 5) or high branch count (>= 20)",
		"ANSWERS control-flow hotspots that are hard to read and test.\nACT apply early returns, guard clauses, or extract inner loop bodies;\n     for parsing state machines, evaluate table-driven dispatch (BOSCC).\nMISLEADS complex tokenizer/parser switch cases may legitimately have\n     many branches; check `n_switch` vs `max_nesting`.",
		dynDeepNesting},
	{"stack-pressure-risk", "Functions with high local variable count (>= 15) and multiple sizeofs",
		"ANSWERS functions with heavy stack footprints from many local variables\n     and local buffers, risking stack overflow in recursive loops or\n     constrained thread stacks.\nACT move large stack buffers into heap scratch structures or arena\n     allocators; reduce scope of temporary locals.\nMISLEADS compiler register allocation can optimize non-aliased scalar\n     locals into registers; arrays and structs are the real risk.",
		dynStackPressure},
}

var dynMetrics = []qentry{
	{"frontier-modules", "Per-module untrusted-frontier dashboard",
		"ANSWERS where the attack surface concentrates by module: functions\n     reachable from io, with memory/alloc hazard counts and the fnptr\n     blindness that makes every number a floor.\nACT budget security review by frontier_fns, then by mem+alloc; every\n     parser module should appear here and its fuzz target should exist.\nMISLEADS io roots only count RAW descriptor calls in the module's own\n     functions -- a parser fed by a caller's read is reached through the\n     graph, so it IS counted here, but its io column stays 0.",
		dynFrontierModules},
	{"native-api-density", "Per-module JS API call density: who owns the runtime boundary",
		"ANSWERS which modules are the heaviest consumers of the JS runtime API\n     (JS_New*/SetProperty/FreeValue family): the modules whose native\n     methods do the most object plumbing are where the refcount and\n     prototype-pollution bugs concentrate.\nACT cross-reference with refcount-leak-shape and define-vs-set per module.\nMISLEADS counts include JS_Throw* (every native method throws); the\n     ranking still holds because error paths are where the work is.",
		dynNativeAPIDensity},
	{"refactoring-hotspots", "Per-module refactoring hotspots (god functions, deep nesting, complexity)",
		"ANSWERS which modules have the highest concentration of complex,\n     deeply nested, and oversized functions that need decomposition.\nACT prioritize refactoring sprints by god_fns count and total_cyclo.\nMISLEADS core interpreter modules legitimately have high complexity.",
		dynRefactoringHotspots},
	{"clone-density", "Per-module structural clone density",
		"ANSWERS which modules contain the most duplicate function structures\n     that could be consolidated into common utility helpers.\nACT extract shared patterns into `dyna-nat.h` or module-internal helpers.\nMISLEADS SIMD architecture variants naturally appear as clone pairs.",
		dynCloneDensity},
}

// DYN PROJECT: shape queries. Each one is the SHAPE of a defect this tree
// actually shipped, written down so the next instance is a query result and
// not a week. They read the raw text of a function body (dynAPITable.body),
// because every one of them is about how a call is WRITTEN -- its arguments,
// what is done with its result -- which the call graph does not record.
//
// A shape query earns its place by firing on the bug it is modelled on:
// bench/codegraph/shapes_test.sh rebuilds each historical defect from a small
// fixture and requires the row. A shape that cannot fail proves nothing.
func init() {
	queries = append(queries, dynShapeQueries...)
}

// ------------------------------------------------------------------ helpers

// dynArgs splits the argument list that opens at body[open] == '(' on its
// top-level commas. end is the offset one past the closing parenthesis.
func dynArgs(body []byte, open int) (args []string, end int) {
	depth, start := 0, open+1
	for i := open; i < len(body); i++ {
		switch c := body[i]; c {
		case '"', '\'':
			q := c
			i++
			for i < len(body) && body[i] != q {
				if body[i] == '\\' {
					i++
				}
				i++
			}
		case '(', '[', '{':
			depth++
		case ')', ']', '}':
			depth--
			if depth == 0 {
				if a := strings.TrimSpace(string(body[start:i])); a != "" || len(args) > 0 {
					args = append(args, a)
				}
				return args, i + 1
			}
		case ',':
			if depth == 1 {
				args = append(args, strings.TrimSpace(string(body[start:i])))
				start = i + 1
			}
		}
	}
	return args, len(body)
}

func dynLineAt(t *dynAPITable, id int32, off int) int32 {
	b := t.body[id]
	if off > len(b) {
		off = len(b)
	}
	n := int32(0)
	for _, c := range b[:off] {
		if c == '\n' {
			n++
		}
	}
	return t.bodyLine[id] + n
}

func (g *Graph) dynSite(t *dynAPITable, s *Symbol, off int) string {
	f := g.fileOf(s.ID)
	if f == nil {
		return g.at(s)
	}
	return f.Path() + ":" + strconv.Itoa(int(dynLineAt(t, s.ID, off)))
}

// every function of the module filter that has a body, in symbol order
func (g *Graph) dynBodies(p params, fn func(s *Symbol, body []byte)) *dynAPITable {
	t := g.dynAPI()
	for _, s := range g.dynFns(p) {
		if b := t.body[s.ID]; len(b) > 0 {
			fn(s, b)
		}
	}
	return t
}

func dynBool(b bool) fval {
	if b {
		return fInt(1)
	}
	return fInt(0)
}

func dynClip(s string, n int) string {
	s = strings.Join(strings.Fields(s), " ")
	if len(s) > n {
		return s[:n]
	}
	return s
}

// ------------------------------------------------------------------ queries

var (
	dynReJSCallAssign = regexp.MustCompile(`\b(\w+)\s*=\s*JS_(?:Call|CallConstructor|Eval|EvalThis|Invoke)\s*\(`)
	dynReJSCallFreed  = regexp.MustCompile(`JS_FreeValue(?:RT)?\s*\([^;()]*,\s*JS_(?:Call|CallConstructor|Eval|EvalThis|Invoke)\s*\(`)
	dynReExcHandled   = regexp.MustCompile(`\b(?:JS_GetException|JS_HasException|dyn_net_handler_result|dyn_net_dns_callback_result|dyn_call_drop|js_std_dump_error|dyn_\w*(?:swallow|unhandled|report_exc)\w*)\s*\(`)
	dynReResolver     = regexp.MustCompile(`resolve|reject|resolving_funcs|settle`)
)

// A value returned by JS_Call that is JS_EXCEPTION is not owned -- freeing it is
// a no-op -- but the exception it stands for IS still pending on the context.
func dynPendingExceptionDropped(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "dropped", "resolver_only", "first_callee", "site", "at"}
	var rows [][]fval
	t := g.dynBodies(p, nil2)
	for _, s := range g.dynFns(p) {
		body := t.body[s.ID]
		if len(body) == 0 || dynReExcHandled.Match(body) {
			continue
		}
		dropped, resolvers, first, firstOff := 0, 0, "", -1
		note := func(open, off int) {
			args, _ := dynArgs(body, open)
			callee := ""
			if len(args) > 1 {
				callee = args[1]
			}
			dropped++
			if dynReResolver.MatchString(callee) {
				resolvers++
			}
			if firstOff < 0 {
				first, firstOff = callee, off
			}
		}
		for _, m := range dynReJSCallAssign.FindAllSubmatchIndex(body, -1) {
			v := regexp.QuoteMeta(string(body[m[2]:m[3]]))
			rest := body[m[1]:]
			checked, _ := regexp.Match(`JS_IsException\s*\(\s*`+v+`\s*\)|\breturn\s+`+v+`\s*;|=\s*`+v+`\s*;`, rest)
			freed, _ := regexp.Match(`JS_FreeValue(?:RT)?\s*\([^;()]*,\s*`+v+`\s*\)`, rest)
			if freed && !checked {
				note(m[1]-1, m[0])
			}
		}
		for _, m := range dynReJSCallFreed.FindAllIndex(body, -1) {
			note(m[1]-1, m[0])
		}
		if dropped == 0 {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fInt(dropped), dynBool(resolvers == dropped),
			fStr(dynClip(first, 40)), fStr(g.dynSite(t, s, firstOff)), fStr(g.at(s))))
	}
	dynSort(rows, func(a, b []fval) int {
		if d := dynCmpI(a[2].i, b[2].i); d != 0 {
			return d
		}
		return dynCmpI(b[1].i, a[1].i)
	})
	return cols, limit(rows, p)
}

func nil2(*Symbol, []byte) {}

var (
	dynReRoundCall = regexp.MustCompile(`-\s*(?:floor|trunc|rint|round|nearbyint|ceil)[fl]?\s*\(`)
	dynReFma       = regexp.MustCompile(`\bfmal?f?\s*\(`)
)

// x - floor(x / y) * y is an exact remainder only when the multiply and the
// subtract are one fused operation. arm64 compilers fuse it; baseline x86-64
// has no FMA and rounds twice.
func dynFmaDependent(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "sites", "has_fma_call", "expr", "site", "at"}
	var rows [][]fval
	t := g.dynBodies(p, nil2)
	for _, s := range g.dynFns(p) {
		body := t.body[s.ID]
		n, first, expr := 0, -1, ""
		for _, m := range dynReRoundCall.FindAllIndex(body, -1) {
			_, end := dynArgs(body, m[1]-1)
			k := end
			for k < len(body) && (body[k] == ' ' || body[k] == '\t' || body[k] == '\n') {
				k++
			}
			if k >= len(body) || body[k] != '*' {
				continue
			}
			if n == 0 {
				first = m[0]
				lo := m[0] - 24
				if lo < 0 {
					lo = 0
				}
				hi := k + 12
				if hi > len(body) {
					hi = len(body)
				}
				expr = dynClip(string(body[lo:hi]), 60)
			}
			n++
		}
		if n == 0 {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fInt(n), dynBool(dynReFma.Match(body)),
			fStr(expr), fStr(g.dynSite(t, s, first)), fStr(g.at(s))))
	}
	dynSort(rows, func(a, b []fval) int {
		if d := dynCmpI(a[2].i, b[2].i); d != 0 {
			return d
		}
		return dynCmpI(b[1].i, a[1].i)
	})
	return cols, limit(rows, p)
}

// rows of functions whose body matches re, minus those matching `unless`
func dynGrepRows(g *Graph, p params, re, unless *regexp.Regexp, keep func(*Symbol) bool) [][]fval {
	var rows [][]fval
	t := g.dynBodies(p, nil2)
	for _, s := range g.dynFns(p) {
		body := t.body[s.ID]
		if len(body) == 0 || (keep != nil && !keep(s)) {
			continue
		}
		ms := re.FindAllIndex(body, -1)
		if len(ms) == 0 || (unless != nil && unless.Match(body)) {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fInt(len(ms)),
			fStr(dynClip(string(body[ms[0][0]:ms[0][1]]), 44)),
			fStr(g.dynSite(t, s, ms[0][0])), fStr(g.at(s))))
	}
	dynSort(rows, dynDesc(1))
	return rows
}

var dynGrepCols = []string{"name", "calls", "first", "site", "at"}

var dynReRingInit = regexp.MustCompile(`\bio_uring_queue_init(?:_params)?\s*\(`)

func dynRawRingOpen(g *Graph, p params) ([]string, [][]fval) {
	rows := dynGrepRows(g, p, dynReRingInit, nil,
		func(s *Symbol) bool { return s.Name() != "dyn_uring_ring_open" })
	return dynGrepCols, limit(rows, p)
}

var (
	dynReReactorAcquire = regexp.MustCompile(`\bdyn_net_reactor_acquire\s*\(`)
	dynReSharedNoReact  = regexp.MustCompile(`\bdyn_net_throw_no_reactor\s*\(`)
)

func dynReactorErrorShape(g *Graph, p params) ([]string, [][]fval) {
	rows := dynGrepRows(g, p, dynReReactorAcquire, dynReSharedNoReact, func(s *Symbol) bool {
		n := s.Name()
		return n != "dyn_net_reactor_acquire" && !strings.HasPrefix(n, "dyn_net_reactor_")
	})
	return dynGrepCols, limit(rows, p)
}

var (
	dynReSetStr  = regexp.MustCompile(`\bJS_SetPropertyStr\s*\(`)
	dynReSetAtom = regexp.MustCompile(`\bJS_SetProperty\s*\(`)
)

// The prototype-walking setters, narrowed to the calls whose KEY is not a
// literal: those are the only ones a peer can aim at __proto__.
func dynSetPropertyDynamicKey(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "dynamic_keys", "literal_keys", "on_frontier", "first_key", "site", "at"}
	taint := g.dynTainted()
	var rows [][]fval
	t := g.dynBodies(p, nil2)
	for _, s := range g.dynFns(p) {
		body := t.body[s.ID]
		dyn, lit, first, off := 0, 0, "", -1
		scan := func(re *regexp.Regexp, atom bool) {
			for _, m := range re.FindAllIndex(body, -1) {
				args, _ := dynArgs(body, m[1]-1)
				if len(args) < 3 {
					continue
				}
				key := args[2]
				if (!atom && strings.HasPrefix(key, `"`)) || (atom && strings.HasPrefix(key, "JS_ATOM_")) {
					lit++
					continue
				}
				dyn++
				if off < 0 {
					first, off = key, m[0]
				}
			}
		}
		scan(dynReSetStr, false)
		scan(dynReSetAtom, true)
		if dyn == 0 {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fInt(dyn), fInt(lit), dynBool(taint[s.ID]),
			fStr(dynClip(first, 32)), fStr(g.dynSite(t, s, off)), fStr(g.at(s))))
	}
	dynSort(rows, dynDesc(3, 1))
	return cols, limit(rows, p)
}

var (
	dynReNewTypedArray = regexp.MustCompile(`\bJS_NewTypedArray\s*\(`)
	dynReBufferName    = regexp.MustCompile(`(?i)buf|buffer|^ab$|_ab$|arraybuffer`)
)

// JS_NewTypedArray(ctx, argc, argv, type) mirrors `new Uint8Array(...)`: one
// argument that is a LENGTH is the ordinary allocation, but one argument that
// is an ArrayBuffer silently yields a length-0 view in this engine. Only the
// second is listed: a short call whose first argument is, or was assigned
// from, an array buffer.
func dynTypedArrayShortArgs(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "argc", "argv", "site", "at"}
	var rows [][]fval
	t := g.dynBodies(p, nil2)
	for _, s := range g.dynFns(p) {
		body := t.body[s.ID]
		for _, m := range dynReNewTypedArray.FindAllIndex(body, -1) {
			args, _ := dynArgs(body, m[1]-1)
			if len(args) < 3 || (args[1] != "1" && args[1] != "2") {
				continue
			}
			v := strings.TrimLeft(args[2], "&(")
			v = strings.TrimSpace(strings.TrimPrefix(v, "JSValueConst*)"))
			name := regexp.MustCompile(`^\w+`).FindString(v)
			if name == "" {
				continue
			}
			fromBuffer, _ := regexp.Match(`\b`+regexp.QuoteMeta(name)+`(?:\s*\[\s*0\s*\])?\s*=\s*JS_NewArrayBuffer\w*\s*\(`, body)
			if !fromBuffer && !dynReBufferName.MatchString(name) {
				continue
			}
			rows = append(rows, mkrow(fStr(s.Name()), fStr(args[1]), fStr(dynClip(args[2], 30)),
				fStr(g.dynSite(t, s, m[0])), fStr(g.at(s))))
		}
	}
	return cols, limit(rows, p)
}

var (
	dynReReactorFile = regexp.MustCompile(`^dyna-(?:aio|aio-uring|evloop|http|net|net-[a-z0-9]+|tls|scrape)\.(?:c|inc\.c)$`)
	dynReBlocking    = regexp.MustCompile(`\b(?:sleep|usleep|nanosleep|getaddrinfo|gethostbyname2?|system|popen)\s*\(|\bpoll\s*\(\s*NULL\b|\bwaitpid\s*\([^;]*,\s*0\s*\)`)
)

// A reactor thread that sleeps, or resolves a name synchronously, stalls every
// connection it owns for that long.
func dynReactorBlockingCall(g *Graph, p params) ([]string, [][]fval) {
	rows := dynGrepRows(g, p, dynReBlocking, nil, func(s *Symbol) bool {
		f := g.fileOf(s.ID)
		return f != nil && dynReReactorFile.MatchString(filepath.Base(f.Path()))
	})
	return dynGrepCols, limit(rows, p)
}

var (
	dynReFsync     = regexp.MustCompile(`\bf(?:data)?sync\s*\(`)
	dynReFullFsync = regexp.MustCompile(`\bF_FULLFSYNC\b`)
)

func dynFsyncNotDurable(g *Graph, p params) ([]string, [][]fval) {
	return dynGrepCols, limit(dynGrepRows(g, p, dynReFsync, dynReFullFsync, nil), p)
}

var dynReLocaleNum = regexp.MustCompile(`\b(?:strtod|strtof|strtold|atof)\s*\(|\bv?f?s?scanf\s*\([^;]*%[0-9*]*l?L?[fFeEgGaA]`)

func dynLocaleNumberParse(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "calls", "on_frontier", "first", "site", "at"}
	taint := g.dynTainted()
	var rows [][]fval
	t := g.dynBodies(p, nil2)
	for _, s := range g.dynFns(p) {
		body := t.body[s.ID]
		ms := dynReLocaleNum.FindAllIndex(body, -1)
		if len(ms) == 0 {
			continue
		}
		rows = append(rows, mkrow(fStr(s.Name()), fInt(len(ms)), dynBool(taint[s.ID]),
			fStr(dynClip(string(body[ms[0][0]:ms[0][1]]), 30)),
			fStr(g.dynSite(t, s, ms[0][0])), fStr(g.at(s))))
	}
	dynSort(rows, dynDesc(2, 1))
	return cols, limit(rows, p)
}

var dynReDiscardedArm = regexp.MustCompile(`\(\s*void\s*\)\s*(?:dyn_evloop_(?:add|mod|set_timer)|dyn_aio_(?:recv|recvfrom|accept|connect|listen|tls_start|tls_attach|offload\w*)|dyn_timer_add|dyn_pool_submit\w*|io_uring_register\w*|io_uring_submit\w*|pthread_create|epoll_ctl|kevent|timerfd_settime|eventfd)\s*\(`)

func dynDiscardedArmResult(g *Graph, p params) ([]string, [][]fval) {
	return dynGrepCols, limit(dynGrepRows(g, p, dynReDiscardedArm, nil, nil), p)
}

// ---------------------------------------------------------------- catalogue

var (
	dynReOffload    = regexp.MustCompile(`\bdyn_fiber_offload\s*\(`)
	dynReCtxCall    = regexp.MustCompile(`\b([A-Za-z_]\w*)\s*\(\s*(?:ctx|\w+->ctx)\s*[,)]`)
	dynReOnJS       = regexp.MustCompile(`\bdyn_fiber_onjs\s*\(`)
	dynReEscape     = regexp.MustCompile(`\b(?:return|goto)\b`)
	dynReNetMacro   = regexp.MustCompile(`(?m)^\s*#\s*define\s+\w+\(\.\.\.\)\s*\(dyn_fiber_onjs\(\)`)
	dynReDefineCall = regexp.MustCompile(`(?m)^\s*#\s*define\s+([A-Za-z_]\w*)\(`)
	dynOffloadExempt = map[string]bool{"JS_NewCModule": true, "JS_AddModuleExport": true,
		"JS_AddModuleExportList": true, "JS_SetModuleExportList": true,
		"dyn_register_class": true, "dyn_register_plain_class": true,
		"if": true, "while": true, "for": true, "switch": true, "return": true, "sizeof": true}
)

// A file that moves work onto another thread (dyn_fiber_offload) may only touch
// the JS runtime after hopping back. dyna-ml.c makes that hold by construction:
// every engine call it uses is redefined as a macro that hops back first. This
// lists the calls that take the context and are NOT behind such a macro -- one
// of them inside an offloaded section runs concurrently with the JS thread.
// A file with no such net is held to
// the explicit form instead: offload, pure C, dyn_fiber_onjs(), with no engine
// call and no return or goto in between.
func dynOffloadUnguarded(g *Graph, p params) ([]string, [][]fval) {
	cols := []string{"name", "unguarded", "first_callee", "site", "at"}
	t := g.dynBodies(p, nil2)
	root := dynScanRoot()
	type fileInfo struct {
		offloads bool
		netted   bool
		local    map[string]bool
		wrapped  map[string]bool
		base     string
		src      []byte
	}
	info := map[int32]*fileInfo{}
	fns := g.dynFns(p)
	for _, s := range fns {
		fi := info[s.FileID]
		if fi == nil {
			fi = &fileInfo{local: map[string]bool{}, wrapped: map[string]bool{}}
			info[s.FileID] = fi
			if f := g.fileOf(s.ID); f != nil {
				if src, err := os.ReadFile(filepath.Join(root, filepath.FromSlash(f.Path()))); err == nil {
					for _, m := range dynReDefineCall.FindAllSubmatch(src, -1) {
						fi.wrapped[string(m[1])] = true
					}
					fi.netted = dynReNetMacro.Match(src)
					fi.base, fi.src = filepath.Base(f.Path()), src
				}
			}
		}
		fi.local[s.Name()] = true
		if dynReOffload.Match(t.body[s.ID]) {
			fi.offloads = true
		}
	}
	// a fragment #included by a netted file is compiled AFTER that file's
	// macros, so it inherits the net and the includer's offload status
	for _, fi := range info {
		if fi.netted || !strings.HasSuffix(fi.base, ".inc.c") {
			continue
		}
		inc := []byte("#include \"" + fi.base + "\"")
		for _, host := range info {
			if host.netted && bytes.Contains(host.src, inc) {
				fi.netted, fi.offloads = true, fi.offloads || host.offloads
				for k := range host.wrapped {
					fi.wrapped[k] = true
				}
				for k := range host.local {
					fi.local[k] = true
				}
			}
		}
	}
	var rows [][]fval
	for _, s := range fns {
		fi := info[s.FileID]
		body := t.body[s.ID]
		if fi == nil || !fi.offloads || len(body) == 0 {
			continue
		}
		if !fi.netted {
			// no macro net in this file: every offloaded section must be
			// closed by an explicit hop back, with nothing but pure C between
			n, first, firstOff := 0, "", -1
			for _, o := range dynReOffload.FindAllIndex(body, -1) {
				end := dynReOnJS.FindIndex(body[o[1]:])
				seg := body[o[1]:]
				what := "no dyn_fiber_onjs() after the offload"
				if end != nil {
					seg = body[o[1] : o[1]+end[0]]
					what = ""
					if m := dynReCtxCall.FindSubmatch(seg); m != nil {
						what = string(m[1])
					} else if m := dynReEscape.Find(seg); m != nil {
						what = string(m) + " inside the offloaded section"
					}
				}
				if what != "" {
					n++
					if firstOff < 0 {
						first, firstOff = what, o[0]
					}
				}
			}
			if n > 0 {
				rows = append(rows, mkrow(fStr(s.Name()), fInt(n), fStr(first),
					fStr(g.dynSite(t, s, firstOff)), fStr(g.at(s))))
			}
			continue
		}
		n, first, firstOff := 0, "", -1
		for _, m := range dynReCtxCall.FindAllSubmatchIndex(body, -1) {
			callee := string(body[m[2]:m[3]])
			if fi.local[callee] || fi.wrapped[callee] || dynOffloadExempt[callee] ||
				strings.HasPrefix(callee, "dyn_fiber_") || strings.HasPrefix(callee, "dyn_ml_") {
				continue
			}
			if m[2] > 0 && (body[m[2]-1] == '.' || body[m[2]-1] == '>') {
				continue
			}
			n++
			if firstOff < 0 {
				first, firstOff = callee, m[0]
			}
		}
		if n > 0 {
			rows = append(rows, mkrow(fStr(s.Name()), fInt(n), fStr(first),
				fStr(g.dynSite(t, s, firstOff)), fStr(g.at(s))))
		}
	}
	dynSort(rows, dynDesc(1))
	return cols, limit(rows, p)
}

var dynShapeQueries = []qentry{
	{"offload-unguarded-runtime-call", "An engine call with no hop-back macro, in a file that offloads to a thread",
		"ANSWERS where code that may run on a compute thread can reach the JS\n     runtime. A file calling dyn_fiber_offload must redefine every engine\n     call it uses as a macro that returns to the JS thread first\n     (#define JS_X(...) (dyn_fiber_onjs(), JS_X(__VA_ARGS__))); this lists\n     the context-taking calls with no such macro.\nACT add the macro beside the others, never a manual hop at the call site.\nMISLEADS module registration (JS_NewCModule and friends) runs once at\n     import and is exempt by name; a call through a function pointer is\n     invisible here.",
		dynOffloadUnguarded},
	{"pending-exception-dropped", "A JS_Call result is freed with no exception handling in the function",
		"ANSWERS where a handler that throws is silently lost AND leaves its\n     exception pending on the context: JS_FreeValue of JS_EXCEPTION frees\n     nothing, so the error is still there for the next unrelated caller of\n     JS_GetException. Shipped once: DNSResolver.query callbacks were dropped\n     this way while TCP/UDP handlers were counted and cleared.\nACT hand the result to the module's accounting (dyn_net_handler_result,\n     dyn_call_drop) or test JS_IsException and clear with JS_GetException.\nMISLEADS a promise's own resolve/reject function cannot throw; rows where\n     every dropped call is one have resolver_only = 1 and sort last. The\n     callee column is the text of the second argument, nothing more.",
		dynPendingExceptionDropped},
	{"fma-dependent-exactness", "x - round_fn(...) * y: exact only where the compiler fuses it",
		"ANSWERS where a result is correct on arm64 and wrong on x86-64. A\n     remainder written as x - floor(x / y) * y is exact only when the\n     multiply and subtract are ONE fused operation; arm64 compilers contract\n     it into an FMA, baseline x86-64 has none and rounds twice. Shipped once:\n     mathx.mod(1e17, 3) returned the wrong remainder on x86.\nACT write the fused operation explicitly -- fma(-q, y, x) -- or use fmod.\n     Prove it natively with ./build.sh build CONFIG_FP_NOCONTRACT=y.\nMISLEADS a bucket or grid edge (floor(t / w) * w with no exactness claim)\n     has the shape and is only platform-divergent in the last ulp, not\n     wrong. has_fma_call = 1 means the function already asks for fusion\n     somewhere, not necessarily at this site.",
		dynFmaDependent},
	{"raw-uring-ring-open", "io_uring_queue_init* called outside dyn_uring_ring_open",
		"ANSWERS which ring setups bypass the memlock step-down. Ring memory is\n     charged against RLIMIT_MEMLOCK and a closed ring's charge comes back\n     asynchronously; a fixed 8192-entry ring failed with ENOMEM after ten\n     reactors at the common 8 MiB limit. dyn_uring_ring_open halves the ring\n     on ENOMEM and waits at the floor.\nACT route the setup through dyn_uring_ring_open.\nMISLEADS a short-lived probe ring of a few entries (dyna-uring.c's disk\n     queue) is small enough not to matter; it still appears.",
		dynRawRingOpen},
	{"reactor-acquire-error-shape", "dyn_net_reactor_acquire without the shared failure error",
		"ANSWERS which callers report a missing reactor in their own words. Every\n     class that needs the reactor must fail the same way and say why\n     (io_uring unavailable or denied); dyn_net_throw_no_reactor is that one\n     sentence.\nACT replace the local JS_Throw with dyn_net_throw_no_reactor(ctx, who).\nMISLEADS a caller that cannot throw (a C-only path returning NULL) is\n     listed and is fine.",
		dynReactorErrorShape},
	{"set-property-dynamic-key", "Prototype-walking set whose KEY is not a literal",
		"ANSWERS the real prototype-pollution candidates. JS_SetProperty and\n     JS_SetPropertyStr walk the prototype chain, so a peer-chosen key of\n     __proto__ retargets the object; define-vs-set lists every caller, this\n     lists only the calls whose key is computed.\nACT a key that comes from input -- a JSON member, a header name, a column\n     -- must be written with JS_DefinePropertyValue*.\nMISLEADS a key held in a local that was itself assigned a literal reads as\n     dynamic. on_frontier comes from the io taint closure, which cannot see\n     calls through a function pointer: 0 does not clear a row.",
		dynSetPropertyDynamicKey},
	{"typed-array-short-args", "JS_NewTypedArray over a BUFFER with fewer than three arguments",
		"ANSWERS a view that is silently EMPTY. One argument that is an array\n     buffer builds a length-0 view over it; a handler handed that loops\n     zero times and reports success.\nACT pass (buffer, byteOffset, length).\nMISLEADS one argument that is a LENGTH is the ordinary allocation and is\n     not listed. The buffer test is by name (buf, ab, buffer) or by an\n     assignment from JS_NewArrayBuffer* in the same function, so a buffer\n     passed in under another name is missed.",
		dynTypedArrayShortArgs},
	{"reactor-blocking-call", "A sleep or synchronous resolver call in reactor code",
		"ANSWERS where one connection's wait becomes every connection's wait.\nACT move the wait onto a timer, the lookup onto the pool.\nMISLEADS a deliberate bounded pause on a failure path (the ring-open\n     reclaim wait) and code that runs on a pool thread rather than the\n     reactor both appear; the file filter is by name, not by thread.",
		dynReactorBlockingCall},
	{"fsync-without-fullfsync", "fsync with no F_FULLFSYNC in the function",
		"ANSWERS which durability claims hold on Linux only. On Darwin fsync\n     returns when the data reaches the drive, not when the drive has\n     committed it; F_FULLFSYNC is the durable form.\nACT fcntl(fd, F_FULLFSYNC) first, plain fsync as the ENOTSUP fallback.\nMISLEADS a flush that is advisory by design.",
		dynFsyncNotDurable},
	{"locale-number-parse", "strtod / atof / scanf %f: the radix character is the locale's",
		"ANSWERS where a wire or file format is parsed with a function that reads\n     LC_NUMERIC and also accepts hex floats, leading space and partial\n     input. One setlocale call elsewhere and every float loses its fraction.\nACT hand-roll the grammar the format states.\nMISLEADS parsing a number a USER typed in their own locale is correct with\n     strtod. on_frontier is the io taint closure (a floor).",
		dynLocaleNumberParse},
	{"discarded-arm-result", "(void) on a call that arms, registers or submits",
		"ANSWERS where 'did the thing I depend on get set up' is thrown away. A\n     discarded timer or watcher registration makes every hook behind it\n     silent, and the caller's status check passes because it was never\n     wired to this.\nACT propagate the failure, or refuse the operation.\nMISLEADS a best-effort re-arm on a path that is already failing.",
		dynDiscardedArmResult},
}
