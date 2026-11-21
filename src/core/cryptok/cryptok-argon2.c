#include "cryptok.h"

#define ARG2_VERSION_NUMBER 0x13
#define ARG2_SYNC_POINTS 4
#define ARG2_QWORDS_IN_BLOCK 128
#define ARG2_BLOCK_SIZE 1024
#define ARG2_ADDRESSES_IN_BLOCK 128
#define ARG2_PREHASH_DIGEST_LENGTH 64
#define ARG2_PREHASH_SEED_LENGTH 72
#define ARG2_TYPE_ID 2
#define ARG2_MAX_MEMORY (1 << 22)
#define ARG2_PAR_MIN_SEGMENT_BLOCKS 32

typedef struct {
    uint64_t v[ARG2_QWORDS_IN_BLOCK];
} arg_block;

typedef struct {
    arg_block* memory;
    uint32_t passes;
    uint32_t memory_blocks;
    uint32_t segment_length;
    uint32_t lane_length;
    uint32_t lanes;
} arg_instance;

static inline uint64_t arg_rotr64(uint64_t x, int n)
{
    return (x >> n) | (x << (64 - n));
}

static inline uint64_t arg_fBlaMka(uint64_t x, uint64_t y)
{
    const uint64_t m = 0xFFFFFFFFULL;
    const uint64_t xy = (x & m) * (y & m);
    return x + y + 2 * xy;
}

#define ARG_G(a, b, c, d)          \
    do {                           \
        a = arg_fBlaMka(a, b);     \
        d = arg_rotr64(d ^ a, 32); \
        c = arg_fBlaMka(c, d);     \
        b = arg_rotr64(b ^ c, 24); \
        a = arg_fBlaMka(a, b);     \
        d = arg_rotr64(d ^ a, 16); \
        c = arg_fBlaMka(c, d);     \
        b = arg_rotr64(b ^ c, 63); \
    } while (0)

#define ARG_ROUND(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15) \
    do {                                                                                \
        ARG_G(v0, v4, v8, v12);                                                         \
        ARG_G(v1, v5, v9, v13);                                                         \
        ARG_G(v2, v6, v10, v14);                                                        \
        ARG_G(v3, v7, v11, v15);                                                        \
        ARG_G(v0, v5, v10, v15);                                                        \
        ARG_G(v1, v6, v11, v12);                                                        \
        ARG_G(v2, v7, v8, v13);                                                         \
        ARG_G(v3, v4, v9, v14);                                                         \
    } while (0)

static inline void arg_store64(uint8_t* p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static inline void arg_store32(uint8_t* p, uint32_t v)
{
    int i;
    for (i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

typedef struct {
    uint64_t h[8];
    uint64_t t;
    uint8_t buf[128];
    size_t buflen;
    uint8_t outlen;
} arg_blake2b_ctx;

static void arg_store_block(uint8_t* out, const arg_block* b)
{
    unsigned i;
    for (i = 0; i < ARG2_QWORDS_IN_BLOCK; i++)
        arg_store64(out + i * 8, b->v[i]);
}

static void arg_blake2b_init(arg_blake2b_ctx* s, size_t outlen)
{
    memset(s, 0, sizeof *s);
    memcpy(s->h, BL2B_IV, sizeof s->h);
    s->h[0] ^= 0x01010000ULL ^ (uint64_t)outlen;
    s->outlen = (uint8_t)outlen;
}

static void arg_blake2b_update(arg_blake2b_ctx* s, const uint8_t* in,
    size_t inlen)
{
    if (inlen == 0)
        return;
    if (s->buflen + inlen > 128) {
        size_t fill = 128 - s->buflen;
        memcpy(s->buf + s->buflen, in, fill);
        s->t += 128;
        bl2b_compress(s->h, s->buf, s->t, 0);
        s->buflen = 0;
        in += fill;
        inlen -= fill;
        while (inlen > 128) {
            s->t += 128;
            bl2b_compress(s->h, in, s->t, 0);
            in += 128;
            inlen -= 128;
        }
    }
    memcpy(s->buf + s->buflen, in, inlen);
    s->buflen += inlen;
}

static void arg_blake2b_final(arg_blake2b_ctx* s, uint8_t* out)
{
    uint8_t block[128];
    size_t i;
    memset(block, 0, sizeof block);
    memcpy(block, s->buf, s->buflen);
    s->t += s->buflen;
    bl2b_compress(s->h, block, s->t, 1);
    for (i = 0; i < s->outlen; i++)
        out[i] = (uint8_t)(s->h[i / 8] >> (8 * (i % 8)));
    dyn_wipe(block, sizeof block);
    memset(s, 0, sizeof *s);
}

static int arg_blake2b_long(uint8_t* out, size_t outlen,
    const uint8_t* in, size_t inlen)
{
    arg_blake2b_ctx s;
    uint8_t ob[4];
    uint8_t outbuf[64];
    if (outlen == 0 || outlen > 0xFFFFFFFFULL)
        return -1;
    ob[0] = (uint8_t)(outlen & 0xff);
    ob[1] = (uint8_t)((outlen >> 8) & 0xff);
    ob[2] = (uint8_t)((outlen >> 16) & 0xff);
    ob[3] = (uint8_t)((outlen >> 24) & 0xff);
    if (outlen <= 64) {
        arg_blake2b_init(&s, outlen);
        arg_blake2b_update(&s, ob, 4);
        arg_blake2b_update(&s, in, inlen);
        arg_blake2b_final(&s, out);
        return 0;
    }
    arg_blake2b_init(&s, 64);
    arg_blake2b_update(&s, ob, 4);
    arg_blake2b_update(&s, in, inlen);
    arg_blake2b_final(&s, outbuf);
    memcpy(out, outbuf, 32);
    out += 32;
    outlen -= 32;
    while (outlen > 64) {
        arg_blake2b_init(&s, 64);
        arg_blake2b_update(&s, outbuf, 64);
        arg_blake2b_final(&s, outbuf);
        memcpy(out, outbuf, 32);
        out += 32;
        outlen -= 32;
    }
    arg_blake2b_init(&s, outlen);
    arg_blake2b_update(&s, outbuf, 64);
    arg_blake2b_final(&s, out);
    memset(outbuf, 0, sizeof outbuf);
    return 0;
}

static void arg_fill_block(const arg_block* prev, const arg_block* ref,
    arg_block* next, int with_xor)
{
    arg_block blockR, block_tmp;
    unsigned i;

    memcpy(blockR.v, ref->v, sizeof blockR.v);
    for (i = 0; i < ARG2_QWORDS_IN_BLOCK; i++)
        blockR.v[i] ^= prev->v[i];
    memcpy(block_tmp.v, blockR.v, sizeof block_tmp.v);
    if (with_xor)
        for (i = 0; i < ARG2_QWORDS_IN_BLOCK; i++)
            block_tmp.v[i] ^= next->v[i];

    for (i = 0; i < 8; i++)
        ARG_ROUND(blockR.v[16 * i], blockR.v[16 * i + 1],
            blockR.v[16 * i + 2], blockR.v[16 * i + 3],
            blockR.v[16 * i + 4], blockR.v[16 * i + 5],
            blockR.v[16 * i + 6], blockR.v[16 * i + 7],
            blockR.v[16 * i + 8], blockR.v[16 * i + 9],
            blockR.v[16 * i + 10], blockR.v[16 * i + 11],
            blockR.v[16 * i + 12], blockR.v[16 * i + 13],
            blockR.v[16 * i + 14], blockR.v[16 * i + 15]);
    for (i = 0; i < 8; i++)
        ARG_ROUND(blockR.v[2 * i], blockR.v[2 * i + 1],
            blockR.v[2 * i + 16], blockR.v[2 * i + 17],
            blockR.v[2 * i + 32], blockR.v[2 * i + 33],
            blockR.v[2 * i + 48], blockR.v[2 * i + 49],
            blockR.v[2 * i + 64], blockR.v[2 * i + 65],
            blockR.v[2 * i + 80], blockR.v[2 * i + 81],
            blockR.v[2 * i + 96], blockR.v[2 * i + 97],
            blockR.v[2 * i + 112], blockR.v[2 * i + 113]);

    memcpy(next->v, block_tmp.v, sizeof next->v);
    for (i = 0; i < ARG2_QWORDS_IN_BLOCK; i++)
        next->v[i] ^= blockR.v[i];
    dyn_wipe(blockR.v, sizeof blockR.v);
    dyn_wipe(block_tmp.v, sizeof block_tmp.v);
}

static void arg_next_addresses(arg_block* address_block,
    arg_block* input_block,
    const arg_block* zero_block)
{
    input_block->v[6]++;
    arg_fill_block(zero_block, input_block, address_block, 0);
    arg_fill_block(zero_block, address_block, address_block, 0);
}

static uint32_t arg_index_alpha(const arg_instance* instance, uint32_t pass,
    uint32_t slice, uint32_t index,
    uint32_t pseudo_rand, int same_lane)
{
    uint32_t reference_area_size;
    uint64_t relative_position;
    uint32_t start_position, absolute_position;

    if (0 == pass) {
        if (0 == slice)
            reference_area_size = index - 1;
        else if (same_lane)
            reference_area_size = slice * instance->segment_length + index - 1;
        else
            reference_area_size = slice * instance->segment_length + ((index == 0) ? UINT32_MAX : 0);
    } else if (same_lane) {
        reference_area_size = instance->lane_length - instance->segment_length + index - 1;
    } else {
        reference_area_size = instance->lane_length - instance->segment_length + ((index == 0) ? UINT32_MAX : 0);
    }
    relative_position = pseudo_rand;
    relative_position = relative_position * relative_position >> 32;
    relative_position = reference_area_size - 1 - (reference_area_size * relative_position >> 32);
    start_position = 0;
    if (0 != pass)
        start_position = (slice == ARG2_SYNC_POINTS - 1)
            ? 0
            : (slice + 1) * instance->segment_length;
    absolute_position = (start_position + (uint32_t)relative_position) % instance->lane_length;
    return absolute_position;
}

static void arg_fill_segment(const arg_instance* instance, uint32_t pass,
    uint32_t lane, uint32_t slice)
{
    arg_block address_block, input_block, zero_block;
    uint64_t pseudo_rand, ref_lane;
    uint32_t ref_index, prev_offset, curr_offset, starting_index, i;
    int data_independent_addressing, blocks_init = 0;

    data_independent_addressing = (pass == 0) && (slice < ARG2_SYNC_POINTS / 2);

    if (data_independent_addressing) {
        memset(&zero_block, 0, sizeof zero_block);
        memset(&input_block, 0, sizeof input_block);
        input_block.v[0] = pass;
        input_block.v[1] = lane;
        input_block.v[2] = slice;
        input_block.v[3] = instance->memory_blocks;
        input_block.v[4] = instance->passes;
        input_block.v[5] = ARG2_TYPE_ID;
        blocks_init = 1;
    }

    starting_index = 0;
    if ((0 == pass) && (0 == slice)) {
        starting_index = 2;
        if (data_independent_addressing) {
            arg_next_addresses(&address_block, &input_block, &zero_block);
            blocks_init = 1;
        }
    }
    curr_offset = lane * instance->lane_length + slice * instance->segment_length + starting_index;
    prev_offset = (0 == curr_offset % instance->lane_length)
        ? curr_offset + instance->lane_length - 1
        : curr_offset - 1;

    for (i = starting_index; i < instance->segment_length;
        i++, curr_offset++, prev_offset++) {
        if (curr_offset % instance->lane_length == 1)
            prev_offset = curr_offset - 1;
        if (data_independent_addressing) {
            if (i % ARG2_ADDRESSES_IN_BLOCK == 0)
                arg_next_addresses(&address_block, &input_block, &zero_block);
            pseudo_rand = address_block.v[i % ARG2_ADDRESSES_IN_BLOCK];
        } else {
            pseudo_rand = instance->memory[prev_offset].v[0];
        }
        ref_lane = (pseudo_rand >> 32) % instance->lanes;
        if ((pass == 0) && (slice == 0))
            ref_lane = lane;
        ref_index = arg_index_alpha(instance, pass, slice, i,
            (uint32_t)(pseudo_rand & 0xFFFFFFFF),
            ref_lane == lane);
        {
            const arg_block* ref = &instance->memory[(size_t)ref_lane * instance->lane_length + ref_index];
            arg_block* curr = &instance->memory[curr_offset];
            arg_fill_block(&instance->memory[prev_offset], ref, curr,
                pass == 0 ? 0 : 1);
        }
    }
    if (blocks_init) {
        memset(&address_block, 0, sizeof address_block);
        memset(&input_block, 0, sizeof input_block);
        memset(&zero_block, 0, sizeof zero_block);
    }
}

static void arg_fill_lanes(void* arg, unsigned worker, unsigned nworkers,
    dyn_par_t* par)
{
    const arg_instance* inst = (const arg_instance*)arg;
    uint32_t r, s, l;
    for (r = 0; r < inst->passes; r++)
        for (s = 0; s < ARG2_SYNC_POINTS; s++) {
            for (l = worker; l < inst->lanes; l += nworkers)
                arg_fill_segment(inst, r, l, s);
            dyn_par_barrier(par);
        }
}

static void arg_initial_hash(uint8_t* blockhash, uint32_t lanes,
    uint32_t outlen, uint32_t m_cost, uint32_t t_cost,
    const uint8_t* pwd, size_t pwdlen,
    const uint8_t* salt, size_t saltlen)
{
    arg_blake2b_ctx S;
    uint8_t value[4];

    arg_blake2b_init(&S, ARG2_PREHASH_DIGEST_LENGTH);
    arg_store32(value, lanes);
    arg_blake2b_update(&S, value, 4);
    arg_store32(value, outlen);
    arg_blake2b_update(&S, value, 4);
    arg_store32(value, m_cost);
    arg_blake2b_update(&S, value, 4);
    arg_store32(value, t_cost);
    arg_blake2b_update(&S, value, 4);
    arg_store32(value, ARG2_VERSION_NUMBER);
    arg_blake2b_update(&S, value, 4);
    arg_store32(value, ARG2_TYPE_ID);
    arg_blake2b_update(&S, value, 4);
    arg_store32(value, (uint32_t)pwdlen);
    arg_blake2b_update(&S, value, 4);
    if (pwdlen)
        arg_blake2b_update(&S, pwd, pwdlen);
    arg_store32(value, (uint32_t)saltlen);
    arg_blake2b_update(&S, value, 4);
    if (saltlen)
        arg_blake2b_update(&S, salt, saltlen);
    arg_store32(value, 0);
    arg_blake2b_update(&S, value, 4);
    arg_store32(value, 0);
    arg_blake2b_update(&S, value, 4);
    arg_blake2b_final(&S, blockhash);
}

int arg_hash_id(const uint8_t* pwd, size_t pwdlen,
    const uint8_t* salt, size_t saltlen,
    uint32_t t, uint32_t m, uint32_t lanes,
    uint8_t* out, size_t outlen)
{
    arg_instance inst;
    arg_block blockhash_b;
    uint8_t blockhash[ARG2_PREHASH_SEED_LENGTH];
    uint8_t blockhash_bytes[ARG2_BLOCK_SIZE];
    uint32_t memory_blocks, segment_length, l, i;
    int rc = -1;

    if (lanes < 1 || lanes > ARG2_MAX_LANES)
        return -1;
    if (m < 8 * lanes || m > ARG2_MAX_MEMORY)
        return -1;
    if (t < 1 || t > ARG2_MAX_TIME)
        return -1;
    if (outlen < 4 || outlen > ARG2_MAX_OUT)
        return -1;
    if (pwdlen > ARG2_MAX_PWD || saltlen > ARG2_MAX_SALT)
        return -1;
    if (saltlen < ARG2_MIN_SALT_LEN)
        return -1;

    memory_blocks = m;
    if (memory_blocks < 2 * ARG2_SYNC_POINTS * lanes)
        memory_blocks = 2 * ARG2_SYNC_POINTS * lanes;
    segment_length = memory_blocks / (lanes * ARG2_SYNC_POINTS);
    memory_blocks = segment_length * (lanes * ARG2_SYNC_POINTS);

    inst.passes = t;
    inst.memory_blocks = memory_blocks;
    inst.segment_length = segment_length;
    inst.lane_length = segment_length * ARG2_SYNC_POINTS;
    inst.lanes = lanes;

    inst.memory = (arg_block*)calloc(memory_blocks, sizeof(arg_block));
    if (!inst.memory)
        return -2;

    arg_initial_hash(blockhash, lanes, (uint32_t)outlen, m, t,
        pwd, pwdlen, salt, saltlen);
    memset(blockhash + ARG2_PREHASH_DIGEST_LENGTH, 0,
        ARG2_PREHASH_SEED_LENGTH - ARG2_PREHASH_DIGEST_LENGTH);

    for (l = 0; l < lanes; l++) {
        arg_store32(blockhash + ARG2_PREHASH_DIGEST_LENGTH, 0);
        arg_store32(blockhash + ARG2_PREHASH_DIGEST_LENGTH + 4, l);
        arg_blake2b_long((uint8_t*)&inst.memory[(size_t)l * inst.lane_length],
            ARG2_BLOCK_SIZE, blockhash, ARG2_PREHASH_SEED_LENGTH);
        arg_store32(blockhash + ARG2_PREHASH_DIGEST_LENGTH, 1);
        arg_blake2b_long((uint8_t*)&inst.memory[(size_t)l * inst.lane_length + 1],
            ARG2_BLOCK_SIZE, blockhash, ARG2_PREHASH_SEED_LENGTH);
    }
    dyn_wipe(blockhash, sizeof blockhash);

    if (segment_length >= ARG2_PAR_MIN_SEGMENT_BLOCKS)
        dyn_par_run(dyn_par_plan(lanes, 1, lanes), arg_fill_lanes, &inst);
    else
        arg_fill_lanes(&inst, 0, 1, NULL);

    memcpy(blockhash_b.v, inst.memory[inst.lane_length - 1].v,
        ARG2_BLOCK_SIZE);
    for (l = 1; l < lanes; l++)
        for (i = 0; i < ARG2_QWORDS_IN_BLOCK; i++)
            blockhash_b.v[i] ^= inst.memory[(size_t)l * inst.lane_length + (inst.lane_length - 1)].v[i];
    arg_store_block(blockhash_bytes, &blockhash_b);
    arg_blake2b_long(out, outlen, blockhash_bytes, ARG2_BLOCK_SIZE);
    rc = 0;

    dyn_wipe(blockhash_bytes, sizeof blockhash_bytes);
    dyn_wipe(blockhash_b.v, sizeof blockhash_b.v);
    dyn_wipe(inst.memory, (size_t)memory_blocks * sizeof(arg_block));
    free(inst.memory);
    return rc;
}

static size_t arg_b64u_encode(const uint8_t* d, size_t n, char* out)
{
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, o = 0;
    for (i = 0; i + 3 <= n; i += 3) {
        uint32_t v = ((uint32_t)d[i] << 16) | ((uint32_t)d[i + 1] << 8) | d[i + 2];
        out[o++] = A[(v >> 18) & 63];
        out[o++] = A[(v >> 12) & 63];
        out[o++] = A[(v >> 6) & 63];
        out[o++] = A[v & 63];
    }
    if (n - i == 1) {
        uint32_t v = (uint32_t)d[i] << 16;
        out[o++] = A[(v >> 18) & 63];
        out[o++] = A[(v >> 12) & 63];
    } else if (n - i == 2) {
        uint32_t v = ((uint32_t)d[i] << 16) | ((uint32_t)d[i + 1] << 8);
        out[o++] = A[(v >> 18) & 63];
        out[o++] = A[(v >> 12) & 63];
        out[o++] = A[(v >> 6) & 63];
    }
    return o;
}

int arg_b64u_decode(const char* s, size_t n, uint8_t* out, size_t* outn)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t i, o = 0;
    for (i = 0; i < n; i++) {
        char c = s[i];
        int v;
        if (c >= 'A' && c <= 'Z')
            v = c - 'A';
        else if (c >= 'a' && c <= 'z')
            v = c - 'a' + 26;
        else if (c >= '0' && c <= '9')
            v = c - '0' + 52;
        else if (c == '+')
            v = 62;
        else if (c == '/')
            v = 63;
        else
            return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    if (acc & ((1u << bits) - 1))
        return -1;
    *outn = o;
    return 0;
}

char* arg_phc_encode(const uint8_t* salt, size_t saltlen,
    const uint8_t* tag, size_t taglen,
    uint32_t t, uint32_t m, uint32_t p)
{
    size_t need;
    char* s;
    size_t o;
    if (saltlen > (SIZE_MAX - 80) / 4 || taglen > (SIZE_MAX - 80) / 4)
        return NULL;
    need = saltlen * 4 / 3 + 8 + taglen * 4 / 3 + 8 + 64;
    s = (char*)malloc(need);
    if (!s)
        return NULL;
    o = (size_t)snprintf(s, 48, "$argon2id$v=19$m=%u,t=%u,p=%u$", m, t, p);
    o += arg_b64u_encode(salt, saltlen, s + o);
    s[o++] = '$';
    o += arg_b64u_encode(tag, taglen, s + o);
    s[o] = 0;
    return s;
}

int arg_phc_parse(const char* str, const char** psalt, size_t* psaltlen,
    const char** ptag, size_t* ptaglen,
    uint32_t* pt, uint32_t* pm, uint32_t* pp)
{
    const char* f[6];
    size_t fl[6];
    int fi = 0, i;
    const char* p = str;
    size_t n = strlen(str);
    uint64_t t = 0, m = 0, pp64 = 0;

    if (n == 0 || n > 4096)
        return -1;
    f[0] = p;
    fl[0] = 0;
    for (i = 0; i < (int)n; i++) {
        if (p[i] == '$') {
            if (++fi > 5)
                return -1;
            f[fi] = p + i + 1;
            fl[fi] = 0;
        } else {
            fl[fi]++;
        }
    }
    if (fi != 5)
        return -1;
    if (fl[0] != 0 || fl[1] != 8 || memcmp(f[1], "argon2id", 8))
        return -1;
    if (fl[2] != 4 || memcmp(f[2], "v=19", 4))
        return -1;
    {
        char tmp[24];
        const char* q = f[3];
        size_t rest = fl[3];
        static const char* const keys[3] = { "m=", "t=", "p=" };
        uint64_t* vals[3] = { &m, &t, &pp64 };
        int k;
        for (k = 0; k < 3; k++) {
            const char* comma;
            size_t L;
            size_t kl = strlen(keys[k]);
            if (rest < kl + 1 || memcmp(q, keys[k], kl))
                return -1;
            q += kl;
            rest -= kl;
            comma = memchr(q, ',', rest);
            L = comma ? (size_t)(comma - q) : rest;
            if (L == 0 || L >= sizeof tmp)
                return -1;
            memcpy(tmp, q, L);
            tmp[L] = 0;
            if (strspn(tmp, "0123456789") != L)
                return -1;
            *vals[k] = (uint64_t)strtoull(tmp, NULL, 10);
            if (comma) {
                q = comma + 1;
                rest -= L + 1;
            } else {
                rest = 0;
            }
        }
        if (rest != 0)
            return -1;
    }
    if (!m || !t || !pp64 || m > ARG2_MAX_MEMORY || t > ARG2_MAX_TIME || pp64 > ARG2_MAX_LANES)
        return -1;
    if (m > ARG2_MAX_SYNC_MEMORY)
        return -2;
    if (fl[4] < 11)
        return -1;
    if (fl[5] == 0 || fl[5] > (size_t)ARG2_MAX_OUT * 4 / 3 + 4)
        return -1;
    *psalt = f[4];
    *psaltlen = fl[4];
    *ptag = f[5];
    *ptaglen = fl[5];
    *pt = (uint32_t)t;
    *pm = (uint32_t)m;
    *pp = (uint32_t)pp64;
    return 0;
}
