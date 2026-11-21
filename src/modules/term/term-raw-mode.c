#include "term-internal.h"

#if defined(CONFIG_NATIVE_MODULES) && defined(CONFIG_NATIVE_MODULE_TERM)

static void dyn_inq_push(unsigned char c)
{
    if (dyn_inq_n >= DYN_INQ_SIZE)
        return;
    dyn_inq[(dyn_inq_head + dyn_inq_n) & (DYN_INQ_SIZE - 1)] = c;
    dyn_inq_n++;
}

#if !defined(_WIN32)

#include "core/dyn-childreg.h"

#define DYN_RAW_NSIG 3
static const int dyn_raw_sigs[DYN_RAW_NSIG] = { SIGTERM, SIGHUP, SIGQUIT };
static struct termios dyn_raw_saved;
static volatile sig_atomic_t dyn_raw_active;
static struct sigaction dyn_raw_old[DYN_RAW_NSIG];
static unsigned char dyn_raw_hooked[DYN_RAW_NSIG];

static void dyn_raw_restore_atexit(void)
{
    if (dyn_raw_active)
        tcsetattr(0, TCSANOW, &dyn_raw_saved);
}

static void dyn_raw_on_signal(int sig)
{
    int i;
    if (dyn_raw_active) {
        dyn_raw_active = 0;
        tcsetattr(0, TCSANOW, &dyn_raw_saved);
    }
    dyn_childreg_kill_all(DYN_CHILDREG_GRACE_MS);
    for (i = 0; i < DYN_RAW_NSIG; i++)
        if (dyn_raw_sigs[i] == sig && dyn_raw_hooked[i]) {
            sigaction(sig, &dyn_raw_old[i], NULL);
            raise(sig);
            return;
        }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void dyn_raw_guard(const struct termios* saved)
{
    static int atexit_done;
    int i;
    dyn_raw_saved = *saved;
    dyn_raw_active = 1;
    if (!atexit_done) {
        atexit_done = 1;
        atexit(dyn_raw_restore_atexit);
    }
    for (i = 0; i < DYN_RAW_NSIG; i++) {
        struct sigaction sa;
        dyn_raw_hooked[i] = 0;
        if (sigaction(dyn_raw_sigs[i], NULL, &dyn_raw_old[i]) != 0
            || (dyn_raw_old[i].sa_handler != SIG_DFL && !dyn_childreg_is_handler(&dyn_raw_old[i])))
            continue;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = dyn_raw_on_signal;
        sigemptyset(&sa.sa_mask);
        if (sigaction(dyn_raw_sigs[i], &sa, NULL) == 0)
            dyn_raw_hooked[i] = 1;
    }
}

static void dyn_raw_unguard(void)
{
    int i;
    dyn_raw_active = 0;
    for (i = 0; i < DYN_RAW_NSIG; i++)
        if (dyn_raw_hooked[i]) {
            sigaction(dyn_raw_sigs[i], &dyn_raw_old[i], NULL);
            dyn_raw_hooked[i] = 0;
        }
}

int dyn_raw_on(dyn_termios_t* saved)
{
    struct termios t;
    if (tcgetattr(0, saved) != 0)
        return 0;
    t = *saved;
    t.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
    t.c_iflag &= ~(IXON | ICRNL | INLCR | IGNCR | BRKINT);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSADRAIN, &t) != 0)
        return 0;
    dyn_raw_guard(saved);
    return 1;
}
void dyn_raw_off(dyn_termios_t* saved, int on)
{
    if (!on)
        return;
    for (;;) {
        unsigned char buf[512];
        fd_set rfds;
        struct timeval tv;
        size_t room = DYN_INQ_SIZE - dyn_inq_n;
        ssize_t r, i;
        if (room == 0)
            break;
        FD_ZERO(&rfds);
        FD_SET(0, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        if (select(1, &rfds, NULL, NULL, &tv) <= 0)
            break;
        r = read(0, buf, room < sizeof buf ? room : sizeof buf);
        if (r <= 0)
            break;
        for (i = 0; i < r; i++)
            dyn_inq_push(buf[i]);
    }
    tcsetattr(0, TCSADRAIN, saved);
    dyn_raw_unguard();
}
#else
int dyn_raw_on(dyn_termios_t* saved)
{
    (void)saved;
    return 0;
}
void dyn_raw_off(dyn_termios_t* saved, int on)
{
    (void)saved;
    (void)on;
}
#endif

#endif
