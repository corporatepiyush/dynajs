#ifndef DYN_CHILDREG_H
#define DYN_CHILDREG_H

#include <signal.h>
#include <sys/types.h>

#define DYN_CHILDREG_MAX 4096
#define DYN_CHILDREG_GRACE_MS 2000

void dyn_childreg_block(sigset_t* old);
void dyn_childreg_unblock(const sigset_t* old);
void dyn_childreg_add(pid_t pgid);
void dyn_childreg_del(pid_t pgid);
int dyn_childreg_is_handler(const struct sigaction* sa);
void dyn_childreg_kill_all(int grace_ms);
void dyn_childreg_kill_now(void);

#endif
