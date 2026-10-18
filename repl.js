import * as std from "std";
import * as os from "os";

(function(g) {
    g.os = os;
    g.std = std;

    var Object = g.Object;
    var String = g.String;
    var Array = g.Array;
    var Date = g.Date;
    var Math = g.Math;
    var Promise = g.Promise;
    var Error = g.Error;
    var isFinite = g.isFinite;
    var parseFloat = g.parseFloat;

    var colors = {
        none:    "\x1b[0m",
        black:   "\x1b[30m",
        red:     "\x1b[31m",
        green:   "\x1b[32m",
        yellow:  "\x1b[33m",
        blue:    "\x1b[34m",
        magenta: "\x1b[35m",
        cyan:    "\x1b[36m",
        white:   "\x1b[37m",
        gray:    "\x1b[30;1m",
        grey:    "\x1b[30;1m",
        bright_red:     "\x1b[31;1m",
        bright_green:   "\x1b[32;1m",
        bright_yellow:  "\x1b[33;1m",
        bright_blue:    "\x1b[34;1m",
        bright_magenta: "\x1b[35;1m",
        bright_cyan:    "\x1b[36;1m",
        bright_white:   "\x1b[37;1m",
    };

    var styles = {
        'default':    'bright_green',
        'comment':    'white',
        'string':     'bright_cyan',
        'regex':      'cyan',
        'number':     'green',
        'keyword':    'bright_white',
        'function':   'bright_yellow',
        'type':       'bright_magenta',
        'identifier': 'bright_green',
        'error':      'red',
        'result':     'bright_white',
        'error_msg':  'bright_red',
    };

    var history = [];
    var history_max = 1000;
    var history_path = null;
    var history_dirty = false;
    var history_draft = "";
    var clip_board = "";
    var prec;
    var expBits;
    var log2_10;

    var pstate = "";
    var prompt = "";
    var plen = 0;
    var ps1 = "dynajs > ";
    var ps2 = "  ... ";
    var utf8 = true;
    var show_time = false;
    var show_colors = true;
    var eval_start_time;
    var eval_time = 0;

    var mexpr = "";
    var level = 0;
    var cmd = "";
    var cursor_pos = 0;
    var last_cmd = "";
    var last_cursor_pos = 0;
    var history_index;
    var this_fun, last_fun;
    var quote_flag = false;

    var utf8_state = 0;
    var utf8_val = 0;

    var term_fd;
    var term_read_buf;
    var term_width;
    var term_is_tty = false;
    var term_cursor_x = 0;

    var search_mode = false;
    var search_dir = -1;
    var search_str = "";
    var search_index = 0;
    var search_match_pos = 0;
    var search_failed = false;
    var search_saved_cmd = "";
    var search_saved_pos = 0;
    var search_printed = 0;

    var paste_mode = false;
    var paste_buf = "";
    var pending_lines = [];

    var eval_gen = 0;
    var eval_busy = false;

    var busy_keys = [];
    var eof_seen = false;

    var completion_tabs = 0;

    function termInit() {
        term_fd = std.in.fileno();

        term_width = 80;
        term_is_tty = !!os.isatty(term_fd);
        if (!term_is_tty)
            show_colors = false;
        if (term_is_tty) {
            term_update_size();
            if (os.ttySetRaw) {
                os.ttySetRaw(term_fd);
            }
            std.puts("\x1b[?2004h");
        }

        os.signal(os.SIGINT, sigint_handler);

        term_read_buf = new Uint8Array(64);
        os.setReadHandler(term_fd, term_read_handler);
    }

    function term_update_size() {
        var tab;
        if (!term_is_tty || !os.ttyGetWinSize)
            return;
        tab = os.ttyGetWinSize(term_fd);
        if (tab && tab[0] >= 4)
            term_width = tab[0];
    }

    function repl_cleanup() {
        history_save();
        if (term_is_tty)
            std.puts("\x1b[?2004l");
        std.out.flush();
    }

    function repl_exit(code) {
        repl_cleanup();
        std.exit(code);
    }

    function history_escape(str) {
        var r = "", i, c;
        for (i = 0; i < str.length; i++) {
            c = str[i];
            if (c === "\\")
                r += "\\\\";
            else if (c === "\n")
                r += "\\n";
            else if (c !== "\r")
                r += c;
        }
        return r;
    }

    function history_unescape(str) {
        var r = "", i, c;
        for (i = 0; i < str.length; i++) {
            c = str[i];
            if (c === "\\" && i + 1 < str.length) {
                c = str[++i];
                r += (c === "n") ? "\n" : c;
            } else {
                r += c;
            }
        }
        return r;
    }

    function history_file_path() {
        var p = std.getenv("DYNAJS_HISTORY");
        if (p !== undefined)
            return (p === "" || p === "0") ? null : p;
        p = std.getenv("HOME");
        return p ? p + "/.dynajs_history" : null;
    }

    function history_load() {
        var f, line;
        history_path = history_file_path();
        if (!history_path)
            return;
        f = std.open(history_path, "r");
        if (!f)
            return;
        while ((line = f.getline()) !== null) {
            line = history_unescape(line);
            if (line)
                history.push(line);
        }
        f.close();
        if (history.length > history_max)
            history.splice(0, history.length - history_max);
    }

    function history_save() {
        var f, i, fd;
        if (!history_path || !history_dirty)
            return;
        fd = os.open(history_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600);
        if (fd < 0)
            return;
        f = std.fdopen(fd, "w");
        if (!f) {
            os.close(fd);
            return;
        }
        for (i = Math.max(0, history.length - history_max); i < history.length; i++)
            f.puts(history_escape(history[i]) + "\n");
        f.close();
        history_dirty = false;
    }

    function sigint_handler() {
        handle_byte(3);
    }

    function term_read_handler() {
        var l, i;
        l = os.read(term_fd, term_read_buf.buffer, 0, term_read_buf.length);
        if (l <= 0) {
            if (eval_busy) {
                eof_seen = true;
                return;
            }
            std.puts("\n");
            repl_exit(0);
        }
        for(i = 0; i < l; i++)
            handle_byte(term_read_buf[i]);
    }

    function handle_byte(c) {
        if (!utf8) {
            handle_char(c);
        } else if (utf8_state !== 0 && (c >= 0x80 && c < 0xc0)) {
            utf8_val = (utf8_val << 6) | (c & 0x3F);
            utf8_state--;
            if (utf8_state === 0) {
                handle_char(utf8_val);
            }
        } else if (c >= 0xc0 && c < 0xf8) {
            utf8_state = 1 + (c >= 0xe0) + (c >= 0xf0);
            utf8_val = c & ((1 << (6 - utf8_state)) - 1);
        } else {
            utf8_state = 0;
            handle_char(c);
        }
    }

    function is_alpha(c) {
        return typeof c === "string" &&
            ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
    }

    function is_digit(c) {
        return typeof c === "string" && (c >= '0' && c <= '9');
    }

    function is_word(c) {
        return typeof c === "string" &&
            (is_alpha(c) || is_digit(c) || c == '_' || c == '$');
    }

    function ucs_length(str) {
        var len, c, i, str_len = str.length;
        len = 0;
        for(i = 0; i < str_len; i++) {
            c = str.charCodeAt(i);
            if (c < 0xdc00 || c >= 0xe000)
                len++;
        }
        return len;
    }

    function is_trailing_surrogate(c)  {
        var d;
        if (typeof c !== "string")
            return false;
        d = c.codePointAt(0);
        return d >= 0xdc00 && d < 0xe000;
    }

    function is_balanced(a, b) {
        switch (a + b) {
        case "()":
        case "[]":
        case "{}":
            return true;
        }
        return false;
    }

    function print_color_text(str, start, style_names) {
        var i, j;
        for (j = start; j < str.length;) {
            var style = style_names[i = j];
            while (++j < str.length && style_names[j] == style)
                continue;
            std.puts(colors[styles[style] || 'default']);
            std.puts(str.substring(i, j));
            std.puts(colors['none']);
        }
    }

    function print_csi(n, code) {
        std.puts("\x1b[" + ((n != 1) ? n : "") + code);
    }

    function move_cursor(delta) {
        var i, l;
        if (delta > 0) {
            while (delta != 0) {
                if (term_cursor_x == (term_width - 1)) {
                    std.puts("\n");
                    term_cursor_x = 0;
                    delta--;
                } else {
                    l = Math.min(term_width - 1 - term_cursor_x, delta);
                    print_csi(l, "C");
                    delta -= l;
                    term_cursor_x += l;
                }
            }
        } else {
            delta = -delta;
            while (delta != 0) {
                if (term_cursor_x == 0) {
                    print_csi(1, "A");
                    print_csi(term_width - 1, "C");
                    delta--;
                    term_cursor_x = term_width - 1;
                } else {
                    l = Math.min(delta, term_cursor_x);
                    print_csi(l, "D");
                    delta -= l;
                    term_cursor_x -= l;
                }
            }
        }
    }

    function update() {
        var i, cmd_len;
        if (cmd != last_cmd) {
            if (!show_colors && last_cmd.substring(0, last_cursor_pos) == cmd.substring(0, last_cursor_pos)) {
                std.puts(cmd.substring(last_cursor_pos));
            } else {
                move_cursor(-ucs_length(last_cmd.substring(0, last_cursor_pos)));
                if (show_colors) {
                    var str = mexpr ? mexpr + '\n' + cmd : cmd;
                    var start = str.length - cmd.length;
                    var colorstate = colorize_js(str);
                    print_color_text(str, start, colorstate[2]);
                } else {
                    std.puts(cmd);
                }
            }
            term_cursor_x = (term_cursor_x + ucs_length(cmd)) % term_width;
            if (term_cursor_x == 0 && term_is_tty) {
                std.puts(" \x08");
            }
            if (term_is_tty)
                std.puts("\x1b[J");
            last_cmd = cmd;
            last_cursor_pos = cmd.length;
        }
        if (cursor_pos > last_cursor_pos) {
            move_cursor(ucs_length(cmd.substring(last_cursor_pos, cursor_pos)));
        } else if (cursor_pos < last_cursor_pos) {
            move_cursor(-ucs_length(cmd.substring(cursor_pos, last_cursor_pos)));
        }
        last_cursor_pos = cursor_pos;
        std.out.flush();
    }

    function insert(str) {
        if (str) {
            cmd = cmd.substring(0, cursor_pos) + str + cmd.substring(cursor_pos);
            cursor_pos += str.length;
        }
    }

    function quoted_insert() {
        quote_flag = true;
    }

    function abort() {
        cmd = "";
        cursor_pos = 0;
        return -2;
    }

    function alert() {
    }

    function beginning_of_line() {
        cursor_pos = 0;
    }

    function end_of_line() {
        cursor_pos = cmd.length;
    }

    function forward_char() {
        if (cursor_pos < cmd.length) {
            cursor_pos++;
            while (is_trailing_surrogate(cmd.charAt(cursor_pos)))
                cursor_pos++;
        }
    }

    function backward_char() {
        if (cursor_pos > 0) {
            cursor_pos--;
            while (is_trailing_surrogate(cmd.charAt(cursor_pos)))
                cursor_pos--;
        }
    }

    function skip_word_forward(pos) {
        while (pos < cmd.length && !is_word(cmd.charAt(pos)))
            pos++;
        while (pos < cmd.length && is_word(cmd.charAt(pos)))
            pos++;
        return pos;
    }

    function skip_word_backward(pos) {
        while (pos > 0 && !is_word(cmd.charAt(pos - 1)))
            pos--;
        while (pos > 0 && is_word(cmd.charAt(pos - 1)))
            pos--;
        return pos;
    }

    function forward_word() {
        cursor_pos = skip_word_forward(cursor_pos);
    }

    function backward_word() {
        cursor_pos = skip_word_backward(cursor_pos);
    }

    function accept_line() {
        std.puts("\n");
        history_add(cmd);
        return -1;
    }

    function history_add(str) {
        if (str && !/^\s*\\q\s*$/.test(str)) {
            if (mexpr !== "" || history[history.length - 1] !== str) {
                history.push(str);
                history_dirty = true;
            }
            if (history.length > history_max)
                history.splice(0, history.length - history_max);
        }
        history_index = history.length;
    }

    function previous_history() {
        if (history_index > 0) {
            if (history_index == history.length)
                history_draft = cmd;
            history_index--;
            cmd = history[history_index];
            cursor_pos = cmd.length;
        }
    }

    function next_history() {
        if (history_index < history.length) {
            history_index++;
            cmd = (history_index == history.length) ?
                history_draft : history[history_index];
            cursor_pos = cmd.length;
        }
    }

    function search_redraw() {
        var p, tail;
        p = (search_failed ? "(failed " : "(") +
            (search_dir < 0 ? "reverse-i-search)`" : "i-search)`") +
            search_str + "': ";
        move_cursor(-search_printed);
        std.puts("\x1b[J");
        std.puts(p + cmd);
        term_cursor_x = (term_cursor_x + ucs_length(p) + ucs_length(cmd)) % term_width;
        if (term_cursor_x == 0)
            std.puts(" \x08");
        search_printed = ucs_length(p) + ucs_length(cmd);
        tail = ucs_length(cmd) - ucs_length(cmd.substring(0, search_match_pos));
        if (tail > 0) {
            move_cursor(-tail);
            search_printed -= tail;
        }
        cursor_pos = search_match_pos;
        last_cmd = cmd;
        last_cursor_pos = cursor_pos;
        std.out.flush();
    }

    function search_run(step) {
        var idx, pos;
        idx = search_index + step;
        while (idx >= 0 && idx < history.length) {
            pos = history[idx].indexOf(search_str);
            if (pos >= 0) {
                search_index = idx;
                search_match_pos = pos;
                search_failed = false;
                cmd = history[idx];
                search_redraw();
                return;
            }
            idx += search_dir;
        }
        search_failed = true;
        search_redraw();
    }

    function search_begin(dir) {
        if (search_mode) {
            search_dir = dir;
            search_run(dir);
            return;
        }
        search_mode = true;
        search_dir = dir;
        search_str = "";
        search_failed = false;
        search_saved_cmd = cmd;
        search_saved_pos = cursor_pos;
        search_index = (dir < 0) ? history.length - 1 : 0;
        search_match_pos = 0;
        search_printed = ucs_length(prompt) +
            ucs_length(cmd.substring(0, cursor_pos));
        search_redraw();
    }

    function search_backward() { search_begin(-1); }
    function search_forward() { search_begin(1); }

    function search_finish(restore) {
        search_mode = false;
        if (restore) {
            cmd = search_saved_cmd;
            cursor_pos = search_saved_pos;
        }
        move_cursor(-search_printed);
        std.puts("\x1b[J");
        search_printed = 0;
        readline_print_prompt();
        update();
    }

    function search_key(keys) {
        switch (keys) {
        case "\x12":
            search_begin(-1);
            return true;
        case "\x13":
            search_begin(1);
            return true;
        case "\x7f":
        case "\x08":
            search_str = search_str.substring(0, search_str.length - 1);
            search_run(0);
            return true;
        case "\x07":
        case "\x03":
            search_finish(true);
            return true;
        }
        if (ucs_length(keys) === 1 && keys >= ' ') {
            search_str += keys;
            search_run(0);
            return true;
        }
        search_finish(false);
        return false;
    }

    function history_search(dir) {
        var pos = cursor_pos;
        for (var i = 1; i <= history.length; i++) {
            var index = (history.length + i * dir + history_index) % history.length;
            if (history[index].substring(0, pos) == cmd.substring(0, pos)) {
                history_index = index;
                cmd = history[index];
                return;
            }
        }
    }

    function history_search_backward() {
        return history_search(-1);
    }

    function history_search_forward() {
        return history_search(1);
    }

    function delete_char_dir(dir) {
        var start, end;

        start = cursor_pos;
        if (dir < 0) {
            start--;
            while (is_trailing_surrogate(cmd.charAt(start)))
                start--;
        }
        end = start + 1;
        while (is_trailing_surrogate(cmd.charAt(end)))
            end++;

        if (start >= 0 && start < cmd.length) {
            if (last_fun === kill_region) {
                kill_region(start, end, dir);
            } else {
                cmd = cmd.substring(0, start) + cmd.substring(end);
                cursor_pos = start;
            }
        }
    }

    function delete_char() {
        delete_char_dir(1);
    }

    function control_d() {
        if (cmd.length == 0) {
            std.puts("\n");
            repl_cleanup();
            return -3;
        } else {
            delete_char_dir(1);
        }
    }

    function backward_delete_char() {
        delete_char_dir(-1);
    }

    function transpose_chars() {
        var pos = cursor_pos;
        if (cmd.length > 1 && pos > 0) {
            if (pos == cmd.length)
                pos--;
            cmd = cmd.substring(0, pos - 1) + cmd.substring(pos, pos + 1) +
                cmd.substring(pos - 1, pos) + cmd.substring(pos + 1);
            cursor_pos = pos + 1;
        }
    }

    function transpose_words() {
        var p1 = skip_word_backward(cursor_pos);
        var p2 = skip_word_forward(p1);
        var p4 = skip_word_forward(cursor_pos);
        var p3 = skip_word_backward(p4);

        if (p1 < p2 && p2 <= cursor_pos && cursor_pos <= p3 && p3 < p4) {
            cmd = cmd.substring(0, p1) + cmd.substring(p3, p4) +
            cmd.substring(p2, p3) + cmd.substring(p1, p2);
            cursor_pos = p4;
        }
    }

    function upcase_word() {
        var end = skip_word_forward(cursor_pos);
        cmd = cmd.substring(0, cursor_pos) +
            cmd.substring(cursor_pos, end).toUpperCase() +
            cmd.substring(end);
    }

    function downcase_word() {
        var end = skip_word_forward(cursor_pos);
        cmd = cmd.substring(0, cursor_pos) +
            cmd.substring(cursor_pos, end).toLowerCase() +
            cmd.substring(end);
    }

    function kill_region(start, end, dir) {
        var s = cmd.substring(start, end);
        if (last_fun !== kill_region)
            clip_board = s;
        else if (dir < 0)
            clip_board = s + clip_board;
        else
            clip_board = clip_board + s;

        cmd = cmd.substring(0, start) + cmd.substring(end);
        if (cursor_pos > end)
            cursor_pos -= end - start;
        else if (cursor_pos > start)
            cursor_pos = start;
        this_fun = kill_region;
    }

    function kill_line() {
        kill_region(cursor_pos, cmd.length, 1);
    }

    function backward_kill_line() {
        kill_region(0, cursor_pos, -1);
    }

    function kill_word() {
        kill_region(cursor_pos, skip_word_forward(cursor_pos), 1);
    }

    function backward_kill_word() {
        kill_region(skip_word_backward(cursor_pos), cursor_pos, -1);
    }

    function yank() {
        insert(clip_board);
    }

    function control_c() {
        if (eval_busy) {
            eval_gen++;
            eval_busy = false;
            busy_keys.length = 0;
            eof_seen = false;
            pending_lines.length = 0;
            level = 0;
            std.puts("^C\n");
            return -4;
        }
        if (cmd !== "" || mexpr !== "") {
            std.puts("^C\n");
            cmd = "";
            cursor_pos = 0;
            mexpr = "";
            pstate = "";
            level = 0;
            pending_lines.length = 0;
            return -4;
        }
        if (last_fun === control_c) {
            std.puts("\n");
            repl_exit(0);
        }
        std.puts("\n(Press Ctrl-C again to quit)\n");
        readline_print_prompt();
    }

    function clear_screen() {
        std.puts("\x1b[H\x1b[2J");
        readline_print_prompt();
    }

    function unix_word_rubout() {
        var pos = cursor_pos;
        while (pos > 0 && " \t".indexOf(cmd.charAt(pos - 1)) >= 0)
            pos--;
        while (pos > 0 && " \t".indexOf(cmd.charAt(pos - 1)) < 0)
            pos--;
        kill_region(pos, cursor_pos, -1);
    }

    function paste_begin() {
        paste_mode = true;
        paste_buf = "";
    }

    function submit_line() {
        accept_line();
        readline_cb(cmd);
    }

    function paste_end() {
        var text, lines, i;
        paste_mode = false;
        text = paste_buf.replace(/\r\n?/g, "\n");
        paste_buf = "";
        lines = text.split("\n");
        insert(lines[0]);
        for (i = 1; i < lines.length; i++)
            pending_lines.push(lines[i]);
        update();
        if (pending_lines.length > 0)
            submit_line();
    }

    function reset() {
        cmd = "";
        cursor_pos = 0;
    }

    function get_context_word(line, pos) {
        var s = "";
        while (pos > 0 && is_word(line[pos - 1])) {
            pos--;
            s = line[pos] + s;
        }
        return s;
    }
    function get_context_object(line, pos) {
        var obj, base, c;
        if (pos <= 0 || " ~!%^&*(-+={[|:;,<>?/".indexOf(line[pos - 1]) >= 0)
            return g;
        if (pos >= 2 && line[pos - 1] === ".") {
            pos--;
            obj = {};
            switch (c = line[pos - 1]) {
            case '\'':
            case '\"':
                return "a";
            case ']':
                return [];
            case '}':
                return {};
            case '/':
                return / /;
            default:
                if (is_word(c)) {
                    base = get_context_word(line, pos);
                    if (["true", "false", "null", "this"].includes(base) || !isNaN(+base))
                        return eval(base);
                    if (pos - base.length >= 3 && line[pos - base.length - 1] === '/')
                        return new RegExp('', base);
                    obj = get_context_object(line, pos - base.length);
                    if (obj === null || obj === void 0)
                        return obj;
                    if (obj === g && obj[base] === void 0)
                        return eval(base);
                    else
                        return obj[base];
                }
                return {};
            }
        }
        return void 0;
    }

    function string_start(line, pos) {
        var i, c, q = "", start = -1, esc = false;
        for (i = 0; i < pos; i++) {
            c = line[i];
            if (esc) {
                esc = false;
            } else if (c === "\\") {
                esc = true;
            } else if (q) {
                if (c === q) {
                    q = "";
                    start = -1;
                }
            } else if (c === '"' || c === "'" || c === "`") {
                q = c;
                start = i + 1;
            }
        }
        return q ? start : -1;
    }

    function path_completions(prefix) {
        var slash, dir, base, res, names, r = [], i, name, st;

        slash = prefix.lastIndexOf("/");
        dir = (slash < 0) ? "./" : prefix.substring(0, slash + 1);
        base = prefix.substring(slash + 1);
        res = os.readdir(dir);
        if (!res || res[1] !== 0)
            return null;
        names = res[0];
        for (i = 0; i < names.length; i++) {
            name = names[i];
            if (name === "." || name === "..")
                continue;
            if (name[0] === "." && base[0] !== ".")
                continue;
            if (!name.startsWith(base))
                continue;
            st = os.stat(dir + name);
            if (st && st[1] === 0 && (st[0].mode & os.S_IFMT) === os.S_IFDIR)
                name += "/";
            r.push(name);
        }
        r.sort();
        return { tab: r, pos: base.length, ctx: null };
    }

    function directive_completions(s) {
        var r = [], names = Object.keys(directives), i;
        for (i = 0; i < names.length; i++) {
            if (!directives[names[i]].alias && names[i].startsWith(s))
                r.push(names[i]);
        }
        r.sort();
        return { tab: r, pos: s.length, ctx: null };
    }

    function get_completions(line, pos) {
        var s, obj, ctx_obj, r, i, j, start, m;

        m = /^\\([a-zA-Z]*)$/.exec(line.substring(0, pos));
        if (m)
            return directive_completions(m[1]);

        m = /^\\load\s+/.exec(line.substring(0, pos));
        start = m ? m[0].length : string_start(line, pos);
        if (start >= 0) {
            r = path_completions(line.substring(start, pos));
            if (r)
                return r;
        }

        s = get_context_word(line, pos);
        ctx_obj = get_context_object(line, pos - s.length);
        r = [];
        for (i = 0, obj = ctx_obj; i < 10 && obj !== null && obj !== void 0; i++) {
            var props = Object.getOwnPropertyNames(obj);
            for (j = 0; j < props.length; j++) {
                var prop = props[j];
                if (typeof prop == "string" && ""+(+prop) != prop && prop.startsWith(s))
                    r.push(prop);
            }
            obj = Object.getPrototypeOf(obj);
        }
        if (r.length > 1) {
            function symcmp(a, b) {
                if (a[0] != b[0]) {
                    if (a[0] == '_')
                        return 1;
                    if (b[0] == '_')
                        return -1;
                }
                if (a < b)
                    return -1;
                if (a > b)
                    return +1;
                return 0;
            }
            r.sort(symcmp);
            for(i = j = 1; i < r.length; i++) {
                if (r[i] != r[i - 1])
                    r[j++] = r[i];
            }
            r.length = j;
        }
        return { tab: r, pos: s.length, ctx: ctx_obj };
    }

    var completion_max_display = 60;

    function completion() {
        var tab, res, s, i, j, len, t, max_width, col, n_cols, row, n_rows;
        var m, shown, hidden;

        completion_tabs = (last_fun === completion) ? completion_tabs + 1 : 1;
        res = get_completions(cmd, cursor_pos);
        tab = res.tab;
        if (tab.length === 0)
            return;
        s = tab[0];
        len = s.length;
        for(i = 1; i < tab.length; i++) {
            t = tab[i];
            for(j = 0; j < len; j++) {
                if (t[j] !== s[j]) {
                    len = j;
                    break;
                }
            }
        }
        for(i = res.pos; i < len; i++) {
            insert(s[i]);
        }
        if (completion_tabs >= 2 && tab.length == 1 && res.ctx) {
            m = res.ctx[tab[0]];
            if (typeof m == "function") {
                insert('(');
                if (m.length == 0)
                    insert(')');
            } else if (typeof m == "object") {
                insert('.');
            }
        }
        if (completion_tabs >= 2 && tab.length >= 2) {
            shown = tab;
            hidden = 0;
            if (tab.length > completion_max_display && completion_tabs < 3) {
                shown = tab.slice(0, completion_max_display);
                hidden = tab.length - completion_max_display;
            }
            max_width = 0;
            for(i = 0; i < shown.length; i++)
                max_width = Math.max(max_width, shown[i].length);
            max_width += 2;
            n_cols = Math.max(1, Math.floor((term_width + 1) / max_width));
            n_rows = Math.ceil(shown.length / n_cols);
            std.puts("\n");
            for (row = 0; row < n_rows; row++) {
                for (col = 0; col < n_cols; col++) {
                    i = col * n_rows + row;
                    if (i >= shown.length)
                        break;
                    s = shown[i];
                    if (col != n_cols - 1)
                        s = s.padEnd(max_width);
                    std.puts(s);
                }
                std.puts("\n");
            }
            if (hidden > 0)
                std.puts("... " + hidden + " more, Tab again to show all\n");
            readline_print_prompt();
        }
    }

    var commands = {
        "\x01":     beginning_of_line,
        "\x02":     backward_char,
        "\x03":     control_c,
        "\x04":     control_d,
        "\x05":     end_of_line,
        "\x06":     forward_char,
        "\x07":     abort,
        "\x08":     backward_delete_char,
        "\x09":     completion,
        "\x0a":     accept_line,
        "\x0b":     kill_line,
        "\x0c":     clear_screen,
        "\x0d":     accept_line,
        "\x0e":     next_history,
        "\x10":     previous_history,
        "\x11":     quoted_insert,
        "\x12":     search_backward,
        "\x13":     search_forward,
        "\x14":     transpose_chars,
        "\x15":     backward_kill_line,
        "\x17":     unix_word_rubout,
        "\x18":     reset,
        "\x19":     yank,
        "\x1bOA":   previous_history,
        "\x1bOB":   next_history,
        "\x1bOC":   forward_char,
        "\x1bOD":   backward_char,
        "\x1bOF":   forward_word,
        "\x1bOH":   backward_word,
        "\x1b[1;5C": forward_word,
        "\x1b[1;5D": backward_word,
        "\x1b[200~": paste_begin,
        "\x1b[1~":  beginning_of_line,
        "\x1b[3~":  delete_char,
        "\x1b[4~":  end_of_line,
        "\x1b[5~":  history_search_backward,
        "\x1b[6~":  history_search_forward,
        "\x1b[A":   previous_history,
        "\x1b[B":   next_history,
        "\x1b[C":   forward_char,
        "\x1b[D":   backward_char,
        "\x1b[F":   end_of_line,
        "\x1b[H":   beginning_of_line,
        "\x1b\x7f": backward_kill_word,
        "\x1bb":    backward_word,
        "\x1bd":    kill_word,
        "\x1bf":    forward_word,
        "\x1bk":    backward_kill_line,
        "\x1bl":    downcase_word,
        "\x1bt":    transpose_words,
        "\x1bu":    upcase_word,
        "\x7f":     backward_delete_char,
    };

    function dupstr(str, count) {
        var res = "";
        while (count-- > 0)
            res += str;
        return res;
    }

    var readline_keys;
    var readline_state;
    var readline_cb;

    function readline_print_prompt()
    {
        std.puts(prompt);
        term_cursor_x = ucs_length(prompt) % term_width;
        last_cmd = "";
        last_cursor_pos = 0;
    }

    function readline_start(defstr, cb) {
        term_update_size();
        cmd = defstr || "";
        cursor_pos = cmd.length;
        history_index = history.length;
        history_draft = "";
        readline_cb = cb;

        prompt = pstate;

        if (mexpr) {
            prompt += dupstr(" ", plen - prompt.length);
            prompt += ps2;
        } else {
            if (show_time) {
                var t = eval_time / 1000;
                prompt += t.toFixed(6) + " ";
            }
            plen = prompt.length;
            prompt += ps1;
        }
        readline_print_prompt();
        update();
        readline_state = 0;

        if (pending_lines.length > 0) {
            insert(pending_lines.shift());
            update();
            if (pending_lines.length > 0)
                submit_line();
        }
    }

    function handle_char(c1) {
        var c;
        c = String.fromCodePoint(c1);
        switch(readline_state) {
        case 0:
            if (c == '\x1b') {
                readline_keys = c;
                readline_state = 1;
            } else {
                handle_key(c);
            }
            break;
        case 1:
            readline_keys += c;
            if (c == '[') {
                readline_state = 2;
            } else if (c == 'O') {
                readline_state = 3;
            } else {
                handle_key(readline_keys);
                readline_state = 0;
            }
            break;
        case 2:
            readline_keys += c;
            if (!(c == ';' || (c >= '0' && c <= '9'))) {
                handle_key(readline_keys);
                readline_state = 0;
            }
            break;
        case 3:
            readline_keys += c;
            handle_key(readline_keys);
            readline_state = 0;
            break;
        }
    }

    function handle_key(keys) {
        var fun;

        if (eval_busy && keys !== "\x03") {
            busy_keys.push(keys);
            return;
        }
        if (paste_mode) {
            if (keys === "\x1b[201~") {
                paste_end();
            } else if (keys === "\x03") {
                paste_mode = false;
                paste_buf = "";
                std.puts("^C\n");
                cmd_readline_start();
            } else {
                paste_buf += keys;
            }
            return;
        }
        if (search_mode && search_key(keys))
            return;

        if (quote_flag) {
            if (ucs_length(keys) === 1)
                insert(keys);
            quote_flag = false;
        } else if (fun = commands[keys]) {
            this_fun = fun;
            switch (fun(keys)) {
            case -1:
                readline_cb(cmd);
                return;
            case -2:
                readline_cb(null);
                return;
            case -3:
                os.signal(os.SIGINT, null);
                os.setReadHandler(term_fd, null);
                return;
            case -4:
                cmd_readline_start();
                return;
            }
            last_fun = this_fun;
        } else if (ucs_length(keys) === 1 && keys >= ' ') {
            insert(keys);
            last_fun = insert;
        } else {
            alert();
        }

        cursor_pos = (cursor_pos < 0) ? 0 :
            (cursor_pos > cmd.length) ? cmd.length : cursor_pos;
        update();
    }

    var hex_mode = false;

    function number_to_string_hex(a) {
        var s;
        if (a < 0) {
            a = -a;
            s = "-";
        } else {
            s = "";
        }
        s += "0x" + a.toString(16);
        return s;
    }

    function extract_directive(a) {
        var pos;
        if (a[0] !== '\\')
            return "";
        for (pos = 1; pos < a.length; pos++) {
            if (!is_alpha(a[pos]))
                break;
        }
        return a.substring(1, pos);
    }

    var directives = {
        "h":     { help: "this help" },
        "help":  { alias: true },
        "load":  { help: "<file>  load and evaluate a script" },
        "x":     { help: "hexadecimal number display" },
        "d":     { help: "decimal number display" },
        "t":     { help: "toggle timing display" },
        "clear": { help: "clear the terminal" },
        "q":     { help: "exit" },
    };

    function handle_directive(cmd, expr) {
        var filename;

        if (!Object.prototype.hasOwnProperty.call(directives, cmd)) {
            std.puts("Unknown directive: \\" + cmd + "\n");
            return false;
        }
        if (cmd === "h" || cmd === "help") {
            help();
        } else if (cmd === "load") {
            filename = expr.substring(cmd.length + 1).trim();
            if (filename === "") {
                std.puts("\\load: missing file name\n");
                return false;
            }
            if (filename.lastIndexOf(".") <= filename.lastIndexOf("/"))
                filename += ".js";
            try {
                std.loadScript(filename);
            } catch (e) {
                print_error(e);
            }
            return false;
        } else if (cmd === "x") {
            hex_mode = true;
        } else if (cmd === "d") {
            hex_mode = false;
        } else if (cmd === "t") {
            show_time = !show_time;
        } else if (cmd === "clear") {
            std.puts("\x1b[H\x1b[J");
        } else if (cmd === "q") {
            repl_exit(0);
        }
        return true;
    }

    function help() {
        var names = Object.keys(directives), name, mark, i;
        for (i = 0; i < names.length; i++) {
            name = names[i];
            if (directives[name].alias)
                continue;
            mark = ((name === "x" && hex_mode) ||
                    (name === "d" && !hex_mode) ||
                    (name === "t" && show_time)) ? "*" : " ";
            std.puts("\\" + name + dupstr(" ", 10 - name.length) + mark + " " +
                     directives[name].help + "\n");
        }
        std.puts("\nkeys  TAB complete       ^R search history    ^C discard line\n" +
                 "      ^A/^E start/end    ^W/^U kill word/line  ^L clear  ^D exit\n" +
                 "\nIncomplete input (open bracket, trailing operator) continues\n" +
                 "on the next line; ^C cancels it.\n" +
                 "The std and os modules are preloaded as globals.\n" +
                 "_ is the last result, _err the last error.\n");
    }

    function cmd_start() {
        history_load();
        std.puts('DynaJS REPL - Type "\\h" or "?" for help, "\\q" or Ctrl-D to exit\n');

        cmd_readline_start();
    }

    function cmd_readline_start() {
        readline_start(dupstr("    ", level), readline_handle_cmd);
    }

    function readline_handle_cmd(expr) {
        if (!handle_cmd(expr)) {
            cmd_readline_start();
        }
    }

    function need_more_input(expr) {
        var t = expr.replace(/\s+$/, "");
        if (t === "")
            return false;
        if (/\+\+$|--$/.test(t))
            return false;
        if (/[+\-*%&,?:=<>^|~(]$/.test(t))
            return true;
        if (/(^|[^0-9.])\.$/.test(t))
            return true;
        if (/(^|[^_\w$])(return|typeof|instanceof|in|of|new|delete|void|throw|case|default|do|else|yield|await)$/.test(t))
            return true;
        return false;
    }

    function handle_cmd(expr) {
        var colorstate, cmd;

        if (expr === null) {
            expr = "";
            return false;
        }
        if (expr === "?") {
            help();
            return false;
        }
        if (expr === ".exit") {
            repl_exit(0);
        }
        cmd = extract_directive(expr);
        if (cmd.length > 0) {
            if (!handle_directive(cmd, expr)) {
                return false;
            }
            expr = expr.substring(cmd.length + 1);
        }
        if (expr === "")
            return false;

        if (mexpr)
            expr = mexpr + '\n' + expr;
        colorstate = colorize_js(expr);
        pstate = colorstate[0];
        level = colorstate[1];
        if (pstate) {
            mexpr = expr;
            return false;
        }
        if (/^\s*(?:\/\/[^\n]*|\/\*[\s\S]*?\*\/)+\s*$/.test(expr)) {
            mexpr = "";
            return false;
        }
        if (need_more_input(expr)) {
            mexpr = expr;
            return false;
        }
        mexpr = "";

        eval_and_print_start(expr);

        return true;
    }

    function eval_and_print_start(expr) {
        var result, gen = ++eval_gen;

        eval_busy = true;
        try {
            eval_start_time = os.now();
            result = std.evalScript(expr, { backtrace_barrier: true, async: true });
            result.then(function (r) { if (gen === eval_gen) print_eval_result(r); },
                        function (e) { if (gen === eval_gen) print_eval_error(e); });
        } catch (error) {
            if (gen === eval_gen)
                print_eval_error(error);
        }
    }

    function print_eval_result(result) {
        var gen = eval_gen;

        result = result.value;
        if (result instanceof Promise) {
            result.then(function (v) { if (gen === eval_gen) print_result_value(v); },
                        function (e) { if (gen === eval_gen) print_eval_error(e); });
            return;
        }
        print_result_value(result);
    }

    function print_result_value(result) {
        var default_print = true;

        eval_busy = false;
        eval_time = os.now() - eval_start_time;
        std.puts(show_colors ? colors[styles.result] : "");
        if (hex_mode) {
            if (typeof result == "number" &&
                result === Math.floor(result)) {
                std.puts(number_to_string_hex(result));
                default_print = false;
            } else if (typeof result == "bigint") {
                std.puts(number_to_string_hex(result));
                std.puts("n");
                default_print = false;
            }
        }
        if (default_print) {
            std.__printObject(result);
        }
        std.puts("\n");
        std.puts(show_colors ? colors.none : "");
        g._ = result;

        handle_cmd_end();
    }

    function print_error(error) {
        std.puts(show_colors ? colors[styles.error_msg] : "");
        if (!(error instanceof Error))
            std.puts("Throw: ");
        std.__printObject(error);
        std.puts("\n");
        std.puts(show_colors ? colors.none : "");
    }

    function print_eval_error(error) {
        eval_busy = false;
        g._err = error;
        print_error(error);

        handle_cmd_end();
    }

    function handle_cmd_end() {
        level = 0;
        if (eof_seen) {
            repl_exit(0);
        }
        std.gc();
        cmd_readline_start();
        while (busy_keys.length > 0 && !eval_busy)
            handle_key(busy_keys.shift());
    }

    function colorize_js(str) {
        var i, c, start, n = str.length;
        var style, state = "", level = 0;
        var primary, can_regex = 1;
        var r = [];

        function push_state(c) { state += c; }
        function last_state(c) { return state.substring(state.length - 1); }
        function pop_state(c) {
            var c = last_state();
            state = state.substring(0, state.length - 1);
            return c;
        }

        function parse_block_comment() {
            style = 'comment';
            push_state('/');
            for (i++; i < n - 1; i++) {
                if (str[i] == '*' && str[i + 1] == '/') {
                    i += 2;
                    pop_state('/');
                    break;
                }
            }
        }

        function parse_line_comment() {
            style = 'comment';
            for (i++; i < n; i++) {
                if (str[i] == '\n') {
                    break;
                }
            }
        }

        function parse_string(delim) {
            style = 'string';
            push_state(delim);
            while (i < n) {
                c = str[i++];
                if (c == '\n') {
                    style = 'error';
                    continue;
                }
                if (c == '\\') {
                    if (i >= n)
                        break;
                    i++;
                } else
                if (c == delim) {
                    pop_state();
                    break;
                }
            }
        }

        function parse_regex() {
            style = 'regex';
            push_state('/');
            while (i < n) {
                c = str[i++];
                if (c == '\n') {
                    style = 'error';
                    continue;
                }
                if (c == '\\') {
                    if (i < n) {
                        i++;
                    }
                    continue;
                }
                if (last_state() == '[') {
                    if (c == ']') {
                        pop_state()
                    }
                    continue;
                }
                if (c == '[') {
                    push_state('[');
                    if (str[i] == '[' || str[i] == ']')
                        i++;
                    continue;
                }
                if (c == '/') {
                    pop_state();
                    while (i < n && is_word(str[i]))
                        i++;
                    break;
                }
            }
        }

        function parse_number() {
            style = 'number';
            while (i < n && (is_word(str[i]) || (str[i] == '.' && (i == n - 1 || str[i + 1] != '.')))) {
                i++;
            }
        }

        var js_keywords = "|" +
            "break|case|catch|continue|debugger|default|delete|do|" +
            "else|finally|for|function|if|in|instanceof|new|" +
            "return|switch|this|throw|try|typeof|while|with|" +
            "class|const|enum|import|export|extends|super|" +
            "implements|interface|let|package|private|protected|" +
            "public|static|yield|" +
            "undefined|null|true|false|Infinity|NaN|" +
            "eval|arguments|" +
            "await|";

        var js_no_regex = "|this|super|undefined|null|true|false|Infinity|NaN|arguments|";
        var js_types = "|void|var|";

        function parse_identifier() {
            can_regex = 1;

            while (i < n && is_word(str[i]))
                i++;

            var w = '|' + str.substring(start, i) + '|';

            if (js_keywords.indexOf(w) >= 0) {
                style = 'keyword';
                if (js_no_regex.indexOf(w) >= 0)
                    can_regex = 0;
                return;
            }

            var i1 = i;
            while (i1 < n && str[i1] == ' ')
                i1++;

            if (i1 < n && str[i1] == '(') {
                style = 'function';
                return;
            }

            if (js_types.indexOf(w) >= 0) {
                style = 'type';
                return;
            }

            style = 'identifier';
            can_regex = 0;
        }

        function set_style(from, to) {
            while (r.length < from)
                r.push('default');
            while (r.length < to)
                r.push(style);
        }

        for (i = 0; i < n;) {
            style = null;
            start = i;
            switch (c = str[i++]) {
            case ' ':
            case '\t':
            case '\r':
            case '\n':
                continue;
            case '+':
            case '-':
                if (i < n && str[i] == c) {
                    i++;
                    continue;
                }
                can_regex = 1;
                continue;
            case '/':
                if (i < n && str[i] == '*') {
                    parse_block_comment();
                    break;
                }
                if (i < n && str[i] == '/') {
                    parse_line_comment();
                    break;
                }
                if (can_regex) {
                    parse_regex();
                    can_regex = 0;
                    break;
                }
                can_regex = 1;
                continue;
            case '\'':
            case '\"':
            case '`':
                parse_string(c);
                can_regex = 0;
                break;
            case '(':
            case '[':
            case '{':
                can_regex = 1;
                level++;
                push_state(c);
                continue;
            case ')':
            case ']':
            case '}':
                can_regex = 0;
                if (level > 0 && is_balanced(last_state(), c)) {
                    level--;
                    pop_state();
                    continue;
                }
                style = 'error';
                break;
            default:
                if (is_digit(c)) {
                    parse_number();
                    can_regex = 0;
                    break;
                }
                if (is_word(c) || c == '$') {
                    parse_identifier();
                    break;
                }
                can_regex = 1;
                continue;
            }
            if (style)
                set_style(start, i);
        }
        set_style(n, n);
        return [ state, level, r ];
    }

    termInit();

    cmd_start();

})(globalThis);
