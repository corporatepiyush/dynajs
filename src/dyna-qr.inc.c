/* QR Code (ISO/IEC 18004) encoding for dyna:encoding.
   Byte mode, versions 1-40, all four EC levels. Design 24 asked for a whole
   image module; the review cut it to QR, which is pure integer arithmetic --
   Reed-Solomon over GF(256) and bit placement -- and needs no codec at all.
   The tables below are specification data; everything acting on them is here. */

#define QR_MAX_SIZE 177                 /* version 40 */
#define QR_MAX_CW   3706                /* version 40 total codewords */

static const uint8_t QR_EXP[512] = {
      1,   2,   4,   8,  16,  32,  64, 128,  29,  58, 116, 232, 205, 135,  19,  38,
     76, 152,  45,  90, 180, 117, 234, 201, 143,   3,   6,  12,  24,  48,  96, 192,
    157,  39,  78, 156,  37,  74, 148,  53, 106, 212, 181, 119, 238, 193, 159,  35,
     70, 140,   5,  10,  20,  40,  80, 160,  93, 186, 105, 210, 185, 111, 222, 161,
     95, 190,  97, 194, 153,  47,  94, 188, 101, 202, 137,  15,  30,  60, 120, 240,
    253, 231, 211, 187, 107, 214, 177, 127, 254, 225, 223, 163,  91, 182, 113, 226,
    217, 175,  67, 134,  17,  34,  68, 136,  13,  26,  52, 104, 208, 189, 103, 206,
    129,  31,  62, 124, 248, 237, 199, 147,  59, 118, 236, 197, 151,  51, 102, 204,
    133,  23,  46,  92, 184, 109, 218, 169,  79, 158,  33,  66, 132,  21,  42,  84,
    168,  77, 154,  41,  82, 164,  85, 170,  73, 146,  57, 114, 228, 213, 183, 115,
    230, 209, 191,  99, 198, 145,  63, 126, 252, 229, 215, 179, 123, 246, 241, 255,
    227, 219, 171,  75, 150,  49,  98, 196, 149,  55, 110, 220, 165,  87, 174,  65,
    130,  25,  50, 100, 200, 141,   7,  14,  28,  56, 112, 224, 221, 167,  83, 166,
     81, 162,  89, 178, 121, 242, 249, 239, 195, 155,  43,  86, 172,  69, 138,   9,
     18,  36,  72, 144,  61, 122, 244, 245, 247, 243, 251, 235, 203, 139,  11,  22,
     44,  88, 176, 125, 250, 233, 207, 131,  27,  54, 108, 216, 173,  71, 142,   1,
      2,   4,   8,  16,  32,  64, 128,  29,  58, 116, 232, 205, 135,  19,  38,  76,
    152,  45,  90, 180, 117, 234, 201, 143,   3,   6,  12,  24,  48,  96, 192, 157,
     39,  78, 156,  37,  74, 148,  53, 106, 212, 181, 119, 238, 193, 159,  35,  70,
    140,   5,  10,  20,  40,  80, 160,  93, 186, 105, 210, 185, 111, 222, 161,  95,
    190,  97, 194, 153,  47,  94, 188, 101, 202, 137,  15,  30,  60, 120, 240, 253,
    231, 211, 187, 107, 214, 177, 127, 254, 225, 223, 163,  91, 182, 113, 226, 217,
    175,  67, 134,  17,  34,  68, 136,  13,  26,  52, 104, 208, 189, 103, 206, 129,
     31,  62, 124, 248, 237, 199, 147,  59, 118, 236, 197, 151,  51, 102, 204, 133,
     23,  46,  92, 184, 109, 218, 169,  79, 158,  33,  66, 132,  21,  42,  84, 168,
     77, 154,  41,  82, 164,  85, 170,  73, 146,  57, 114, 228, 213, 183, 115, 230,
    209, 191,  99, 198, 145,  63, 126, 252, 229, 215, 179, 123, 246, 241, 255, 227,
    219, 171,  75, 150,  49,  98, 196, 149,  55, 110, 220, 165,  87, 174,  65, 130,
     25,  50, 100, 200, 141,   7,  14,  28,  56, 112, 224, 221, 167,  83, 166,  81,
    162,  89, 178, 121, 242, 249, 239, 195, 155,  43,  86, 172,  69, 138,   9,  18,
     36,  72, 144,  61, 122, 244, 245, 247, 243, 251, 235, 203, 139,  11,  22,  44,
     88, 176, 125, 250, 233, 207, 131,  27,  54, 108, 216, 173,  71, 142,   1,   2,
};
static const uint8_t QR_LOG[256] = {
      0,   0,   1,  25,   2,  50,  26, 198,   3, 223,  51, 238,  27, 104, 199,  75,
      4, 100, 224,  14,  52, 141, 239, 129,  28, 193, 105, 248, 200,   8,  76, 113,
      5, 138, 101,  47, 225,  36,  15,  33,  53, 147, 142, 218, 240,  18, 130,  69,
     29, 181, 194, 125, 106,  39, 249, 185, 201, 154,   9, 120,  77, 228, 114, 166,
      6, 191, 139,  98, 102, 221,  48, 253, 226, 152,  37, 179,  16, 145,  34, 136,
     54, 208, 148, 206, 143, 150, 219, 189, 241, 210,  19,  92, 131,  56,  70,  64,
     30,  66, 182, 163, 195,  72, 126, 110, 107,  58,  40,  84, 250, 133, 186,  61,
    202,  94, 155, 159,  10,  21, 121,  43,  78, 212, 229, 172, 115, 243, 167,  87,
      7, 112, 192, 247, 140, 128,  99,  13, 103,  74, 222, 237,  49, 197, 254,  24,
    227, 165, 153, 119,  38, 184, 180, 124,  17,  68, 146, 217,  35,  32, 137,  46,
     55,  63, 209,  91, 149, 188, 207, 205, 144, 135, 151, 178, 220, 252, 190,  97,
    242,  86, 211, 171,  20,  42,  93, 158, 132,  60,  57,  83,  71, 109,  65, 162,
     31,  45,  67, 216, 183, 123, 164, 118, 196,  23,  73, 236, 127,  12, 111, 246,
    108, 161,  59,  82,  41, 157,  85, 170, 251,  96, 134, 177, 187, 204,  62,  90,
    203,  89,  95, 176, 156, 169, 160,  81,  11, 245,  22, 235, 122, 117,  44, 215,
     79, 174, 213, 233, 230, 231, 173, 232, 116, 214, 244, 234, 168,  80,  88, 175,
};
/* [(version-1)*4 + ecl] = {ec per block, blocks g1, data g1, blocks g2, data g2}
   ecl is 0=L 1=M 2=Q 3=H. From the specification's block tables. */
static const uint16_t QR_ECC[160][5] = {
    {  7,  1, 19,  0,  0}, { 10,  1, 16,  0,  0}, { 13,  1, 13,  0,  0}, { 17,  1,  9,  0,  0},   /* v1 */
    { 10,  1, 34,  0,  0}, { 16,  1, 28,  0,  0}, { 22,  1, 22,  0,  0}, { 28,  1, 16,  0,  0},   /* v2 */
    { 15,  1, 55,  0,  0}, { 26,  1, 44,  0,  0}, { 18,  2, 17,  0,  0}, { 22,  2, 13,  0,  0},   /* v3 */
    { 20,  1, 80,  0,  0}, { 18,  2, 32,  0,  0}, { 26,  2, 24,  0,  0}, { 16,  4,  9,  0,  0},   /* v4 */
    { 26,  1,108,  0,  0}, { 24,  2, 43,  0,  0}, { 18,  2, 15,  2, 16}, { 22,  2, 11,  2, 12},   /* v5 */
    { 18,  2, 68,  0,  0}, { 16,  4, 27,  0,  0}, { 24,  4, 19,  0,  0}, { 28,  4, 15,  0,  0},   /* v6 */
    { 20,  2, 78,  0,  0}, { 18,  4, 31,  0,  0}, { 18,  2, 14,  4, 15}, { 26,  4, 13,  1, 14},   /* v7 */
    { 24,  2, 97,  0,  0}, { 22,  2, 38,  2, 39}, { 22,  4, 18,  2, 19}, { 26,  4, 14,  2, 15},   /* v8 */
    { 30,  2,116,  0,  0}, { 22,  3, 36,  2, 37}, { 20,  4, 16,  4, 17}, { 24,  4, 12,  4, 13},   /* v9 */
    { 18,  2, 68,  2, 69}, { 26,  4, 43,  1, 44}, { 24,  6, 19,  2, 20}, { 28,  6, 15,  2, 16},   /* v10 */
    { 20,  4, 81,  0,  0}, { 30,  1, 50,  4, 51}, { 28,  4, 22,  4, 23}, { 24,  3, 12,  8, 13},   /* v11 */
    { 24,  2, 92,  2, 93}, { 22,  6, 36,  2, 37}, { 26,  4, 20,  6, 21}, { 28,  7, 14,  4, 15},   /* v12 */
    { 26,  4,107,  0,  0}, { 22,  8, 37,  1, 38}, { 24,  8, 20,  4, 21}, { 22, 12, 11,  4, 12},   /* v13 */
    { 30,  3,115,  1,116}, { 24,  4, 40,  5, 41}, { 20, 11, 16,  5, 17}, { 24, 11, 12,  5, 13},   /* v14 */
    { 22,  5, 87,  1, 88}, { 24,  5, 41,  5, 42}, { 30,  5, 24,  7, 25}, { 24, 11, 12,  7, 13},   /* v15 */
    { 24,  5, 98,  1, 99}, { 28,  7, 45,  3, 46}, { 24, 15, 19,  2, 20}, { 30,  3, 15, 13, 16},   /* v16 */
    { 28,  1,107,  5,108}, { 28, 10, 46,  1, 47}, { 28,  1, 22, 15, 23}, { 28,  2, 14, 17, 15},   /* v17 */
    { 30,  5,120,  1,121}, { 26,  9, 43,  4, 44}, { 28, 17, 22,  1, 23}, { 28,  2, 14, 19, 15},   /* v18 */
    { 28,  3,113,  4,114}, { 26,  3, 44, 11, 45}, { 26, 17, 21,  4, 22}, { 26,  9, 13, 16, 14},   /* v19 */
    { 28,  3,107,  5,108}, { 26,  3, 41, 13, 42}, { 30, 15, 24,  5, 25}, { 28, 15, 15, 10, 16},   /* v20 */
    { 28,  4,116,  4,117}, { 26, 17, 42,  0,  0}, { 28, 17, 22,  6, 23}, { 30, 19, 16,  6, 17},   /* v21 */
    { 28,  2,111,  7,112}, { 28, 17, 46,  0,  0}, { 30,  7, 24, 16, 25}, { 24, 34, 13,  0,  0},   /* v22 */
    { 30,  4,121,  5,122}, { 28,  4, 47, 14, 48}, { 30, 11, 24, 14, 25}, { 30, 16, 15, 14, 16},   /* v23 */
    { 30,  6,117,  4,118}, { 28,  6, 45, 14, 46}, { 30, 11, 24, 16, 25}, { 30, 30, 16,  2, 17},   /* v24 */
    { 26,  8,106,  4,107}, { 28,  8, 47, 13, 48}, { 30,  7, 24, 22, 25}, { 30, 22, 15, 13, 16},   /* v25 */
    { 28, 10,114,  2,115}, { 28, 19, 46,  4, 47}, { 28, 28, 22,  6, 23}, { 30, 33, 16,  4, 17},   /* v26 */
    { 30,  8,122,  4,123}, { 28, 22, 45,  3, 46}, { 30,  8, 23, 26, 24}, { 30, 12, 15, 28, 16},   /* v27 */
    { 30,  3,117, 10,118}, { 28,  3, 45, 23, 46}, { 30,  4, 24, 31, 25}, { 30, 11, 15, 31, 16},   /* v28 */
    { 30,  7,116,  7,117}, { 28, 21, 45,  7, 46}, { 30,  1, 23, 37, 24}, { 30, 19, 15, 26, 16},   /* v29 */
    { 30,  5,115, 10,116}, { 28, 19, 47, 10, 48}, { 30, 15, 24, 25, 25}, { 30, 23, 15, 25, 16},   /* v30 */
    { 30, 13,115,  3,116}, { 28,  2, 46, 29, 47}, { 30, 42, 24,  1, 25}, { 30, 23, 15, 28, 16},   /* v31 */
    { 30, 17,115,  0,  0}, { 28, 10, 46, 23, 47}, { 30, 10, 24, 35, 25}, { 30, 19, 15, 35, 16},   /* v32 */
    { 30, 17,115,  1,116}, { 28, 14, 46, 21, 47}, { 30, 29, 24, 19, 25}, { 30, 11, 15, 46, 16},   /* v33 */
    { 30, 13,115,  6,116}, { 28, 14, 46, 23, 47}, { 30, 44, 24,  7, 25}, { 30, 59, 16,  1, 17},   /* v34 */
    { 30, 12,121,  7,122}, { 28, 12, 47, 26, 48}, { 30, 39, 24, 14, 25}, { 30, 22, 15, 41, 16},   /* v35 */
    { 30,  6,121, 14,122}, { 28,  6, 47, 34, 48}, { 30, 46, 24, 10, 25}, { 30,  2, 15, 64, 16},   /* v36 */
    { 30, 17,122,  4,123}, { 28, 29, 46, 14, 47}, { 30, 49, 24, 10, 25}, { 30, 24, 15, 46, 16},   /* v37 */
    { 30,  4,122, 18,123}, { 28, 13, 46, 32, 47}, { 30, 48, 24, 14, 25}, { 30, 42, 15, 32, 16},   /* v38 */
    { 30, 20,117,  4,118}, { 28, 40, 47,  7, 48}, { 30, 43, 24, 22, 25}, { 30, 10, 15, 67, 16},   /* v39 */
    { 30, 19,118,  6,119}, { 28, 18, 47, 31, 48}, { 30, 34, 24, 34, 25}, { 30, 20, 15, 61, 16},   /* v40 */
};

/* Alignment-pattern centre coordinates per version; version 1 has none. */
static const uint8_t QR_ALIGN[41][8] = {
    {  0,  0,  0,  0,  0,  0,  0,  0},   /* v0 */
    {  0,  0,  0,  0,  0,  0,  0,  0},   /* v1 */
    {  6, 18,  0,  0,  0,  0,  0,  0},   /* v2 */
    {  6, 22,  0,  0,  0,  0,  0,  0},   /* v3 */
    {  6, 26,  0,  0,  0,  0,  0,  0},   /* v4 */
    {  6, 30,  0,  0,  0,  0,  0,  0},   /* v5 */
    {  6, 34,  0,  0,  0,  0,  0,  0},   /* v6 */
    {  6, 22, 38,  0,  0,  0,  0,  0},   /* v7 */
    {  6, 24, 42,  0,  0,  0,  0,  0},   /* v8 */
    {  6, 26, 46,  0,  0,  0,  0,  0},   /* v9 */
    {  6, 28, 50,  0,  0,  0,  0,  0},   /* v10 */
    {  6, 30, 54,  0,  0,  0,  0,  0},   /* v11 */
    {  6, 32, 58,  0,  0,  0,  0,  0},   /* v12 */
    {  6, 34, 62,  0,  0,  0,  0,  0},   /* v13 */
    {  6, 26, 46, 66,  0,  0,  0,  0},   /* v14 */
    {  6, 26, 48, 70,  0,  0,  0,  0},   /* v15 */
    {  6, 26, 50, 74,  0,  0,  0,  0},   /* v16 */
    {  6, 30, 54, 78,  0,  0,  0,  0},   /* v17 */
    {  6, 30, 56, 82,  0,  0,  0,  0},   /* v18 */
    {  6, 30, 58, 86,  0,  0,  0,  0},   /* v19 */
    {  6, 34, 62, 90,  0,  0,  0,  0},   /* v20 */
    {  6, 28, 50, 72, 94,  0,  0,  0},   /* v21 */
    {  6, 26, 50, 74, 98,  0,  0,  0},   /* v22 */
    {  6, 30, 54, 78,102,  0,  0,  0},   /* v23 */
    {  6, 28, 54, 80,106,  0,  0,  0},   /* v24 */
    {  6, 32, 58, 84,110,  0,  0,  0},   /* v25 */
    {  6, 30, 58, 86,114,  0,  0,  0},   /* v26 */
    {  6, 34, 62, 90,118,  0,  0,  0},   /* v27 */
    {  6, 26, 50, 74, 98,122,  0,  0},   /* v28 */
    {  6, 30, 54, 78,102,126,  0,  0},   /* v29 */
    {  6, 26, 52, 78,104,130,  0,  0},   /* v30 */
    {  6, 30, 56, 82,108,134,  0,  0},   /* v31 */
    {  6, 34, 60, 86,112,138,  0,  0},   /* v32 */
    {  6, 30, 58, 86,114,142,  0,  0},   /* v33 */
    {  6, 34, 62, 90,118,146,  0,  0},   /* v34 */
    {  6, 30, 54, 78,102,126,150,  0},   /* v35 */
    {  6, 24, 50, 76,102,128,154,  0},   /* v36 */
    {  6, 28, 54, 80,106,132,158,  0},   /* v37 */
    {  6, 32, 58, 84,110,136,162,  0},   /* v38 */
    {  6, 26, 54, 82,110,138,166,  0},   /* v39 */
    {  6, 30, 58, 86,114,142,170,  0},   /* v40 */
};

/* ---- GF(256) ---- */

static uint8_t qr_mul(uint8_t a, uint8_t b)
{
    return (a && b) ? QR_EXP[QR_LOG[a] + QR_LOG[b]] : 0;
}

/* The generator polynomial for `n` error-correction codewords, built by
   multiplying out (x - a^0)(x - a^1)...  Cheap, and it keeps the 40-odd
   generator polynomials out of the binary. */
static void qr_gen_poly(int n, uint8_t *g)
{
    int i, j;

    memset(g, 0, (size_t)n);
    g[n - 1] = 1;
    for (i = 0; i < n; i++) {
        for (j = 0; j < n - 1; j++)
            g[j] = qr_mul(g[j], QR_EXP[i]) ^ g[j + 1];
        g[n - 1] = qr_mul(g[n - 1], QR_EXP[i]);
    }
}

/* Remainder of data*x^n modulo the generator: the EC codewords. */
static void qr_rs(const uint8_t *data, int dlen, int n, const uint8_t *g,
                  uint8_t *ec)
{
    int i, j;

    memset(ec, 0, (size_t)n);
    for (i = 0; i < dlen; i++) {
        uint8_t f = data[i] ^ ec[0];
        memmove(ec, ec + 1, (size_t)(n - 1));
        ec[n - 1] = 0;
        for (j = 0; j < n; j++)
            ec[j] ^= qr_mul(g[j], f);
    }
}

/* ---- the module grid ----
   Two bits per cell: DARK is the colour, FUNC marks a module the data stream
   must skip and the mask must not flip. Keeping them in one byte means the
   placement loop tests one array, not two. */
#define QR_DARK 1u
#define QR_FUNC 2u

typedef struct {
    uint8_t m[QR_MAX_SIZE * QR_MAX_SIZE];
    int     size;
    int     version;
} qr_grid_t;

static void qr_set(qr_grid_t *q, int x, int y, unsigned v)
{
    if (x >= 0 && y >= 0 && x < q->size && y < q->size)
        q->m[y * q->size + x] = (uint8_t)v;
}

static unsigned qr_get(const qr_grid_t *q, int x, int y)
{
    return q->m[y * q->size + x];
}

static void qr_rect(qr_grid_t *q, int x0, int y0, int w, int h, unsigned v)
{
    int x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            qr_set(q, x0 + x, y0 + y, v);
}

static void qr_finder(qr_grid_t *q, int x, int y)
{
    qr_rect(q, x - 1, y - 1, 9, 9, QR_FUNC);        /* separator ring */
    qr_rect(q, x, y, 7, 7, QR_FUNC | QR_DARK);
    qr_rect(q, x + 1, y + 1, 5, 5, QR_FUNC);
    qr_rect(q, x + 2, y + 2, 3, 3, QR_FUNC | QR_DARK);
}

static void qr_functions(qr_grid_t *q)
{
    const uint8_t *ap = QR_ALIGN[q->version];
    int i, j, n = 0, s = q->size;

    qr_finder(q, 0, 0);
    qr_finder(q, s - 7, 0);
    qr_finder(q, 0, s - 7);
    for (i = 8; i < s - 8; i++) {       /* timing patterns */
        unsigned v = QR_FUNC | ((i & 1) ? 0u : QR_DARK);
        qr_set(q, i, 6, v);
        qr_set(q, 6, i, v);
    }
    while (n < 8 && ap[n])
        n++;
    for (i = 0; i < n; i++) {
        for (j = 0; j < n; j++) {
            /* The three corners already hold finders. */
            if ((i == 0 && j == 0) || (i == 0 && j == n - 1)
                || (i == n - 1 && j == 0))
                continue;
            qr_rect(q, ap[j] - 2, ap[i] - 2, 5, 5, QR_FUNC | QR_DARK);
            qr_rect(q, ap[j] - 1, ap[i] - 1, 3, 3, QR_FUNC);
            qr_set(q, ap[j], ap[i], QR_FUNC | QR_DARK);
        }
    }
    qr_set(q, 8, s - 8, QR_FUNC | QR_DARK);         /* the dark module */
    for (i = 0; i < 9; i++) {                       /* format-info reserve */
        if (i != 6) { qr_set(q, i, 8, QR_FUNC); qr_set(q, 8, i, QR_FUNC); }
    }
    for (i = 0; i < 8; i++) {
        qr_set(q, s - 1 - i, 8, QR_FUNC);
        qr_set(q, 8, s - 1 - i, QR_FUNC);
    }
    if (q->version >= 7) {                          /* version-info reserve */
        for (i = 0; i < 6; i++)
            for (j = 0; j < 3; j++) {
                qr_set(q, s - 11 + j, i, QR_FUNC);
                qr_set(q, i, s - 11 + j, QR_FUNC);
            }
    }
}

/* ---- format and version information ---- */

/* The general form: reduce `v << deg` modulo `poly` (whose top bit is at
   position `deg`). */
static uint32_t qr_bch_n(uint32_t v, uint32_t poly, int deg)
{
    uint32_t r = v << deg;
    int i;

    for (i = 31; i >= deg; i--)
        if (r & (1u << (unsigned)i))
            r ^= poly << (unsigned)(i - deg);
    return (v << deg) | r;
}

static void qr_place_format(qr_grid_t *q, int ecl, int mask)
{
    /* ecl here is the FORMAT field, not my 0..3 index: L=01 M=00 Q=11 H=10. */
    static const int ECL_BITS[4] = { 1, 0, 3, 2 };
    uint32_t d = (uint32_t)((ECL_BITS[ecl] << 3) | mask);
    uint32_t bits = (qr_bch_n(d, 0x537u, 10) ^ 0x5412u) & 0x7FFFu;
    int i, s = q->size;

    for (i = 0; i <= 5; i++)
        qr_set(q, 8, i, QR_FUNC | ((bits >> i) & 1u));
    qr_set(q, 8, 7, QR_FUNC | ((bits >> 6) & 1u));
    qr_set(q, 8, 8, QR_FUNC | ((bits >> 7) & 1u));
    qr_set(q, 7, 8, QR_FUNC | ((bits >> 8) & 1u));
    for (i = 9; i <= 14; i++)
        qr_set(q, 14 - i, 8, QR_FUNC | ((bits >> i) & 1u));
    for (i = 0; i <= 7; i++)
        qr_set(q, s - 1 - i, 8, QR_FUNC | ((bits >> i) & 1u));
    for (i = 8; i <= 14; i++)
        qr_set(q, 8, s - 15 + i, QR_FUNC | ((bits >> i) & 1u));
    qr_set(q, 8, s - 8, QR_FUNC | QR_DARK);
}

static void qr_place_version(qr_grid_t *q)
{
    uint32_t bits;
    int i, j, s = q->size;

    if (q->version < 7)
        return;
    bits = qr_bch_n((uint32_t)q->version, 0x1F25u, 12) & 0x3FFFFu;
    for (i = 0; i < 6; i++) {
        for (j = 0; j < 3; j++) {
            unsigned b = (bits >> (unsigned)(i * 3 + j)) & 1u;
            qr_set(q, s - 11 + j, i, QR_FUNC | b);
            qr_set(q, i, s - 11 + j, QR_FUNC | b);
        }
    }
}

/* ---- data placement and masking ---- */

/* Two modules wide, bottom-up then top-down, skipping the vertical timing
   column. Every function module is stepped over, never overwritten. */
static void qr_place_data(qr_grid_t *q, const uint8_t *cw, int ncw)
{
    int s = q->size, x = s - 1, y = s - 1, up = 1;
    long bit = 0, total = (long)ncw * 8;

    while (x > 0) {
        if (x == 6)
            x--;                        /* the timing column is not data */
        for (;;) {
            int k;
            for (k = 0; k < 2; k++) {
                int cx = x - k;
                if (!(qr_get(q, cx, y) & QR_FUNC)) {
                    unsigned v = 0;
                    if (bit < total)
                        v = (cw[bit >> 3] >> (7 - (bit & 7))) & 1u;
                    qr_set(q, cx, y, v);   /* past the data: remainder bits, 0 */
                    bit++;
                }
            }
            y += up ? -1 : 1;
            if (y < 0 || y >= s) {
                y = up ? 0 : s - 1;
                up = !up;
                break;
            }
        }
        x -= 2;
    }
}

static int qr_mask_bit(int mask, int x, int y)
{
    switch (mask) {
    case 0: return ((y + x) % 2) == 0;
    case 1: return (y % 2) == 0;
    case 2: return (x % 3) == 0;
    case 3: return ((y + x) % 3) == 0;
    case 4: return (((y / 2) + (x / 3)) % 2) == 0;
    case 5: return ((y * x) % 2 + (y * x) % 3) == 0;
    case 6: return (((y * x) % 2 + (y * x) % 3) % 2) == 0;
    default: return (((y + x) % 2 + (y * x) % 3) % 2) == 0;
    }
}

static void qr_apply_mask(qr_grid_t *q, int mask)
{
    int x, y;

    for (y = 0; y < q->size; y++)
        for (x = 0; x < q->size; x++)
            if (!(qr_get(q, x, y) & QR_FUNC) && qr_mask_bit(mask, x, y))
                q->m[y * q->size + x] ^= QR_DARK;
}

/* The four penalty rules. A lower total is a symbol a scanner finds more
   easily; rule 3 in particular punishes anything resembling a finder. */
static long qr_penalty(const qr_grid_t *q)
{
    static const unsigned P1[11] = { 1,0,1,1,1,0,1,0,0,0,0 };
    static const unsigned P2[11] = { 0,0,0,0,1,0,1,1,1,0,1 };
    int s = q->size, x, y, i;
    long score = 0, dark = 0;

    for (y = 0; y < s; y++) {
        for (x = 0; x < s; x++) {
            unsigned c = qr_get(q, x, y) & QR_DARK;
            dark += c;
            if (x + 1 < s && y + 1 < s        /* rule 2: a 2x2 block */
                && c == (qr_get(q, x + 1, y) & QR_DARK)
                && c == (qr_get(q, x, y + 1) & QR_DARK)
                && c == (qr_get(q, x + 1, y + 1) & QR_DARK))
                score += 3;
        }
    }
    for (i = 0; i < 2; i++) {             /* i=0 rows, i=1 columns */
        int a, b;
        for (a = 0; a < s; a++) {
            int run = 1;
            unsigned prev = i ? qr_get(q, a, 0) : qr_get(q, 0, a);
            for (b = 1; b < s; b++) {
                unsigned c = (i ? qr_get(q, a, b) : qr_get(q, b, a)) & QR_DARK;
                if (c == (prev & QR_DARK)) {
                    run++;
                } else {
                    if (run >= 5) score += 3 + (run - 5);   /* rule 1 */
                    run = 1;
                    prev = c;
                }
            }
            if (run >= 5) score += 3 + (run - 5);
            for (b = 0; b + 11 <= s; b++) {                 /* rule 3 */
                int k, m1 = 1, m2 = 1;
                for (k = 0; k < 11; k++) {
                    unsigned c = (i ? qr_get(q, a, b + k) : qr_get(q, b + k, a))
                                 & QR_DARK;
                    if (c != P1[k]) m1 = 0;
                    if (c != P2[k]) m2 = 0;
                }
                if (m1) score += 40;
                if (m2) score += 40;
            }
        }
    }
    {   /* rule 4: distance of the dark ratio from half, in 5% steps */
        long total = (long)s * s;
        long pct = dark * 100 / total;
        long k = (pct >= 50 ? pct - 50 : 50 - pct) / 5;
        score += k * 10;
    }
    return score;
}

/* ---- the encoder ---- */

/* Byte mode: 4 mode bits + a character count (8 bits to version 9, 16 above)
   + the bytes themselves. Returns the smallest version that fits, or -1. */
static int qr_pick_version(size_t n, int ecl, int want)
{
    int v;

    for (v = want > 0 ? want : 1; v <= 40; v++) {
        const uint16_t *e = QR_ECC[(v - 1) * 4 + ecl];
        int cap = e[1] * e[2] + e[3] * e[4];        /* data codewords */
        int hdr = 4 + (v <= 9 ? 8 : 16);
        if ((long)cap * 8 >= (long)hdr + (long)n * 8)
            return v;
        if (want > 0)
            return -1;                  /* the caller pinned a version */
    }
    return -1;
}

/* Interleave: data codewords are read one per block in turn, then the EC
   codewords the same way. Concatenating the blocks instead would put every
   byte of a burst error inside one block, which is what the interleave exists
   to prevent. */
static int qr_encode_bits(const uint8_t *in, size_t n, int ver, int ecl,
                          uint8_t *out)
{
    const uint16_t *e = QR_ECC[(ver - 1) * 4 + ecl];
    int ecpb = e[0], nb1 = e[1], d1 = e[2], nb2 = e[3], d2 = e[4];
    int nblk = nb1 + nb2, ndata = nb1 * d1 + nb2 * d2;
    uint8_t buf[QR_MAX_CW], gen[64], ec[QR_MAX_CW];
    int cntbits = ver <= 9 ? 8 : 16;
    long bit = 0;
    int i, j, k, off, pos = 0, maxd = d1 > d2 ? d1 : d2;

    memset(buf, 0, sizeof buf);
#define PUT(val, len) do { int b_; for (b_ = (len) - 1; b_ >= 0; b_--) { \
        if (((val) >> b_) & 1) buf[bit >> 3] |= (uint8_t)(0x80u >> (bit & 7)); \
        bit++; } } while (0)
    PUT(4, 4);                                  /* byte mode */
    PUT((int)n, cntbits);
    for (i = 0; i < (int)n; i++)
        PUT(in[i], 8);
    for (i = 0; i < 4 && bit < (long)ndata * 8; i++)   /* terminator */
        PUT(0, 1);
    while (bit & 7)                             /* pad to a byte boundary */
        PUT(0, 1);
    for (i = 0; bit < (long)ndata * 8; i++)      /* alternating pad bytes */
        PUT(i & 1 ? 0x11 : 0xEC, 8);
#undef PUT

    qr_gen_poly(ecpb, gen);
    for (i = 0, off = 0; i < nblk; i++) {
        int dlen = i < nb1 ? d1 : d2;
        qr_rs(buf + off, dlen, ecpb, gen, ec + (size_t)i * ecpb);
        off += dlen;
    }
    for (k = 0; k < maxd; k++) {
        for (i = 0, off = 0; i < nblk; i++) {
            int dlen = i < nb1 ? d1 : d2;
            if (k < dlen)
                out[pos++] = buf[off + k];
            off += dlen;
        }
    }
    for (k = 0; k < ecpb; k++)
        for (i = 0; i < nblk; i++)
            out[pos++] = ec[(size_t)i * ecpb + k];
    (void)j;
    return pos;
}

/* Build the finished symbol. Every one of the eight masks is applied to a
   fresh copy and scored; the specification requires the lowest, and a scanner
   that cannot lock onto a badly-masked symbol simply fails to read it. */
static int qr_build(const uint8_t *in, size_t n, int ecl, int want_ver,
                    int want_mask, qr_grid_t *out)
{
    uint8_t cw[QR_MAX_CW];
    qr_grid_t base, best, cur;
    int ver = qr_pick_version(n, ecl, want_ver);
    int ncw, mask, bestmask = 0;
    long bestscore = 0;

    if (ver < 0)
        return -1;
    memset(&base, 0, sizeof base);
    base.version = ver;
    base.size = 17 + ver * 4;
    qr_functions(&base);
    ncw = qr_encode_bits(in, n, ver, ecl, cw);
    qr_place_data(&base, cw, ncw);
    qr_place_version(&base);

    for (mask = 0; mask < 8; mask++) {
        long sc;
        if (want_mask >= 0 && mask != want_mask)
            continue;
        cur = base;
        qr_apply_mask(&cur, mask);
        qr_place_format(&cur, ecl, mask);
        sc = qr_penalty(&cur);
        if (bestscore == 0 || sc < bestscore || (want_mask >= 0)) {
            bestscore = sc;
            bestmask = mask;
            best = cur;
        }
    }
    (void)bestmask;
    *out = best;
    return 0;
}

/* ---- JS entry points ---- */

static int qr_opts(JSContext *ctx, JSValueConst v, int *ecl, int *ver, int *mask)
{
    static const char *NAMES = "LMQH";
    JSValue x;

    *ecl = 1;                           /* M: the usual default */
    *ver = 0;
    *mask = -1;
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return 0;
    if (!JS_IsObject(v)) {
        JS_ThrowTypeError(ctx, "QREncode: options must be an object");
        return -1;
    }
    x = JS_GetPropertyStr(ctx, v, "ecc");
    if (JS_IsException(x))
        return -1;
    if (!JS_IsUndefined(x)) {
        const char *s = JS_ToCString(ctx, x);
        const char *p;
        JS_FreeValue(ctx, x);
        if (!s)
            return -1;
        p = (s[0] && !s[1]) ? strchr(NAMES, s[0] & ~0x20) : NULL;
        if (!p) {
            JS_ThrowRangeError(ctx, "QREncode: ecc is \"L\", \"M\", \"Q\" or "
                                    "\"H\", not \"%s\"", s);
            JS_FreeCString(ctx, s);
            return -1;
        }
        *ecl = (int)(p - NAMES);
        JS_FreeCString(ctx, s);
    } else {
        JS_FreeValue(ctx, x);
    }
    x = JS_GetPropertyStr(ctx, v, "version");
    if (JS_IsException(x))
        return -1;
    if (!JS_IsUndefined(x)) {
        int32_t n;
        int rc = JS_ToInt32(ctx, &n, x);
        JS_FreeValue(ctx, x);
        if (rc < 0)
            return -1;
        if (n < 1 || n > 40) {
            JS_ThrowRangeError(ctx, "QREncode: version is 1 to 40");
            return -1;
        }
        *ver = n;
    } else {
        JS_FreeValue(ctx, x);
    }
    x = JS_GetPropertyStr(ctx, v, "mask");
    if (JS_IsException(x))
        return -1;
    if (!JS_IsUndefined(x)) {
        int32_t n;
        int rc = JS_ToInt32(ctx, &n, x);
        JS_FreeValue(ctx, x);
        if (rc < 0)
            return -1;
        if (n < 0 || n > 7) {
            JS_ThrowRangeError(ctx, "QREncode: mask is 0 to 7");
            return -1;
        }
        *mask = n;
    } else {
        JS_FreeValue(ctx, x);
    }
    return 0;
}

/* magic 0 = QREncode -> {version, size, modules}, 1 = QRToString -> text */
static JSValue dyn_qr_encode(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int magic)
{
    qr_grid_t *q;
    const char *text;
    size_t n;
    int ecl, ver, mask, x, y;
    JSValue r;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "%s(text): text is required",
                                 magic ? "QRToString" : "QREncode");
    if (qr_opts(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &ecl, &ver, &mask) < 0)
        return JS_EXCEPTION;
    text = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!text)
        return JS_EXCEPTION;
    /* The grid is 31 KB at version 40 -- too much for the stack of a callback
       that may already be deep in the interpreter. */
    q = (qr_grid_t *)malloc(sizeof *q);
    if (!q) {
        JS_FreeCString(ctx, text);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (qr_build((const uint8_t *)text, n, ecl, ver, mask, q) < 0) {
        JS_FreeCString(ctx, text);
        free(q);
        return JS_ThrowRangeError(ctx, "QREncode: %zu bytes do not fit in %s "
            "at error level %c -- a QR symbol holds at most 2953 bytes", n,
            ver ? "that version" : "any version", "LMQH"[ecl]);
    }
    JS_FreeCString(ctx, text);

    if (magic == 1) {
        /* Two half-blocks per cell so the symbol is square in a terminal,
           and a two-module quiet zone, without which nothing will scan it. */
        size_t cap = (size_t)(q->size + 8) * (size_t)(q->size + 8) * 4 + 64;
        char *buf = (char *)malloc(cap);
        size_t off = 0;
        if (!buf) { free(q); return JS_ThrowOutOfMemory(ctx); }
        for (y = -2; y < q->size + 2; y += 2) {
            for (x = -2; x < q->size + 2; x++) {
                int top = (x >= 0 && x < q->size && y >= 0 && y < q->size)
                          && (qr_get(q, x, y) & QR_DARK);
                int bot = (x >= 0 && x < q->size && y + 1 >= 0
                           && y + 1 < q->size)
                          && (qr_get(q, x, y + 1) & QR_DARK);
                /* Length carried with the glyph: strlen of a three-byte
                   literal, once per module, is 31k calls on a version-40
                   symbol for an answer known at compile time. */
                static const char *const G[4] = { " ", "\xE2\x96\x84",
                                                  "\xE2\x96\x80", "\xE2\x96\x88" };
                static const uint8_t GL[4] = { 1, 3, 3, 3 };
                int gi = (top << 1) | bot;
                memcpy(buf + off, G[gi], GL[gi]);
                off += GL[gi];
            }
            buf[off++] = '\n';
        }
        r = JS_NewStringLen(ctx, buf, off);
        free(buf);
        free(q);
        return r;
    }
    {
        uint8_t *mods = (uint8_t *)malloc((size_t)q->size * q->size);
        if (!mods) { free(q); return JS_ThrowOutOfMemory(ctx); }
        for (y = 0; y < q->size; y++)
            for (x = 0; x < q->size; x++)
                mods[y * q->size + x] = (uint8_t)(qr_get(q, x, y) & QR_DARK);
        r = JS_NewObject(ctx);
        if (JS_IsException(r)) { free(mods); free(q); return r; }
        if (JS_DefinePropertyValueStr(ctx, r, "version",
                JS_NewInt32(ctx, q->version), JS_PROP_C_W_E) < 0
            || JS_DefinePropertyValueStr(ctx, r, "size",
                JS_NewInt32(ctx, q->size), JS_PROP_C_W_E) < 0
            || JS_DefinePropertyValueStr(ctx, r, "modules",
                dyn_enc_new_u8array(ctx, mods, (size_t)q->size * q->size),
                JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, r);
            free(mods); free(q);
            return JS_EXCEPTION;
        }
        free(mods);
        free(q);
        return r;
    }
}
