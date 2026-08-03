#!/usr/bin/env python3
"""Interactive REPL harness: drives ./dynajs under a real pty.

The REPL redraws the whole input line on every keystroke, so the raw pty
stream contains superseded renderings. Assertions therefore run against a
small terminal emulator (visible text) or against the output segment
emitted between one prompt and the next -- never against the raw stream.

Two groups, counted separately:
  baseline  -- must PASS against the current REPL.
  new       -- features not yet implemented; must FAIL on the baseline and
               PASS afterwards. A `new` case that passes on the baseline is
               a bug in the case, not a feature that exists.

Usage:
  python3 tests/test_repl.py                    every case must PASS
  python3 tests/test_repl.py --baseline         baseline must PASS, new must FAIL
  python3 tests/test_repl.py --binary <path>    engine under test (default ./dynajs)
  python3 tests/test_repl.py --selftest         also prove FAIL/TIMEOUT can fire
  python3 tests/test_repl.py -k <substr>        run matching cases only
  python3 tests/test_repl.py -v                 dump the visible screen per case
"""

import fcntl
import hashlib
import os
import pty
import re
import select
import shutil
import signal
import struct
import sys
import tempfile
import termios
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Reassigned by --binary so one harness can run against two builds without a
# rebuild. Cases never name a binary; they always go through Ctx.spawn.
BIN = os.path.join(REPO, "dynajs")

COLS, ROWS = 80, 24
CASE_TIMEOUT = 25.0     # hard wall-clock bound per case
READ_TIMEOUT = 6.0      # default bound on a single read_until
SETTLE = 0.12           # quiet period that means "the REPL stopped emitting"

PROMPT_RE = re.compile(r"dynajs > |\.\.\. ")


class Timeout(Exception):
    """Nothing arrived in time. Distinct from a wrong answer."""


# --------------------------------------------------------------------------
# terminal emulator
# --------------------------------------------------------------------------

# ECMA-48 CSI: ESC [ params intermediates final.  The parameter class
# includes '?', so private modes such as ESC [ ? 2004 h parse as one unit
# and never leak into the visible text.
CSI_RE = re.compile(r"\x1b\[([0-?]*)([ -/]*)([@-~])")
OSC_RE = re.compile(r"\x1b\]([^\x07\x1b]*)(?:\x07|\x1b\\)")
ESC2_RE = re.compile(r"\x1b([ -/]*[0-~])")


class Screen:
    """Enough of a terminal to hold the REPL's redraws.

    Models an unbounded grid with no scrollback: the cases stay well inside
    24 rows. Autowrap moves to the next row immediately rather than using a
    pending-wrap flag, which differs from a real terminal only when a line
    ends exactly at the right margin.
    """

    def __init__(self, width=COLS, height=ROWS):
        self.w = width
        self.h = height
        self.rows = [[]]
        self.cy = 0
        self.cx = 0
        self.pending = ""       # incomplete escape split across reads

    # -- grid helpers ------------------------------------------------------
    def _row(self, y):
        while len(self.rows) <= y:
            self.rows.append([])
        return self.rows[y]

    def _put(self, ch):
        row = self._row(self.cy)
        while len(row) < self.cx:
            row.append(" ")
        if self.cx < len(row):
            row[self.cx] = ch
        else:
            row.append(ch)
        self.cx += 1
        if self.cx >= self.w:
            self.cy += 1
            self.cx = 0
            self._row(self.cy)

    def _erase_display(self, mode):
        if mode == 0:
            row = self._row(self.cy)
            del row[self.cx:]
            del self.rows[self.cy + 1:]
        elif mode == 1:
            row = self._row(self.cy)
            for x in range(min(self.cx + 1, len(row))):
                row[x] = " "
            for y in range(self.cy):
                self.rows[y] = []
        else:
            self.rows = [[]]
            self.cy = self.cx = 0

    def _erase_line(self, mode):
        row = self._row(self.cy)
        if mode == 0:
            del row[self.cx:]
        elif mode == 1:
            for x in range(min(self.cx + 1, len(row))):
                row[x] = " "
        else:
            self.rows[self.cy] = []

    # -- CSI ---------------------------------------------------------------
    def _csi(self, params, inter, final):
        if params.startswith("?") or inter:
            return                      # private modes (incl. 2004), ignore
        nums = []
        for p in params.split(";"):
            nums.append(int(p) if p.isdigit() else None)

        def n(default=1):
            return nums[0] if nums and nums[0] is not None else default

        if final == "A":
            self.cy = max(0, self.cy - n())
        elif final == "B":
            self.cy += n()
            self._row(self.cy)
        elif final == "C":
            self.cx = min(self.w - 1, self.cx + n())
        elif final == "D":
            self.cx = max(0, self.cx - n())
        elif final in ("H", "f"):
            y = nums[0] if nums and nums[0] is not None else 1
            x = nums[1] if len(nums) > 1 and nums[1] is not None else 1
            self.cy, self.cx = max(0, y - 1), max(0, x - 1)
            self._row(self.cy)
        elif final == "J":
            self._erase_display(n(0))
        elif final == "K":
            self._erase_line(n(0))
        elif final == "G":
            self.cx = max(0, n() - 1)
        # 'm' (SGR) and everything else: no effect on visible text.

    # -- feed --------------------------------------------------------------
    def feed(self, data):
        data = self.pending + data
        self.pending = ""
        i, n = 0, len(data)
        while i < n:
            c = data[i]
            if c == "\x1b":
                rest = data[i:]
                m = CSI_RE.match(rest)
                if m:
                    self._csi(m.group(1), m.group(2), m.group(3))
                    i += m.end()
                    continue
                m = OSC_RE.match(rest)
                if m:
                    i += m.end()
                    continue
                # An introducer that did not match its full form is an escape
                # split across reads. It must be pended BEFORE the two-char
                # fallback, which would otherwise eat the "\x1b[" of an
                # incomplete CSI and render "?2004h" as visible text.
                if (rest == "\x1b" or rest.startswith("\x1b[")
                        or rest.startswith("\x1b]")):
                    if len(rest) <= 64:
                        self.pending = rest
                        return
                    i += 1          # runaway: treat ESC as noise, resync
                    continue
                m = ESC2_RE.match(rest)
                if m:
                    i += m.end()
                    continue
                self.pending = rest
                return
            i += 1
            if c == "\r":
                self.cx = 0
            elif c == "\n":
                self.cy += 1
                self.cx = 0
                self._row(self.cy)
            elif c == "\x08":
                self.cx = max(0, self.cx - 1)
            elif c == "\x07":
                pass
            elif c == "\t":
                self.cx = min(self.w - 1, (self.cx // 8 + 1) * 8)
            elif c >= " ":
                self._put(c)

    # -- readback ----------------------------------------------------------
    def lines(self):
        return ["".join(r).rstrip() for r in self.rows]

    def text(self):
        return "\n".join(self.lines())

    def cursor_line(self):
        return "".join(self._row(self.cy)).rstrip()

    def input_line(self):
        """The cursor's line with the prompt removed -- what is being edited.

        Bounded by the cursor column, not by rstrip: an editing command can
        legitimately leave a trailing space (^W on "foo bar" gives "foo "),
        and rstrip would silently turn that into a different answer.
        """
        raw = "".join(self._row(self.cy))
        end = max(self.cx, len(raw.rstrip()))
        m = None
        for m in PROMPT_RE.finditer(raw):
            pass
        start = m.end() if m else 0
        return raw[start:end]


def strip_ansi(data):
    out = []
    i, n = 0, len(data)
    while i < n:
        if data[i] == "\x1b":
            rest = data[i:]
            for rx in (CSI_RE, OSC_RE, ESC2_RE):
                m = rx.match(rest)
                if m:
                    i += m.end()
                    break
            else:
                i += 1
            continue
        out.append(data[i])
        i += 1
    return "".join(out)


# --------------------------------------------------------------------------
# the REPL under test
# --------------------------------------------------------------------------

class Repl:
    def __init__(self, env=None, cwd=REPO, args=(), cols=COLS, rows=ROWS):
        self.screen = Screen(cols, rows)
        self.stripped = ""      # ANSI-stripped stream, for output segments
        self.raw = b""
        self.alive = True
        self.status = None
        self.pid, self.fd = pty.fork()
        if self.pid == 0:                                   # child
            try:
                os.chdir(cwd)
                e = dict(os.environ)
                e["TERM"] = "xterm"
                e.pop("DYNAJS_HISTORY", None)
                if env:
                    for k, v in env.items():
                        if v is None:
                            e.pop(k, None)
                        else:
                            e[k] = v
                os.execve(BIN, [BIN] + list(args), e)
            except Exception:
                pass
            os._exit(127)
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ,
                    struct.pack("HHHH", rows, cols, 0, 0))

    # -- io ----------------------------------------------------------------
    def _drain(self, budget):
        """Read whatever is available within `budget` seconds. Returns bytes read."""
        end = time.time() + budget
        got = 0
        while True:
            left = end - time.time()
            if left <= 0:
                return got
            try:
                r, _, _ = select.select([self.fd], [], [], left)
            except (OSError, ValueError):
                return got
            if not r:
                return got
            try:
                data = os.read(self.fd, 65536)
            except OSError:                 # EIO == child gone
                self.alive = False
                return got
            if not data:
                self.alive = False
                return got
            self.raw += data
            text = data.decode("utf-8", "replace")
            self.screen.feed(text)
            self.stripped += strip_ansi(text)
            got += len(data)

    def read_until(self, pred, timeout=READ_TIMEOUT, what="condition"):
        """Pump until pred() is true. Raises Timeout -- never returns False."""
        end = time.time() + timeout
        if pred():
            return
        while time.time() < end:
            self._drain(min(0.2, max(0.01, end - time.time())))
            if pred():
                return
            if not self.alive:
                break
        if pred():
            return
        raise Timeout("timed out after %.1fs waiting for %s" % (timeout, what))

    def settle(self, quiet=SETTLE, cap=2.0):
        """Pump until the REPL goes quiet for `quiet` seconds."""
        end = time.time() + cap
        while time.time() < end:
            if self._drain(quiet) == 0:
                return

    def n_prompts(self):
        return len(PROMPT_RE.findall(self.stripped))

    def wait_prompt(self, timeout=READ_TIMEOUT):
        n = self.n_prompts()
        self.read_until(lambda: self.n_prompts() > n, timeout, "a new prompt")

    def wait_banner(self, timeout=READ_TIMEOUT):
        self.read_until(lambda: self.n_prompts() >= 1, timeout, "the first prompt")
        self.settle()

    # -- input -------------------------------------------------------------
    def send(self, data):
        if isinstance(data, str):
            data = data.encode("utf-8")
        os.write(self.fd, data)

    def type(self, text, settle=True):
        """Type printable text; no newlines (they would submit the line)."""
        for ch in text:
            self.send(ch)
            time.sleep(0.002)
        if settle:
            self.settle()

    def enter(self, timeout=READ_TIMEOUT):
        """Press Enter and return the output emitted before the next prompt."""
        mark = len(self.stripped)
        self.send("\r")
        self.wait_prompt(timeout)
        seg = self.stripped[mark:]
        return PROMPT_RE.split(seg)[0].strip()

    def run(self, text, timeout=READ_TIMEOUT):
        self.type(text)
        return self.enter(timeout)

    def clear_line(self):
        self.send("\x18")               # ^X -> reset(): empties the buffer
        self.settle()

    # -- teardown ----------------------------------------------------------
    def wait_exit(self, timeout=5.0):
        end = time.time() + timeout
        while time.time() < end:
            self._drain(0.05)
            try:
                pid, st = os.waitpid(self.pid, os.WNOHANG)
            except ChildProcessError:
                self.alive = False
                return self.status if self.status is not None else 0
            if pid:
                self.alive = False
                self.status = os.waitstatus_to_exitcode(st)
                return self.status
        raise Timeout("child did not exit within %.1fs" % timeout)

    def close(self):
        try:
            os.kill(self.pid, signal.SIGKILL)
        except (ProcessLookupError, OSError):
            pass
        try:
            os.waitpid(self.pid, 0)
        except (ChildProcessError, OSError):
            pass
        try:
            os.close(self.fd)
        except OSError:
            pass


# --------------------------------------------------------------------------
# registry
# --------------------------------------------------------------------------

CASES = []


def case(group, name, timeout=CASE_TIMEOUT):
    def deco(fn):
        CASES.append((group, name, fn, timeout))
        return fn
    return deco


class Ctx:
    """Owns every child a case spawns so none can outlive the case."""

    def __init__(self):
        self.children = []
        self.tmpdirs = []

    def spawn(self, **kw):
        r = Repl(**kw)
        self.children.append(r)
        r.wait_banner()
        return r

    def tmpdir(self):
        d = tempfile.mkdtemp(prefix="repltest_")
        self.tmpdirs.append(d)
        return d

    def cleanup(self):
        for r in self.children:
            r.close()
        for d in self.tmpdirs:
            shutil.rmtree(d, ignore_errors=True)


def want(cond, msg):
    if not cond:
        raise AssertionError(msg)


def want_in(needle, hay, what):
    if needle not in hay:
        raise AssertionError("%s: expected %r in %r" % (what, needle, hay))


# ==========================================================================
# GROUP: baseline -- must pass against the current REPL
# ==========================================================================

@case("baseline", "1+1 prints 2")
def t_arith(ctx):
    r = ctx.spawn()
    out = r.run("1+1")
    want(out == "2", "expected result '2', got %r" % out)


@case("baseline", "multi-line function then call prints 42")
def t_multiline(ctx):
    r = ctx.spawn()
    r.type("function f() {")
    r.enter()                                   # continuation prompt
    r.type("return 42")
    r.enter()
    r.type("}")
    out = r.enter()
    want(out == "undefined", "function decl should yield undefined, got %r" % out)
    out = r.run("f()")
    want(out == "42", "expected '42', got %r" % out)


@case("baseline", "Up-arrow recalls the previous line")
def t_history_up(ctx):
    r = ctx.spawn()
    r.run("1+1")
    r.send("\x1b[A")
    r.settle()
    line = r.screen.input_line()
    want(line == "1+1", "Up-arrow should recall '1+1', input line is %r" % line)


@case("baseline", "Tab completes Mat -> Math")
def t_tab_unique(ctx):
    r = ctx.spawn()
    r.type("Mat")
    r.send("\x09")
    r.settle()
    line = r.screen.input_line()
    want(line == "Math", "Mat<TAB> should give 'Math', input line is %r" % line)


@case("baseline", "Tab Tab on ambiguous prefix lists Map and Math")
def t_tab_list(ctx):
    r = ctx.spawn()
    r.type("Ma")
    mark = len(r.stripped)
    r.send("\x09")
    r.settle()
    r.send("\x09")
    r.settle()
    seg = r.stripped[mark:]
    want_in("Map", seg, "completion list")
    want_in("Math", seg, "completion list")


@case("baseline", "\\h prints help")
def t_help(ctx):
    r = ctx.spawn()
    out = r.run("\\h")
    want_in("this help", out, "help text")
    want_in("\\q", out, "help text")


@case("baseline", "\\q exits cleanly")
def t_quit(ctx):
    r = ctx.spawn()
    r.type("\\q")
    r.send("\r")
    st = r.wait_exit()
    want(st == 0, "\\q should exit 0, got %r" % st)


@case("baseline", "Ctrl-D on an empty line exits cleanly")
def t_ctrl_d(ctx):
    r = ctx.spawn()
    r.send("\x04")
    st = r.wait_exit()
    want(st == 0, "Ctrl-D should exit 0, got %r" % st)


@case("baseline", "a runtime error is reported and the REPL survives")
def t_error_recovers(ctx):
    r = ctx.spawn()
    out = r.run("throw new Error('boom')")
    want_in("boom", out, "error message")
    out = r.run("1+1")
    want(out == "2", "REPL should still evaluate after an error, got %r" % out)


# ==========================================================================
# GROUP: new -- must FAIL on the baseline, PASS after the change
# ==========================================================================

@case("new", "history persists across sessions via DYNAJS_HISTORY")
def t_hist_persist(ctx):
    home = ctx.tmpdir()
    path = os.path.join(home, "hist")
    env = {"DYNAJS_HISTORY": path, "HOME": home}

    a = ctx.spawn(env=env)
    a.run("let zz=1")
    a.type("\\q")
    a.send("\r")
    a.wait_exit()

    want(os.path.exists(path), "history file %s was not written" % path)

    b = ctx.spawn(env=env)
    b.send("\x1b[A")
    b.settle()
    line = b.screen.input_line()
    want(line == "let zz=1",
         "second session Up-arrow should recall 'let zz=1', got %r" % line)


@case("new", 'DYNAJS_HISTORY="" disables the history file')
def t_hist_disabled(ctx):
    # The negative half alone passes on any build with no history feature at
    # all, so it proves nothing on its own. Establish that the mechanism
    # exists first, then show the empty value switches it off.
    on_home = ctx.tmpdir()
    on_path = os.path.join(on_home, "hist")
    a = ctx.spawn(env={"DYNAJS_HISTORY": on_path, "HOME": on_home})
    a.run("let qq=1")
    a.send("\x04")                      # Ctrl-D: a keystroke, never a history line
    a.wait_exit()
    want(os.path.exists(on_path),
         "control failed: history is not written even when enabled, so this "
         "case cannot show that the empty value disables anything")

    off_home = ctx.tmpdir()
    b = ctx.spawn(env={"DYNAJS_HISTORY": "", "HOME": off_home})
    b.run("let qq=1")
    b.send("\x04")
    b.wait_exit()
    left = os.listdir(off_home)
    want(left == [], "no history file expected under HOME, found %r" % left)

    c = ctx.spawn(env={"DYNAJS_HISTORY": "", "HOME": off_home})
    c.send("\x1b[A")
    c.settle()
    line = c.screen.input_line()
    want(line == "",
         "history is disabled so Up-arrow must recall nothing, got %r" % line)


@case("new", "Ctrl-R reverse search finds a previous line")
def t_ctrl_r(ctx):
    r = ctx.spawn()
    r.run("let banana=1")
    r.send("\x12")
    r.settle()
    r.type("ban")
    want_in("reverse-i-search", r.screen.text(), "reverse search prompt")
    want_in("let banana=1", r.screen.cursor_line(), "recalled line")


@case("new", "bracketed paste inserts a multi-line block literally")
def t_bracketed_paste(ctx):
    r = ctx.spawn()
    mark = len(r.stripped)
    before = r.n_prompts()
    r.send("\x1b[200~")
    r.send("function g() {\nreturn 7\n}")
    r.settle()
    # Measured BEFORE the terminator. Inside the bracket the newlines are
    # text, so nothing is submitted: prompts stay put and the REPL emits
    # nothing at all. Without bracketed paste each newline submits a line --
    # measured 3 prompts and a per-keystroke echo storm on the old build.
    # The end result (g() == 7) is identical either way, so asserting only
    # that would not discriminate.
    issued = r.n_prompts() - before
    want(issued == 0,
         "paste body must not submit anything, but %d prompt(s) were issued;"
         " stream was %r" % (issued, r.stripped[mark:]))

    r.send("\x1b[201~")
    r.settle()
    seg = r.stripped[mark:]
    want("200~" not in seg and "201~" not in seg,
         "paste markers leaked into the visible stream: %r" % seg)
    r.enter()
    out = r.run("g()")
    want(out == "7", "expected '7' from the pasted function, got %r" % out)


@case("new", "Ctrl-W kills the word before the cursor")
def t_ctrl_w(ctx):
    r = ctx.spawn()
    r.type("foo bar")
    r.send("\x17")
    r.settle()
    line = r.screen.input_line()
    want(line == "foo ", "Ctrl-W should leave 'foo ', got %r" % line)


@case("new", "Ctrl-U kills the whole line")
def t_ctrl_u(ctx):
    r = ctx.spawn()
    r.type("abc")
    r.send("\x15")
    r.settle()
    line = r.screen.input_line()
    want(line == "", "Ctrl-U should empty the line, got %r" % line)


@case("new", "Ctrl-L clears the screen and keeps the line")
def t_ctrl_l(ctx):
    r = ctx.spawn()
    r.type("keepme")
    r.send("\x0c")
    r.settle()
    txt = r.screen.text()
    want("DynaJS - Type" not in txt,
         "Ctrl-L should clear the banner off the screen, screen is %r" % txt)
    line = r.screen.input_line()
    want(line == "keepme", "Ctrl-L must keep the input line, got %r" % line)


@case("new", "Ctrl-C discards the line and gives a fresh prompt")
def t_ctrl_c(ctx):
    r = ctx.spawn()
    r.type("abcdef")
    mark = len(r.stripped)
    r.send("\x03")
    r.settle()
    seg = r.stripped[mark:]
    want_in("^C", seg, "Ctrl-C marker")
    line = r.screen.input_line()
    want(line == "", "Ctrl-C must discard the line, input line is %r" % line)
    out = r.run("1+1")
    want(out == "2", "expected '2' on the fresh line, got %r" % out)


@case("new", "\\lo<TAB> completes to \\load")
def t_directive_completion(ctx):
    r = ctx.spawn()
    r.type("\\lo")
    r.send("\x09")
    r.settle()
    line = r.screen.input_line()
    want(line == "\\load", "\\lo<TAB> should give '\\load', got %r" % line)


@case("new", "\\load ./REA<TAB> completes a path")
def t_path_completion(ctx):
    r = ctx.spawn()
    r.type("\\load ./REA")
    r.send("\x09")
    r.settle()
    line = r.screen.input_line()
    want(line == "\\load ./README.md",
         "path completion should give '\\load ./README.md', got %r" % line)


@case("new", "a resolved promise is auto-awaited")
def t_promise_await(ctx):
    r = ctx.spawn()
    out = r.run("Promise.resolve(42)")
    want(out == "42", "expected '42', got %r" % out)


@case("new", "_err holds the last error")
def t_last_error(ctx):
    r = ctx.spawn()
    r.run("nosuchfn()")
    out = r.run("_err instanceof ReferenceError")
    want(out == "true", "expected 'true', got %r" % out)


@case("new", "\\h documents \\load and ^R")
def t_help_mentions(ctx):
    r = ctx.spawn()
    out = r.run("\\h")
    want_in("\\load", out, "help text")
    want_in("^R", out, "help text")


# ==========================================================================
# GROUP: selftest -- proves the harness itself can report TIMEOUT and FAIL.
# Opt-in (--selftest): a check nobody has watched fail is not a check.
# ==========================================================================

@case("selftest", "read_until reports TIMEOUT when text never arrives")
def t_self_timeout(ctx):
    r = ctx.spawn()
    r.read_until(lambda: "NEVER-PRINTED-BY-ANY-REPL" in r.stripped,
                 timeout=2.0, what="a string the REPL cannot emit")


@case("selftest", "the global per-case alarm fires on a wedged case", timeout=2.0)
def t_self_alarm(ctx):
    ctx.spawn()
    while True:                     # wedged: only the wall-clock alarm frees it
        time.sleep(0.05)


@case("selftest", "a wrong expectation reports FAIL")
def t_self_fail(ctx):
    r = ctx.spawn()
    out = r.run("1+1")
    want(out == "3", "deliberately wrong: expected '3', got %r" % out)


# ==========================================================================
# GROUP: unit -- the emulator itself. No subprocess; always runs.
# ==========================================================================

@case("unit", "private-mode and SGR escapes leave no visible residue")
def t_unit_ansi(ctx):
    s = Screen()
    s.feed("\x1b[?2004hab\x1b[32mc\x1b[0m\x1b[?2004l")
    want(s.text() == "abc", "escapes leaked into visible text: %r" % s.text())
    s2 = Screen()
    for chunk in ("\x1b", "[?20", "04h", "xy"):          # split across reads
        s2.feed(chunk)
    want(s2.text() == "xy", "split escape leaked: %r" % s2.text())
    want(strip_ansi("\x1b[?2004hq\x1b[0m") == "q", "strip_ansi leaked residue")


@case("unit", "the emulator honours erase and cursor motion")
def t_unit_erase(ctx):
    s = Screen()
    s.feed("hello\x1b[3D\x1b[J")             # back 3, erase to end of display
    want(s.text() == "he", "erase-to-end gave %r" % s.text())
    s = Screen()
    s.feed("abc\rX")                          # CR then overwrite
    want(s.text() == "Xbc", "carriage return gave %r" % s.text())
    s = Screen()
    s.feed("dynajs > foo bar")
    want(s.input_line() == "foo bar", "input_line gave %r" % s.input_line())
    # The REPL's ^W redraw: back over all 7 typed chars, reprint, erase tail.
    s.feed("\x1b[7D" + "foo " + "\x1b[J")
    want(s.input_line() == "foo ",
         "input_line must keep a trailing space, got %r" % s.input_line())
    s.feed("\x1b[4D\x1b[J")                   # now empty
    want(s.input_line() == "",
         "an empty input line must read as '', got %r" % s.input_line())


# ==========================================================================
# runner
# ==========================================================================

PASS, FAIL, TMOUT = "PASS", "FAIL", "TIMEOUT"


_alarm_budget = CASE_TIMEOUT


def _alarm(signum, frame):
    raise Timeout("case exceeded its %.0fs wall-clock bound" % _alarm_budget)


def run_case(fn, budget=CASE_TIMEOUT, verbose=False):
    global _alarm_budget
    _alarm_budget = budget
    ctx = Ctx()
    signal.signal(signal.SIGALRM, _alarm)
    signal.setitimer(signal.ITIMER_REAL, budget)
    try:
        fn(ctx)
        return PASS, ""
    except Timeout as e:
        return TMOUT, str(e)
    except AssertionError as e:
        return FAIL, str(e)
    except Exception as e:
        return FAIL, "%s: %s" % (type(e).__name__, e)
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        if verbose:
            for r in ctx.children:
                sys.stderr.write("--- screen ---\n%s\n" % r.screen.text())
        ctx.cleanup()


def main(argv):
    global BIN
    baseline_mode = "--baseline" in argv
    selftest = "--selftest" in argv
    verbose = "-v" in argv
    only = None
    if "-k" in argv:
        only = argv[argv.index("-k") + 1]
    if "--binary" in argv:
        BIN = os.path.abspath(argv[argv.index("--binary") + 1])

    if not os.access(BIN, os.X_OK):
        sys.stderr.write("no executable at %s -- build first\n" % BIN)
        return 2

    # A host with no /dev/ptmx (some minimal containers) cannot run any of
    # this. Say so once, loudly, instead of throwing the same OSError from
    # every case and looking like 24 test failures.
    try:
        _probe_pid, _probe_fd = pty.fork()
        if _probe_pid == 0:
            os._exit(0)
        os.close(_probe_fd)
        os.waitpid(_probe_pid, 0)
    except OSError as e:
        print("=== SKIP test_repl: no pty available (%s)" % e)
        return 0

    # Identify the build under test in the output itself: two runs that report
    # the same hash were the same experiment run twice.
    with open(BIN, "rb") as fh:
        digest = hashlib.sha256(fh.read()).hexdigest()
    started = time.time()
    print("binary : %s" % BIN)
    print("sha256 : %s" % digest)
    print("size   : %d bytes" % os.path.getsize(BIN))
    print()

    # The selftest group asserts the harness can report TIMEOUT and FAIL, so
    # its expected outcome is inverted: every case must NOT pass.
    results = {"unit": [], "baseline": [], "new": [], "selftest": []}
    for group, name, fn, budget in CASES:
        if group == "selftest" and not selftest:
            continue
        if only and only not in name:
            continue
        status, detail = run_case(fn, budget, verbose)
        results[group].append((name, status, detail))
        print("%-7s [%-8s] %s%s" % (status, group, name,
                                    ("  -- " + detail) if detail else ""))
        sys.stdout.flush()

    print()
    bad = 0
    for group in ("unit", "baseline", "new", "selftest"):
        rows = results[group]
        if not rows:
            continue
        if group == "selftest":
            n_pass = sum(1 for _, s, _ in rows if s == PASS)
            n_to = sum(1 for _, s, _ in rows if s == TMOUT)
            n_fail = sum(1 for _, s, _ in rows if s == FAIL)
            print("selftest %2d cases: %2d pass  %2d fail  %2d timeout"
                  " (each must NOT pass)" % (len(rows), n_pass, n_fail, n_to))
            for n, s, _ in rows:
                if s == PASS:
                    print("  BROKEN: %r passed, so that path cannot report a "
                          "problem" % n)
                    bad += 1
            continue
        n_pass = sum(1 for _, s, _ in rows if s == PASS)
        n_fail = sum(1 for _, s, _ in rows if s == FAIL)
        n_to = sum(1 for _, s, _ in rows if s == TMOUT)
        print("%-8s %2d cases: %2d pass  %2d fail  %2d timeout"
              % (group, len(rows), n_pass, n_fail, n_to))

        if group in ("unit", "baseline") or not baseline_mode:
            bad += n_fail + n_to
        else:
            # Baseline run: a `new` case that PASSES is not testing what it
            # claims, because the feature does not exist yet.
            bogus = [n for n, s, _ in rows if s == PASS]
            if bogus:
                print("  BOGUS (passed on baseline, so they prove nothing):")
                for n in bogus:
                    print("    - %s" % n)
                bad += len(bogus)
            if n_to:
                print("  note: %d new case(s) TIMED OUT rather than failed" % n_to)

    print()
    print("binary  %s (%s)" % (BIN, digest[:16]))
    print("elapsed %.1fs" % (time.time() - started))
    print("VERDICT: %s" % ("ok" if bad == 0 else "%d problem(s)" % bad))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.stdout.reconfigure(line_buffering=True)
    sys.exit(main(sys.argv[1:]))
