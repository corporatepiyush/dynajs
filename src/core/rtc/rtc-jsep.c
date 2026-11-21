#include "rtc-jsep.h"

#include <stddef.h>

typedef struct {
    unsigned profile;
    unsigned iop;
    unsigned level;
} rtc_h264_plid;

typedef struct {
    rtc_sdp* ans;
    int oom;
} rtc_jctx;

static const struct {
    uint8_t pt;
    uint8_t channels;
    uint32_t clock;
    const char* name;
} rtc_static_pts[] = {
    { 0, 1, 8000, "PCMU" },
    { 8, 1, 8000, "PCMA" },
    { 9, 1, 8000, "G722" },
};

static const struct {
    uint8_t idc;
    uint8_t mask;
    uint8_t value;
    uint8_t profile;
} rtc_h264_profiles[] = {
    { 0x42, 0x4F, 0x40, RTC_H264_CONSTRAINED_BASELINE },
    { 0x4D, 0x8F, 0x80, RTC_H264_CONSTRAINED_BASELINE },
    { 0x58, 0xCF, 0xC0, RTC_H264_CONSTRAINED_BASELINE },
    { 0x42, 0x4F, 0x00, RTC_H264_BASELINE },
    { 0x58, 0xCF, 0x80, RTC_H264_BASELINE },
    { 0x4D, 0xAF, 0x00, RTC_H264_MAIN },
    { 0x58, 0xCF, 0x00, RTC_H264_EXTENDED },
    { 0x64, 0xFF, 0x00, RTC_H264_HIGH },
    { 0x6E, 0xFF, 0x00, RTC_H264_HIGH10 },
    { 0x7A, 0xFF, 0x00, RTC_H264_HIGH422 },
    { 0xF4, 0xFF, 0x00, RTC_H264_HIGH444 },
    { 0x6E, 0xFF, 0x10, RTC_H264_HIGH10_INTRA },
    { 0x7A, 0xFF, 0x10, RTC_H264_HIGH422_INTRA },
    { 0xF4, 0xFF, 0x10, RTC_H264_HIGH444_INTRA },
    { 0x2C, 0xFF, 0x10, RTC_H264_CAVLC444_INTRA },
};

static int rtc_ieq(const char* a, const char* b)
{
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z')
            x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z')
            y = (char)(y + 32);
        if (x != y)
            return 0;
        if (!x)
            return 1;
    }
}

static int rtc_hexd(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int rtc_h264_plid_get(const char* fmtp, rtc_h264_plid* o)
{
    const char* v;
    size_t n, i;
    unsigned x = 0;
    o->profile = 0x42;
    o->iop = 0;
    o->level = 10;
    if (!rtc_sdp_fmtp_get(fmtp, "profile-level-id", &v, &n))
        return 0;
    if (n != 6)
        return -1;
    for (i = 0; i < 6; i++) {
        int d = rtc_hexd(v[i]);
        if (d < 0)
            return -1;
        x = x << 4 | (unsigned)d;
    }
    o->profile = x >> 16;
    o->iop = x >> 8 & 0xFFu;
    o->level = x & 0xFFu;
    return 0;
}

int rtc_jsep_h264_profile(const char* fmtp)
{
    rtc_h264_plid p;
    size_t i;
    if (rtc_h264_plid_get(fmtp, &p) < 0)
        return -1;
    for (i = 0; i < sizeof(rtc_h264_profiles) / sizeof(rtc_h264_profiles[0]); i++)
        if (rtc_h264_profiles[i].idc == p.profile && (p.iop & rtc_h264_profiles[i].mask) == rtc_h264_profiles[i].value)
            return rtc_h264_profiles[i].profile;
    return 0;
}

static int rtc_h264_base(unsigned profile)
{
    return profile == 66 || profile == 77 || profile == 88;
}

static unsigned rtc_h264_rank(const rtc_h264_plid* p)
{
    if (p->level == 9 || (p->level == 11 && rtc_h264_base(p->profile) && (p->iop & 0x10u)))
        return 21;
    return p->level * 2;
}

static int rtc_fmtp_uint(const char* fmtp, const char* key, unsigned dflt, unsigned* out)
{
    const char* v;
    size_t n, i;
    unsigned x = 0;
    *out = dflt;
    if (!rtc_sdp_fmtp_get(fmtp, key, &v, &n))
        return 0;
    if (!n || n > 5)
        return -1;
    for (i = 0; i < n; i++) {
        if (v[i] < '0' || v[i] > '9')
            return -1;
        x = x * 10 + (unsigned)(v[i] - '0');
    }
    *out = x;
    return 0;
}

static void rtc_codec_name(const rtc_sdp_codec* c, const char** name, uint32_t* clock, unsigned* ch)
{
    size_t i;
    *name = c->name;
    *clock = c->clock;
    *ch = c->channels ? c->channels : 1;
    if (c->has_rtpmap || c->name[0])
        return;
    for (i = 0; i < sizeof(rtc_static_pts) / sizeof(rtc_static_pts[0]); i++)
        if (rtc_static_pts[i].pt == c->pt) {
            *name = rtc_static_pts[i].name;
            *clock = rtc_static_pts[i].clock;
            *ch = rtc_static_pts[i].channels;
        }
}

int rtc_jsep_codec_match(const rtc_sdp_codec* offered, const rtc_sdp_codec* local)
{
    const char *on, *ln;
    uint32_t oc, lc;
    unsigned och, lch, opm, lpm;
    int op;
    rtc_codec_name(offered, &on, &oc, &och);
    rtc_codec_name(local, &ln, &lc, &lch);
    if (!on[0] || !rtc_ieq(on, ln) || oc != lc || och != lch)
        return 0;
    if (!rtc_ieq(on, "H264"))
        return 1;
    if (rtc_fmtp_uint(offered->fmtp, "packetization-mode", 0, &opm) < 0 || rtc_fmtp_uint(local->fmtp, "packetization-mode", 0, &lpm) < 0)
        return 0;
    op = rtc_jsep_h264_profile(offered->fmtp);
    return opm == lpm && op > 0 && op == rtc_jsep_h264_profile(local->fmtp);
}

static void rtc_hex2(char* o, unsigned v)
{
    static const char hx[] = "0123456789abcdef";
    o[0] = hx[v >> 4 & 15];
    o[1] = hx[v & 15];
}

int rtc_jsep_h264_answer_fmtp(const char* offered, const char* local, char* out, size_t cap)
{
    rtc_h264_plid op, lp;
    unsigned oa, la, rank, iop, level;
    const char *p = local, *tv;
    size_t n = 0, tn;
    int put = 0, named = rtc_sdp_fmtp_get(offered, "profile-level-id", &tv, &tn);
    char plid[6];
    if (rtc_h264_plid_get(offered, &op) < 0 || rtc_h264_plid_get(local, &lp) < 0)
        return -1;
    if (rtc_fmtp_uint(offered, "level-asymmetry-allowed", 0, &oa) < 0 || rtc_fmtp_uint(local, "level-asymmetry-allowed", 0, &la) < 0)
        return -1;
    rank = oa == 1 && la == 1 ? rtc_h264_rank(&lp) : rtc_h264_rank(&op) < rtc_h264_rank(&lp) ? rtc_h264_rank(&op) : rtc_h264_rank(&lp);
    iop = rtc_h264_base(op.profile) ? op.iop & ~0x10u : op.iop;
    level = rank / 2;
    if (rank == 21) {
        if (rtc_h264_base(op.profile)) {
            level = 11;
            iop |= 0x10u;
        } else
            level = 9;
    }
    rtc_hex2(plid, op.profile);
    rtc_hex2(plid + 2, iop);
    rtc_hex2(plid + 4, level);
    while (*p) {
        const char* s;
        size_t l;
        while (*p == ' ' || *p == ';')
            p++;
        if (!*p)
            break;
        s = p;
        while (*p && *p != ';')
            p++;
        l = (size_t)(p - s);
        while (l && s[l - 1] == ' ')
            l--;
        if (n + l + 2 + 22 > cap)
            return -1;
        if (n)
            out[n++] = ';';
        if (l >= 16 && !strncmp(s, "profile-level-id", 16) && (l == 16 || s[16] == '=' || s[16] == ' ')) {
            memcpy(out + n, "profile-level-id=", 17);
            memcpy(out + n + 17, plid, 6);
            n += 23;
            put = 1;
        } else {
            memcpy(out + n, s, l);
            n += l;
        }
    }
    if (!put && (named || op.profile != 0x42 || level != 10)) {
        if (n + 25 > cap)
            return -1;
        if (n)
            out[n++] = ';';
        memcpy(out + n, "profile-level-id=", 17);
        memcpy(out + n + 17, plid, 6);
        n += 23;
    }
    out[n] = 0;
    return 0;
}

int rtc_jsep_dir_answer(int offered, int local)
{
    int osend = offered == RTC_DIR_SENDRECV || offered == RTC_DIR_SENDONLY;
    int orecv = offered == RTC_DIR_SENDRECV || offered == RTC_DIR_RECVONLY;
    int lsend = local == RTC_DIR_SENDRECV || local == RTC_DIR_SENDONLY;
    int lrecv = local == RTC_DIR_SENDRECV || local == RTC_DIR_RECVONLY;
    int send = lsend && orecv, recv = lrecv && osend;
    return send && recv ? RTC_DIR_SENDRECV : send ? RTC_DIR_SENDONLY : recv ? RTC_DIR_RECVONLY : RTC_DIR_INACTIVE;
}

static const char* rtc_d(rtc_jctx* x, const char* s)
{
    const char* d = rtc_sdp_strdup(x->ans, s, strlen(s));
    if (!d) {
        x->oom = 1;
        return "";
    }
    return d;
}

static void rtc_minit(rtc_sdp_media* m)
{
    memset(m, 0, sizeof(*m));
    m->kind = m->proto = m->fmt_text = m->conn = m->mid = m->ufrag = m->pwd = m->ice_opts = "";
}

static void rtc_reject(rtc_jctx* x, rtc_sdp_media* am, const rtc_sdp_media* om)
{
    int i;
    rtc_minit(am);
    am->kind = rtc_d(x, om->kind);
    am->proto = rtc_d(x, om->proto);
    am->mid = rtc_d(x, om->mid);
    am->fmt_text = rtc_d(x, om->fmt_text);
    for (i = 0; i < om->ncodecs; i++) {
        rtc_sdp_codec* c = rtc_sdp_add_codec(x->ans, am);
        if (!c) {
            x->oom = 1;
            return;
        }
        c->pt = om->codecs[i].pt;
    }
    am->has_c = 1;
    am->conn = "IN IP4 0.0.0.0";
}

static const rtc_sdp_codec* rtc_local_for(const rtc_sdp_codec* oc, const rtc_sdp_codec* caps, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (rtc_jsep_codec_match(oc, &caps[i]))
            return &caps[i];
    return NULL;
}

static void rtc_apt_fmtp(char* o, unsigned apt)
{
    size_t k = 4;
    memcpy(o, "apt=", 4);
    if (apt >= 100)
        o[k++] = (char)('0' + apt / 100);
    if (apt >= 10)
        o[k++] = (char)('0' + apt / 10 % 10);
    o[k++] = (char)('0' + apt % 10);
    o[k] = 0;
}

static int rtc_pref_rank(const rtc_sdp_codec* c, const rtc_sdp_codec* pref, int npref)
{
    int i;
    for (i = 0; i < npref; i++)
        if (rtc_jsep_codec_match(c, &pref[i]))
            return i;
    return npref;
}

static int rtc_has_rtx(const rtc_sdp_media* m)
{
    int i;
    for (i = 0; i < m->ncodecs; i++)
        if (rtc_ieq(m->codecs[i].name, "rtx"))
            return 1;
    return 0;
}

static int rtc_add_ssrcs(rtc_jctx* x, rtc_sdp_media* m, uint32_t ssrc, uint32_t rtx, const char* cname, const rtc_sdp_msid* msid)
{
    uint32_t ids[2];
    int i, n = rtx ? 2 : 1;
    ids[0] = ssrc;
    ids[1] = rtx;
    if (rtx) {
        rtc_sdp_ssrc_group* g = rtc_sdp_add_group(x->ans, m);
        if (!g || !(g->ssrc = (uint32_t*)rtc_sdp_alloc(x->ans, sizeof(uint32_t) * 2)))
            return -1;
        g->semantics = "FID";
        g->ssrc[0] = ssrc;
        g->ssrc[1] = rtx;
        g->n = 2;
    }
    for (i = 0; i < n; i++) {
        rtc_sdp_ssrc* s = rtc_sdp_add_ssrc(x->ans, m);
        if (!s)
            return -1;
        s->id = ids[i];
        s->cname = rtc_d(x, cname ? cname : "");
        if (msid && msid->stream && msid->stream[0]) {
            s->msid.stream = rtc_d(x, msid->stream);
            s->msid.track = rtc_d(x, msid->track ? msid->track : "");
        }
    }
    return x->oom ? -1 : 0;
}

static void rtc_pref_order(rtc_sdp_media* am, const rtc_sdp_codec* pref, int npref)
{
    int i, j;
    for (i = 1; i < am->ncodecs; i++) {
        rtc_sdp_codec c = am->codecs[i];
        int ki = rtc_pref_rank(&c, pref, npref);
        for (j = i; j > 0 && rtc_pref_rank(&am->codecs[j - 1], pref, npref) > ki; j--)
            am->codecs[j] = am->codecs[j - 1];
        am->codecs[j] = c;
    }
}

static int rtc_answer_codecs(rtc_jctx* x, const rtc_sdp_media* om, const rtc_sdp_codec* caps, int ncaps, rtc_sdp_media* am)
{
    uint8_t* ok = (uint8_t*)rtc_sdp_alloc(x->ans, (size_t)om->ncodecs);
    const rtc_sdp_codec** lc = (const rtc_sdp_codec**)rtc_sdp_alloc(x->ans, sizeof(*lc) * (size_t)om->ncodecs);
    char* buf;
    size_t bcap = 64;
    int i, j, prim = 0;
    if (!ok || !lc)
        return -1;
    for (i = 0; i < om->ncodecs; i++)
        bcap += strlen(om->codecs[i].fmtp);
    for (i = 0; i < ncaps; i++)
        bcap += strlen(caps[i].fmtp ? caps[i].fmtp : "");
    if (!(buf = (char*)rtc_sdp_alloc(x->ans, bcap)))
        return -1;
    for (i = 0; i < om->ncodecs; i++) {
        ok[i] = 0;
        lc[i] = rtc_ieq(om->codecs[i].name, "rtx") ? NULL : rtc_local_for(&om->codecs[i], caps, ncaps);
        if (lc[i]) {
            ok[i] = 1;
            prim++;
        }
    }
    if (!prim)
        return 0;
    for (i = 0; i < om->ncodecs; i++) {
        const rtc_sdp_codec* oc = &om->codecs[i];
        if (!rtc_ieq(oc->name, "rtx") || !oc->rtx_apt_set)
            continue;
        for (j = 0; j < om->ncodecs; j++)
            if (ok[j] && !rtc_ieq(om->codecs[j].name, "rtx") && om->codecs[j].pt == oc->rtx_apt)
                break;
        if (j < om->ncodecs)
            lc[i] = rtc_local_for(oc, caps, ncaps);
        ok[i] = lc[i] != NULL;
    }
    for (i = 0; i < om->ncodecs; i++) {
        const rtc_sdp_codec* oc = &om->codecs[i];
        rtc_sdp_codec* ac;
        const char* nm;
        uint32_t clock;
        unsigned ch;
        if (!ok[i])
            continue;
        if (!(ac = rtc_sdp_add_codec(x->ans, am)))
            return -1;
        rtc_codec_name(oc, &nm, &clock, &ch);
        ac->pt = oc->pt;
        ac->has_rtpmap = 1;
        ac->name = rtc_d(x, nm);
        ac->clock = clock;
        ac->channels = oc->channels;
        ac->fb = (uint8_t)(oc->fb & lc[i]->fb);
        if (rtc_ieq(nm, "rtx")) {
            ac->rtx_apt_set = 1;
            ac->rtx_apt = oc->rtx_apt;
            rtc_apt_fmtp(buf, oc->rtx_apt);
            ac->fmtp = rtc_d(x, buf);
            ac->has_fmtp = 1;
        } else if (rtc_ieq(nm, "H264")) {
            if (rtc_jsep_h264_answer_fmtp(oc->fmtp, lc[i]->fmtp, buf, bcap) < 0)
                return -1;
            ac->fmtp = rtc_d(x, buf);
            ac->has_fmtp = buf[0] != 0;
        } else if (lc[i]->has_fmtp) {
            ac->fmtp = rtc_d(x, lc[i]->fmtp);
            ac->has_fmtp = 1;
        }
    }
    return 1;
}

static void rtc_answer_transport(rtc_jctx* x, rtc_sdp_media* am, const rtc_sdp* offer, const rtc_sdp_media* om, const rtc_jsep_params* prm)
{
    int os = om->setup ? om->setup : offer->setup;
    am->ufrag = rtc_d(x, prm->ufrag ? prm->ufrag : "");
    am->pwd = rtc_d(x, prm->pwd ? prm->pwd : "");
    if (prm->trickle) {
        am->ice_opts = "trickle";
        am->trickle = 1;
    }
    if (prm->fp && prm->fp_len <= sizeof(am->fp)) {
        memcpy(am->fp, prm->fp, prm->fp_len);
        am->fp_len = prm->fp_len;
        am->fp_alg = prm->fp_alg;
    }
    if (os == RTC_SETUP_ACTPASS)
        am->setup = prm->setup == RTC_SETUP_PASSIVE ? RTC_SETUP_PASSIVE : RTC_SETUP_ACTIVE;
    else if (os == RTC_SETUP_ACTIVE)
        am->setup = RTC_SETUP_PASSIVE;
    else if (os == RTC_SETUP_PASSIVE)
        am->setup = RTC_SETUP_ACTIVE;
    else
        am->setup = RTC_SETUP_PASSIVE;
}

static int rtc_rid_sent(const rtc_sdp_media* om, const char* id, size_t n)
{
    int i;
    for (i = 0; i < om->nextra; i++) {
        const char* l = om->extra[i];
        if (strncmp(l, "a=rid:", 6) || strncmp(l + 6, id, n) || l[6 + n] != ' ')
            continue;
        l += 7 + n;
        while (*l == ' ')
            l++;
        if (!strncmp(l, "send", 4) && (!l[4] || l[4] == ' '))
            return 1;
    }
    return 0;
}

static const char* rtc_sim_send_list(const rtc_sdp_media* om, size_t* n)
{
    int i;
    for (i = 0; i < om->nextra; i++) {
        const char* l = om->extra[i];
        if (strncmp(l, "a=simulcast:", 12))
            continue;
        l += 12;
        for (;;) {
            const char *d, *v;
            size_t dl;
            while (*l == ' ')
                l++;
            if (!*l)
                break;
            d = l;
            while (*l && *l != ' ')
                l++;
            dl = (size_t)(l - d);
            while (*l == ' ')
                l++;
            v = l;
            while (*l && *l != ' ')
                l++;
            if (dl == 4 && !strncmp(d, "send", 4) && l > v) {
                *n = (size_t)(l - v);
                return v;
            }
        }
    }
    return NULL;
}

static int rtc_answer_simulcast(rtc_jctx* x, const rtc_sdp_media* om, rtc_sdp_media* am)
{
    size_t n = 0, i = 0;
    const char* v = rtc_sim_send_list(om, &n);
    rtc_sb sim, rid;
    int streams = 0, r = 0;
    if (!v || (am->dir != RTC_DIR_SENDRECV && am->dir != RTC_DIR_RECVONLY))
        return 0;
    rtc_sb_init(&sim);
    rtc_sb_str(&sim, "simulcast:recv ");
    while (i < n && !r) {
        size_t s = i;
        int alts = 0;
        while (i < n && v[i] != ';')
            i++;
        while (s < i) {
            size_t a = s, b;
            const char* id;
            while (s < i && v[s] != ',')
                s++;
            b = s;
            if (s < i)
                s++;
            id = v + a + (v[a] == '~');
            if (b <= a + (v[a] == '~') || !rtc_rid_sent(om, id, (size_t)(v + b - id)))
                continue;
            rtc_sb_put(&sim, alts ? "," : (streams ? ";" : ""), alts || streams ? 1 : 0);
            rtc_sb_put(&sim, v + a, b - a);
            rtc_sb_init(&rid);
            rtc_sb_str(&rid, "rid:");
            rtc_sb_put(&rid, id, (size_t)(v + b - id));
            rtc_sb_str(&rid, " recv");
            r = rid.err || rtc_sdp_extra_add(x->ans, am, 'a', rid.buf, rid.len) < 0 ? -1 : 0;
            rtc_sb_free(&rid);
            if (r)
                break;
            alts++;
        }
        if (alts)
            streams++;
        if (i < n)
            i++;
    }
    if (!r && streams)
        r = sim.err || rtc_sdp_extra_add(x->ans, am, 'a', sim.buf, sim.len) < 0 ? -1 : 0;
    rtc_sb_free(&sim);
    return r;
}

static int rtc_answer_media(rtc_jctx* x, const rtc_sdp* offer, int i, const rtc_jsep_caps* caps, const rtc_jsep_local* lo,
    const rtc_jsep_params* prm, rtc_sdp_media* am)
{
    const rtc_sdp_media* om = &offer->media[i];
    int audio = !strcmp(om->kind, "audio"), video = !strcmp(om->kind, "video"), r, j, k;
    rtc_reject(x, am, om);
    if (rtc_sdp_media_rejected(om) || (lo && lo->stopped))
        return 0;
    if (!strcmp(om->kind, "application")) {
        if (!caps->data || strcmp(om->fmt_text, "webrtc-datachannel") || !strstr(om->proto, "SCTP"))
            return 0;
        am->has_sctp_port = 1;
        am->sctp_port = prm->sctp_port ? prm->sctp_port : 5000;
        if (prm->max_msg) {
            am->has_max_msg = 1;
            am->max_msg = prm->max_msg;
        }
    } else {
        if ((!audio && !video) || !strstr(om->proto, "RTP/"))
            return 0;
        am->ncodecs = 0;
        if (lo && lo->codecs)
            r = rtc_answer_codecs(x, om, lo->codecs, lo->ncodecs, am);
        else
            r = rtc_answer_codecs(x, om, audio ? caps->audio : caps->video, audio ? caps->naudio : caps->nvideo, am);
        if (r <= 0) {
            rtc_reject(x, am, om);
            return r;
        }
        if (lo && lo->codecs)
            rtc_pref_order(am, lo->codecs, lo->ncodecs);
        for (j = 0; j < om->next; j++)
            for (k = 0; k < caps->next; k++)
                if (!strcmp(om->ext[j].uri, caps->ext[k]) && (!caps->ext_kinds || (caps->ext_kinds[k] & (audio ? 1 : 2)))) {
                    rtc_sdp_ext* e = rtc_sdp_add_ext(x->ans, am);
                    if (!e)
                        return -1;
                    e->id = om->ext[j].id;
                    e->uri = rtc_d(x, om->ext[j].uri);
                    break;
                }
        am->has_dir = 1;
        am->dir = (uint8_t)rtc_jsep_dir_answer(om->has_dir ? om->dir : RTC_DIR_SENDRECV, lo ? lo->dir : RTC_DIR_SENDRECV);
        if (lo && (am->dir == RTC_DIR_SENDRECV || am->dir == RTC_DIR_SENDONLY)) {
            int q, nm = lo->nmsid ? lo->nmsid : 1;
            for (q = 0; q < nm; q++) {
                const rtc_sdp_msid* src = lo->nmsid ? &lo->msids[q] : &lo->msid;
                rtc_sdp_msid* s;
                if (!src->stream || !src->stream[0])
                    continue;
                if (!(s = rtc_sdp_add_msid(x->ans, am)))
                    return -1;
                s->stream = rtc_d(x, src->stream);
                s->track = rtc_d(x, src->track ? src->track : "");
            }
        }
        if (lo && lo->ssrc && (am->dir == RTC_DIR_SENDRECV || am->dir == RTC_DIR_SENDONLY)
            && rtc_add_ssrcs(x, am, lo->ssrc, rtc_has_rtx(am) ? lo->rtx_ssrc : 0, lo->cname, lo->msid.stream ? &lo->msid : NULL) < 0)
            return -1;
        if (video && rtc_answer_simulcast(x, om, am) < 0)
            return -1;
        am->rtcp_mux = om->rtcp_mux || om->rtcp_mux_only;
        am->rtcp_mux_only = om->rtcp_mux_only;
        am->rtcp_rsize = om->rtcp_rsize;
        am->allow_mixed = (uint8_t)(om->allow_mixed && caps->mixed);
        if (!am->rtcp_mux && rtc_sdp_extra_add(x->ans, am, 'a', "rtcp:9 IN IP4 0.0.0.0", 21) < 0)
            return -1;
    }
    am->port = 9;
    rtc_answer_transport(x, am, offer, om, prm);
    return 1;
}

static int rtc_mid_index(const rtc_sdp* s, const char* mid)
{
    int i;
    for (i = 0; i < s->nmedia; i++)
        if (s->media[i].mid[0] && !strcmp(s->media[i].mid, mid))
            return i;
    return -1;
}

int rtc_jsep_answer(const rtc_sdp* offer, const rtc_jsep_caps* caps, const rtc_jsep_local* local, const rtc_jsep_params* prm,
    rtc_sdp* ans, const char** err)
{
    int i, tag = -1;
    rtc_jctx x;
    rtc_sb b;
    *err = NULL;
    rtc_sdp_reset(ans, 1024 + 1024 * (size_t)offer->nmedia);
    ans->lim = offer->lim;
    x.ans = ans;
    x.oom = 0;
    ans->sess_id = prm->sess_id;
    ans->sess_ver = prm->sess_ver;
    rtc_sb_init(&b);
    rtc_sb_str(&b, "- ");
    rtc_sb_u64(&b, prm->sess_id);
    rtc_sb_put(&b, " ", 1);
    rtc_sb_u64(&b, prm->sess_ver);
    rtc_sb_str(&b, " IN IP4 127.0.0.1");
    ans->origin = b.err ? "" : rtc_d(&x, b.buf);
    if (b.err)
        x.oom = 1;
    rtc_sb_free(&b);
    ans->name = "-";
    ans->timing = "0 0";
    ans->has_o = ans->has_s = ans->has_t = 1;
    ans->allow_mixed = (uint8_t)(offer->allow_mixed && caps->mixed);
    for (i = 0; i < offer->nmedia; i++)
        if (!rtc_sdp_add_media(ans))
            x.oom = 1;
    for (i = 0; i < offer->nmedia && !x.oom; i++)
        if (rtc_answer_media(&x, offer, i, caps, local ? &local[i] : NULL, prm, &ans->media[i]) < 0)
            x.oom = 1;
    if (!x.oom && offer->has_bundle && offer->nbundle) {
        tag = rtc_mid_index(offer, offer->bundle[0]);
        if (tag >= 0 && (offer->media[tag].port == 0 || ans->media[tag].port == 0))
            tag = -1;
    }
    if (tag >= 0) {
        ans->has_bundle = 1;
        ans->nbundle_groups = 1;
        if (rtc_sdp_add_bundle(ans, offer->bundle[0]) < 0)
            x.oom = 1;
        for (i = 1; i < offer->nbundle && !x.oom; i++) {
            int k = rtc_mid_index(offer, offer->bundle[i]);
            if (k >= 0 && ans->media[k].port && rtc_sdp_add_bundle(ans, offer->bundle[i]) < 0)
                x.oom = 1;
        }
    } else
        for (i = 0; i < offer->nmedia && !x.oom; i++)
            if (offer->media[i].bundle_only)
                rtc_reject(&x, &ans->media[i], &offer->media[i]);
    if (x.oom) {
        *err = "out of memory or an answer list over its cap";
        return -1;
    }
    return 0;
}

static const rtc_sdp_codec rtc_default_audio[] = {
    { .name = "opus", .fmtp = "minptime=10;useinbandfec=1", .clock = 48000, .pt = 111, .channels = 2, .fb = RTC_FB_TWCC, .has_rtpmap = 1, .has_fmtp = 1 },
    { .name = "PCMU", .fmtp = "", .clock = 8000, .pt = 0, .has_rtpmap = 1 },
    { .name = "PCMA", .fmtp = "", .clock = 8000, .pt = 8, .has_rtpmap = 1 },
    { .name = "telephone-event", .fmtp = "0-15", .clock = 48000, .pt = 110, .has_rtpmap = 1, .has_fmtp = 1 },
    { .name = "telephone-event", .fmtp = "0-15", .clock = 8000, .pt = 126, .has_rtpmap = 1, .has_fmtp = 1 },
};

static const rtc_sdp_codec rtc_default_video[] = {
    { .name = "VP8", .fmtp = "", .clock = 90000, .pt = 96, .fb = RTC_FB_NACK | RTC_FB_PLI | RTC_FB_FIR | RTC_FB_TWCC, .has_rtpmap = 1 },
    { .name = "rtx", .fmtp = "apt=96", .clock = 90000, .pt = 97, .rtx_apt_set = 1, .rtx_apt = 96, .has_rtpmap = 1, .has_fmtp = 1 },
    { .name = "H264", .fmtp = "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f", .clock = 90000, .pt = 98,
        .fb = RTC_FB_NACK | RTC_FB_PLI | RTC_FB_FIR | RTC_FB_TWCC, .has_rtpmap = 1, .has_fmtp = 1 },
    { .name = "rtx", .fmtp = "apt=98", .clock = 90000, .pt = 99, .rtx_apt_set = 1, .rtx_apt = 98, .has_rtpmap = 1, .has_fmtp = 1 },
    { .name = "H264", .fmtp = "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f", .clock = 90000, .pt = 100,
        .fb = RTC_FB_NACK | RTC_FB_PLI | RTC_FB_FIR | RTC_FB_TWCC, .has_rtpmap = 1, .has_fmtp = 1 },
    { .name = "rtx", .fmtp = "apt=100", .clock = 90000, .pt = 101, .rtx_apt_set = 1, .rtx_apt = 100, .has_rtpmap = 1, .has_fmtp = 1 },
};

static const char* const rtc_default_ext[] = {
    "urn:ietf:params:rtp-hdrext:sdes:mid",
    "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01",
    "urn:ietf:params:rtp-hdrext:ssrc-audio-level",
    "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id",
    "urn:ietf:params:rtp-hdrext:sdes:repaired-rtp-stream-id",
    "urn:ietf:params:rtp-hdrext:csrc-audio-level",
};

static const uint8_t rtc_default_ext_kinds[] = { 3, 3, 1, 2, 2, 1 };

static const rtc_jsep_caps rtc_default_caps = {
    .audio = rtc_default_audio,
    .naudio = (int)(sizeof(rtc_default_audio) / sizeof(rtc_default_audio[0])),
    .video = rtc_default_video,
    .nvideo = (int)(sizeof(rtc_default_video) / sizeof(rtc_default_video[0])),
    .ext = rtc_default_ext,
    .ext_kinds = rtc_default_ext_kinds,
    .next = (int)(sizeof(rtc_default_ext) / sizeof(rtc_default_ext[0])),
    .data = 1,
    .mixed = 0,
};

const rtc_jsep_caps* rtc_jsep_default_caps(void)
{
    return &rtc_default_caps;
}

static void rtc_offer_transport(rtc_jctx* x, rtc_sdp_media* m, const rtc_jsep_params* prm)
{
    m->ufrag = rtc_d(x, prm->ufrag ? prm->ufrag : "");
    m->pwd = rtc_d(x, prm->pwd ? prm->pwd : "");
    if (prm->trickle) {
        m->ice_opts = "trickle";
        m->trickle = 1;
    }
    if (prm->fp && prm->fp_len <= sizeof(m->fp)) {
        memcpy(m->fp, prm->fp, prm->fp_len);
        m->fp_len = prm->fp_len;
        m->fp_alg = prm->fp_alg;
    }
    m->setup = prm->setup ? prm->setup : RTC_SETUP_ACTPASS;
}

static int rtc_offer_rids(rtc_jctx* x, rtc_sdp_media* m, const rtc_jsep_offer_m* o)
{
    rtc_sb b;
    int i, r = 0;
    if (o->nrids < 2)
        return 0;
    for (i = 0; i < o->nrids && !r; i++) {
        rtc_sb_init(&b);
        rtc_sb_str(&b, "rid:");
        rtc_sb_str(&b, o->rids[i]);
        rtc_sb_str(&b, " send");
        r = b.err || rtc_sdp_extra_add(x->ans, m, 'a', b.buf, b.len) < 0 ? -1 : 0;
        rtc_sb_free(&b);
    }
    if (r)
        return -1;
    rtc_sb_init(&b);
    rtc_sb_str(&b, "simulcast:send ");
    for (i = 0; i < o->nrids; i++) {
        if (i)
            rtc_sb_put(&b, ";", 1);
        rtc_sb_str(&b, o->rids[i]);
    }
    r = b.err || rtc_sdp_extra_add(x->ans, m, 'a', b.buf, b.len) < 0 ? -1 : 0;
    rtc_sb_free(&b);
    return r;
}

static int rtc_offer_media(rtc_jctx* x, const rtc_jsep_offer_m* o, const rtc_jsep_caps* caps, const rtc_jsep_params* prm,
    rtc_sdp_media* m)
{
    int app = !strcmp(o->kind, "application"), audio = !strcmp(o->kind, "audio"), i;
    m->kind = rtc_d(x, o->kind);
    m->proto = app ? "UDP/DTLS/SCTP" : "UDP/TLS/RTP/SAVPF";
    m->mid = rtc_d(x, o->mid ? o->mid : "");
    m->has_c = 1;
    m->conn = "IN IP4 0.0.0.0";
    if (app)
        m->fmt_text = "webrtc-datachannel";
    for (i = 0; i < o->ncodecs && !app; i++) {
        rtc_sdp_codec* c = rtc_sdp_add_codec(x->ans, m);
        if (!c)
            return -1;
        *c = o->codecs[i];
        c->name = rtc_d(x, o->codecs[i].name ? o->codecs[i].name : "");
        c->fmtp = rtc_d(x, o->codecs[i].fmtp ? o->codecs[i].fmtp : "");
        c->has_fmtp = c->fmtp[0] != 0;
        c->has_rtpmap = 1;
    }
    if (!app && !m->ncodecs)
        m->fmt_text = "0";
    if (o->rejected) {
        m->port = 0;
        m->has_dir = 1;
        m->dir = RTC_DIR_INACTIVE;
        for (i = 0; i < m->ncodecs; i++)
            m->codecs[i].has_rtpmap = m->codecs[i].has_fmtp = m->codecs[i].fb = 0;
        return 0;
    }
    m->port = o->bundle_only ? 0 : 9;
    m->bundle_only = o->bundle_only;
    rtc_offer_transport(x, m, prm);
    if (app) {
        m->has_sctp_port = 1;
        m->sctp_port = prm->sctp_port ? prm->sctp_port : 5000;
        if (prm->max_msg) {
            m->has_max_msg = 1;
            m->max_msg = prm->max_msg;
        }
        return 0;
    }
    for (i = 0; i < caps->next; i++)
        if (!caps->ext_kinds || (caps->ext_kinds[i] & (audio ? 1 : 2))) {
            rtc_sdp_ext* e = rtc_sdp_add_ext(x->ans, m);
            if (!e)
                return -1;
            e->id = o->ext_ids && o->ext_ids[i] ? o->ext_ids[i] : (uint16_t)(i + 1);
            e->uri = caps->ext[i];
        }
    m->has_dir = 1;
    m->dir = o->dir;
    for (i = 0; i < o->nmsid; i++) {
        rtc_sdp_msid* s = rtc_sdp_add_msid(x->ans, m);
        if (!s)
            return -1;
        s->stream = rtc_d(x, o->msid[i].stream ? o->msid[i].stream : "-");
        s->track = rtc_d(x, o->msid[i].track ? o->msid[i].track : "");
    }
    m->rtcp_mux = 1;
    m->rtcp_rsize = 1;
    if (o->ssrc && rtc_add_ssrcs(x, m, o->ssrc, rtc_has_rtx(m) ? o->rtx_ssrc : 0, o->cname, o->nmsid ? &o->msid[0] : NULL) < 0)
        return -1;
    return rtc_offer_rids(x, m, o);
}

int rtc_jsep_offer(const rtc_jsep_offer_m* ms, int n, const rtc_jsep_caps* caps, const rtc_jsep_params* prm, rtc_sdp* out,
    const char** err)
{
    rtc_jctx x;
    rtc_sb b;
    int i, any = 0;
    *err = NULL;
    rtc_sdp_reset(out, 2048 + 2048 * (size_t)(n > 0 ? n : 0));
    x.ans = out;
    x.oom = 0;
    out->sess_id = prm->sess_id;
    out->sess_ver = prm->sess_ver;
    rtc_sb_init(&b);
    rtc_sb_str(&b, "- ");
    rtc_sb_u64(&b, prm->sess_id);
    rtc_sb_put(&b, " ", 1);
    rtc_sb_u64(&b, prm->sess_ver);
    rtc_sb_str(&b, " IN IP4 127.0.0.1");
    out->origin = b.err ? "" : rtc_d(&x, b.buf);
    if (b.err)
        x.oom = 1;
    rtc_sb_free(&b);
    out->name = "-";
    out->timing = "0 0";
    out->has_o = out->has_s = out->has_t = 1;
    for (i = 0; i < n && !x.oom; i++) {
        rtc_sdp_media* m = rtc_sdp_add_media(out);
        if (!m || rtc_offer_media(&x, &ms[i], caps, prm, m) < 0)
            x.oom = 1;
        else if (!ms[i].rejected)
            any = 1;
    }
    if (!x.oom && any) {
        out->has_bundle = 1;
        out->nbundle_groups = 1;
        for (i = 0; i < n && !x.oom; i++)
            if (!ms[i].rejected && rtc_sdp_add_bundle(out, ms[i].mid ? ms[i].mid : "") < 0)
                x.oom = 1;
    }
    if (!x.oom && rtc_sdp_extra_add(out, NULL, 'a', "msid-semantic: WMS", 18) < 0)
        x.oom = 1;
    if (x.oom) {
        *err = "out of memory or an offer list over its cap";
        return -1;
    }
    return 0;
}
