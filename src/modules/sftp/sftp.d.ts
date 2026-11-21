/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:sftp
/** dyna:sftp — SFTP version 3 (draft-ietf-secsh-filexfer-02, as OpenSSH speaks it) over dyna:ssh, client and server. Both sides frame
 *  packets in C on the SSH channel; no JS runs per packet except the server's optional allow hook. Packets are capped at 256 KiB
 *  (a bigger or malformed one from the peer ends the session); a read asks for at most 261120 bytes. Paths are UTF-8 strings; SYMLINK
 *  carries the link target first and the link path second, the order OpenSSH uses (the draft lists them the other way round), so
 *  both sides interoperate with OpenSSH. No extensions are advertised or used (v3 RENAME refuses an existing target). Errors are
 *  Error objects whose message starts "SFTP: ", with `code` (the status name: NO_SUCH_FILE, PERMISSION_DENIED, FAILURE, EOF,
 *  BAD_MESSAGE, NO_CONNECTION, CONNECTION_LOST, OP_UNSUPPORTED) and `status` (its number). */
declare module "dyna:sftp" {
    /** File attributes; a field is present only when the server sent it. isFile / isDirectory / isSymbolicLink come from mode's type bits
     *  (all false without mode). Times are seconds since the epoch. */
    interface SFTPAttrs {
        size?: number;
        uid?: number;
        gid?: number;
        mode?: number;
        atime?: number;
        mtime?: number;
        readonly isFile: boolean;
        readonly isDirectory: boolean;
        readonly isSymbolicLink: boolean;
    }

    /** Attributes to set; a strict bag. uid and gid go together, and so do atime and mtime. size truncates or extends; mode is
     *  masked to 07777 by the server. Integers: size 0..2^53-1, the others 0..2^32-1. */
    interface SFTPSetAttrs {
        size?: number;
        uid?: number;
        gid?: number;
        mode?: number;
        atime?: number;
        mtime?: number;
    }

    interface SFTPEntry {
        filename: string;
        /** The server's ls -l style line (this module's server: mode, links, numeric uid and gid, size, UTC mtime, name). */
        longname: string;
        attrs: SFTPAttrs;
    }

    /** dyna extension: SFTPOpenOptions.dyna, a strict bag. */
    interface SFTPClientMetaOptions {
        /** Requests in flight per streaming file (read-ahead and write-behind), 1..1024; default 64 (the OpenSSH sftp default). */
        maxInflight?: number;
        /** Bytes per READ / WRITE request of the streaming calls, 1024..261120; default 32768. */
        chunkBytes?: number;
    }

    /** Options for SFTPClient.open; a strict bag. */
    interface SFTPOpenOptions {
        dyna?: SFTPClientMetaOptions;
    }

    /** "r" read; "r+" read and write; "w" write, create, truncate; "wx" as w but the file must not exist; "w+" / "wx+" add read;
     *  "a" append (create if missing); "ax" append, must not exist; "a+" / "ax+" add read. */
    type SFTPOpenFlags = "r" | "r+" | "w" | "wx" | "w+" | "wx+" | "a" | "ax" | "a+" | "ax+";

    /** An open remote file. read() and write() are streaming cursors, each starting at 0 and independent of the other:
     *  read() keeps up to maxInflight READ requests of chunkBytes ahead and returns the bytes in file order, filling a short reply by
     *  re-asking for the rest before moving on (so a server that caps its reads below chunkBytes is read correctly); write() splits
     *  the data into chunkBytes WRITE requests and resolves once they are all sent with at most maxInflight unanswered, so a loop of
     *  awaited writes keeps the pipe full; a failed write is reported by the next write() and by close(). One read() and one write()
     *  may be pending at a time. A file collected without close() keeps its server handle until the session ends; one with a read(), write() or close()
     *  pending is a GC root until that settles. */
    interface SFTPFile {
        /** The next bytes in file order (at most chunkBytes), or null at end of file. */
        read(): Promise<Uint8Array | null>;
        /** One READ at `position` for at most `length` bytes (0..261120); null at end of file. The reply may be shorter. */
        pread(position: number, length: number): Promise<Uint8Array | null>;
        write(data: string | ArrayBuffer | ArrayBufferView): Promise<void>;
        /** One WRITE of `data` (at most 261120 bytes) at `position`, resolved when the server acknowledges it. */
        pwrite(position: number, data: string | ArrayBuffer | ArrayBufferView): Promise<void>;
        /** FSTAT / FSETSTAT on the handle. */
        stat(): Promise<SFTPAttrs>;
        setstat(attrs: SFTPSetAttrs): Promise<void>;
        /** Waits for every write to be acknowledged, then CLOSE; rejects with the first write error if one failed. A pending read()
         *  rejects. A second close() rejects. */
        close(): Promise<void>;
        /** Bytes handed to write() so far. */
        readonly writePosition: number;
    }

    interface SFTPDir {
        /** The next batch of entries ("." and ".." included when the server lists them), or null at the end. */
        read(): Promise<SFTPEntry[] | null>;
        close(): Promise<void>;
    }

    /** An SFTP session on an "sftp" subsystem channel of a connected SSHClient (one channel of its dyna.maxChannels). Every method
     *  rejects with NO_CONNECTION once the session is closed. */
    class SFTPClient {
        /** new SFTPClient() is a TypeError: use open. */
        constructor();
        /** Opens the channel, sends INIT and resolves after the server's VERSION (3 or more; older is refused). A closed or
         *  non-SSHClient argument is a TypeError. */
        static open(ssh: import("dyna:ssh").SSHClient, options?: SFTPOpenOptions): Promise<SFTPClient>;
        /** The server's protocol version. */
        readonly version: number;
        /** Requests sent, payload bytes read and written, the most requests ever in flight at once, and those in flight now. */
        readonly stats: { requests: number; bytesRead: number; bytesWritten: number; maxInflight: number; inflight: number };
        stat(path: string): Promise<SFTPAttrs>;
        lstat(path: string): Promise<SFTPAttrs>;
        setstat(path: string, attrs: SFTPSetAttrs): Promise<void>;
        realpath(path: string): Promise<string>;
        readlink(path: string): Promise<string>;
        /** Creates `path` as a symbolic link whose content is `target`. */
        symlink(target: string, path: string): Promise<void>;
        mkdir(path: string, attrs?: SFTPSetAttrs): Promise<void>;
        rmdir(path: string): Promise<void>;
        remove(path: string): Promise<void>;
        /** v3 RENAME: fails when `to` exists. */
        rename(from: string, to: string): Promise<void>;
        /** OPENDIR, READDIR until EOF, CLOSE: every entry. */
        readdir(path: string): Promise<SFTPEntry[]>;
        opendir(path: string): Promise<SFTPDir>;
        /** flags default "r"; attrs apply when the file is created (mode default 0666 before the server's umask). */
        open(path: string, flags?: SFTPOpenFlags, attrs?: SFTPSetAttrs): Promise<SFTPFile>;
        /** The whole file through the pipelined read path. */
        readFile(path: string): Promise<Uint8Array>;
        /** Create or truncate, then the pipelined write path, then CLOSE; rejects with the first failure. */
        writeFile(path: string, data: string | ArrayBuffer | ArrayBufferView, attrs?: SFTPSetAttrs): Promise<void>;
        /** Rejects every pending request (CONNECTION_LOST), closes the channel and resolves once it is closed. */
        close(): Promise<void>;
    }

    /** dyna extension: SFTPServeOptions.dyna, a strict bag (safety caps). */
    interface SFTPServerMetaOptions {
        /** Open file and directory handles per session, 1..4096; default 64. OPEN / OPENDIR past it fail with FAILURE. */
        maxHandles?: number;
        /** Largest READ reply, 1024..261120; default 261120. A bigger request gets a shorter reply (the client re-asks). */
        maxReadBytes?: number;
    }

    /** What the allow hook is asked. `path` is the virtual path ("/" is the root) after resolving symlinks the operation follows
     *  (open, stat, setstat, opendir); for lstat, remove, rmdir, mkdir, readlink, rename and symlink it is the entry itself. `target`
     *  is the rename destination, or the symlink's content as sent. `write` is true for every operation that can change the tree. */
    interface SFTPAllowRequest {
        op: "open" | "stat" | "lstat" | "setstat" | "opendir" | "remove" | "mkdir" | "rmdir" | "readlink" | "rename" | "symlink";
        path: string;
        target?: string;
        user: string;
        write: boolean;
    }

    /** Options for SFTPServer.serve; a strict bag. */
    interface SFTPServeOptions {
        /** The directory the session sees as "/". Every path is resolved component by component from it with openat(O_NOFOLLOW): ".."
         *  never climbs above it, and a symlink is followed by this resolver (at most 40 per path), never by the kernel, with an
         *  absolute target read relative to the root, so neither ".." nor a symlink (in the tree, or made through SYMLINK) reaches a file
         *  outside it. A final component swapped for a symlink between the check and the operation makes the operation fail (O_NOFOLLOW /
         *  AT_SYMLINK_NOFOLLOW). Symlinks are stored with their raw content, so a process outside the session that follows them gets the
         *  kernel's meaning. Only regular files can be opened. */
        root: string;
        /** Default false. true refuses OPEN with any write flag, SETSTAT, FSETSTAT, REMOVE, MKDIR, RMDIR, RENAME and SYMLINK with
         *  PERMISSION_DENIED. FSETSTAT is refused on any handle not opened for writing, whatever readOnly says. */
        readOnly?: boolean;
        /** Called before every path operation (not for READ/WRITE/FSTAT on an open handle, nor REALPATH). true allows; false, a throw,
         *  a rejection or a non-boolean denies with PERMISSION_DENIED. A Promise pauses the session: later requests wait (and the SSH
         *  window stops being granted) until it settles. The approval is bound to the request it was asked for: the operation, resolved path and target are
         *  checked again when the request runs, and if any changed (a rename or symlink swap meanwhile) the request is denied. */
        allow?: (request: SFTPAllowRequest) => boolean | Promise<boolean>;
        dyna?: SFTPServerMetaOptions;
    }

    interface SFTPServeResult {
        requests: number;
        bytesRead: number;
        bytesWritten: number;
        /** Requests refused by readOnly or allow. */
        denied: number;
        /** Why the session ended abnormally (a malformed packet, the connection dying), or null. */
        error: string | null;
    }

    const SFTPServer: {
        /** Takes over an SSHSession, normally of type "subsystem" (command "sftp"); an "exec" or "shell" session is accepted too, for a
         *  custom transport: its stdin no longer delivers data and this server answers
         *  the client. Call it synchronously inside onSession and return its promise from onSession, so the session exits 0 when the
         *  client leaves. The promise resolves (never rejects) when the client sends EOF or the channel closes; every handle is closed
         *  then. A session that already received data, is closing, or is already served, an unknown option key, a bad cap or a root
         *  that cannot be opened as a directory is a TypeError (RangeError for a cap out of range). */
        serve(session: import("dyna:ssh").SSHSession, options: SFTPServeOptions): Promise<SFTPServeResult>;
    };
}
