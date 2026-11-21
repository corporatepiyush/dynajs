#ifndef SFTP_INTERNAL_H
#define SFTP_INTERNAL_H

#include "dyna-nat.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_SFTP) && defined(CONFIG_NATIVE_MODULE_SSH)

#include "../ssh/ssh-internal.h"

#include <sys/stat.h>

#define SFTP_INIT 1
#define SFTP_VERSION 2
#define SFTP_OPEN 3
#define SFTP_CLOSE 4
#define SFTP_READ 5
#define SFTP_WRITE 6
#define SFTP_LSTAT 7
#define SFTP_FSTAT 8
#define SFTP_SETSTAT 9
#define SFTP_FSETSTAT 10
#define SFTP_OPENDIR 11
#define SFTP_READDIR 12
#define SFTP_REMOVE 13
#define SFTP_MKDIR 14
#define SFTP_RMDIR 15
#define SFTP_REALPATH 16
#define SFTP_STAT 17
#define SFTP_RENAME 18
#define SFTP_READLINK 19
#define SFTP_SYMLINK 20
#define SFTP_STATUS 101
#define SFTP_HANDLE 102
#define SFTP_DATA 103
#define SFTP_NAME 104
#define SFTP_ATTRS 105
#define SFTP_EXTENDED 200
#define SFTP_EXTENDED_REPLY 201

#define SFTP_OK 0
#define SFTP_EOF 1
#define SFTP_NO_SUCH_FILE 2
#define SFTP_PERMISSION_DENIED 3
#define SFTP_FAILURE 4
#define SFTP_BAD_MESSAGE 5
#define SFTP_NO_CONNECTION 6
#define SFTP_CONNECTION_LOST 7
#define SFTP_OP_UNSUPPORTED 8

#define SFTP_FXF_READ 0x01
#define SFTP_FXF_WRITE 0x02
#define SFTP_FXF_APPEND 0x04
#define SFTP_FXF_CREAT 0x08
#define SFTP_FXF_TRUNC 0x10
#define SFTP_FXF_EXCL 0x20

#define SFTP_A_SIZE 0x01
#define SFTP_A_UIDGID 0x02
#define SFTP_A_PERM 0x04
#define SFTP_A_TIME 0x08
#define SFTP_A_EXT 0x80000000u

#define SFTP_MAX_MSG (256u * 1024u)
#define SFTP_MAX_READ (SFTP_MAX_MSG - 1024u)
#define SFTP_CLI_MAX_MSG (1024u * 1024u)

typedef struct {
    uint32_t flags;
    uint64_t size;
    uint32_t uid, gid, perm, atime, mtime;
} sftp_attr_t;

int sftp_attr_get(sshr_t* r, sftp_attr_t* a);
int sftp_attr_put(sshb_t* b, const sftp_attr_t* a);
void sftp_attr_stat(const struct stat* st, sftp_attr_t* a);
JSValue sftp_attr_js(JSContext* ctx, const sftp_attr_t* a);
int sftp_attr_from_js(JSContext* ctx, JSValueConst o, sftp_attr_t* a);
void sftp_longname(const sftp_attr_t* a, const char* name, size_t nl, sshb_t* out);
const char* sftp_status_name(uint32_t code);
JSValue sftp_error(JSContext* ctx, uint32_t code, const char* msg, size_t n);
int sftp_pkt_begin(sshb_t* b, uint8_t type);
void sftp_pkt_end(sshb_t* b);
int sftp_send(ssh_chan_t* ch, sshb_t* b);

int sftp_server_init(JSContext* ctx, JSModuleDef* m);
int sftp_client_init(JSContext* ctx, JSModuleDef* m);

#endif
#endif
