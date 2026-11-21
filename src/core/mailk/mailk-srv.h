#ifndef MAILK_SRV_H
#define MAILK_SRV_H

#include "../dyn-mailk.h"

enum { V_CMD, V_DATA, V_BDAT, V_AUTH, V_OERR, V_TLS, V_CLOSED };

int mailk_srv_ieqn(const char* a, size_t n, const char* lit);
void mailk_srv_wipe(dyn_sb_t* b);
int mailk_srv_reply(mailk_srv_t* s, int code, const char* esc, const char* text);
int mailk_srv_bad(mailk_srv_t* s, int code, const char* esc, const char* text);
int mailk_srv_close(mailk_srv_t* s);
int mailk_srv_reset_tx(mailk_srv_t* s);
int mailk_srv_want(mailk_srv_t* s, int p);
int mailk_srv_accept(mailk_srv_t* s, int p, int code, const char* esc, const char* text);
int mailk_srv_mechs_line(mailk_srv_t* s);
int mailk_srv_cmd_auth(mailk_srv_t* s, const char* a, size_t n);
int mailk_srv_auth_line(mailk_srv_t* s, const char* l, size_t n);
int mailk_srv_auth_failed(mailk_srv_t* s, int code, const char* esc, const char* text);
int mailk_srv_auth_reject(mailk_srv_t* s, int code, const char* esc, const char* text);
int mailk_srv_cmd_data(mailk_srv_t* s, size_t argn);
int mailk_srv_cmd_bdat(mailk_srv_t* s, const char* a, size_t n);
size_t mailk_srv_body_feed(mailk_srv_t* s, const uint8_t* p, size_t n);

#endif
