// cybit.c - Cybit64 unit decoder, reimplemented from LvCore.dll v520.
//
// 2 kbps LPC vocoder: 16-bit header (frame count, tail samples) then 40-bit frames of
// pitch(7) LSF split-VQ(8+8+7) gain(6) voicing(4), LSB-first in 16-bit words. Each frame
// is 160 samples: mixed pulse/noise excitation, 4 interpolated LPC subframes, formant
// postfilter, DC blocker. Integer semantics follow the x86 original exactly.

#include "cybit.h"
#include "cybit_tables.h"

#include <string.h>

#define PULSE_CENTER  12
#define MAX_BLOCK     256
#define HIST          10

const CybitOptions CYBIT_V520 = { -1024, 0x5333, 0x6000, 0 };

typedef struct {
    int16_t  prevLsf[10];
    int16_t  prevGain;
    int16_t  prevPitch;
    int16_t  prevVoicing;
    int16_t  dcX;
    int32_t  dcY;
    int32_t  seed;
    int16_t  lsfMem[10];
    int16_t  pulseMem[8];
    uint16_t phase;
    int16_t  noiseMem[160];
    int16_t  synth[HIST + MAX_BLOCK];
    int16_t  tiltIn[1 + MAX_BLOCK];
    int16_t  postIn[HIST + MAX_BLOCK];
    int16_t  postOut[HIST + MAX_BLOCK];
} CybitState;

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
    uint16_t cur;
    int pos;
} Bits;

static int32_t w32(int64_t x)
{
    return (int32_t)(uint32_t)(uint64_t)x;
}

static int16_t sat16(int32_t x)
{
    return (int16_t)(x > 32767 ? 32767 : (x < -32768 ? -32768 : x));
}

static int16_t q12_round(int64_t acc)
{
    uint32_t v = ((uint32_t)w32(acc) + 0x800u) * 16u;
    return (int16_t)(v >> 16);
}

static int32_t mpy_32_16(int32_t a, int16_t b)
{
    int16_t hi = (int16_t)((uint32_t)a >> 16);
    int16_t lo = (int16_t)(w32((int64_t)a - (int64_t)hi * 0x10000) >> 1);
    int32_t t = ((b * (int32_t)lo) >> 14) & (int32_t)0xFFFFFFFE;
    return w32((int64_t)t + (int64_t)b * hi * 2);
}

static int norm_s16(int16_t s)
{
    uint32_t v = (uint32_t)(int32_t)s;
    int n = 0;
    if (s == 0) {
        return 0;
    }
    if (s == -1) {
        return 15;
    }
    if (s < 0) {
        v = ~v;
    }
    while ((int16_t)v < 0x4000) {
        v <<= 1;
        n++;
    }
    return n;
}

static uint16_t next_word(Bits *b)
{
    uint16_t w = 0;
    if (b->p + 1 < b->end) {
        w = (uint16_t)(b->p[0] | (b->p[1] << 8));
    }
    b->p += 2;
    return w;
}

static unsigned getbits(Bits *b, int n)
{
    int freeBits = 16 - b->pos;
    if (n <= freeBits) {
        unsigned v = ((unsigned)b->cur >> b->pos) & ((1u << n) - 1);
        b->pos += n;
        return v;
    }
    uint16_t old = b->cur;
    b->cur = next_word(b);
    int need = n - freeBits;
    unsigned v = (b->pos < 16 ? ((unsigned)old >> b->pos) : 0u) | (((unsigned)b->cur & ((1u << need) - 1)) << freeBits);
    b->pos = need;
    return v;
}

static void state_init(CybitState *st)
{
    memset(st, 0, sizeof *st);
    memcpy(st->prevLsf, CYBIT_LSF_MEAN, sizeof st->prevLsf);
    st->prevPitch = 0x3C0;
    st->seed = 0xC4B;
}

static void lsf_decode(CybitState *st, int i1, int i2, int i3, int16_t *out)
{
    int16_t resid[10];
    int k;
    for (k = 0; k < 3; ++k) {
        resid[k] = CYBIT_VQ1[i1 * 3 + k];
        resid[3 + k] = CYBIT_VQ2[i2 * 3 + k];
    }
    for (k = 0; k < 4; ++k) {
        resid[6 + k] = CYBIT_VQ3[i3 * 4 + k];
    }
    for (k = 0; k < 10; ++k) {
        int16_t p = (int16_t)((int16_t)((CYBIT_LSF_PRED[k] * (int32_t)st->lsfMem[k]) >> 15) + (resid[k] >> 3));
        out[k] = (int16_t)(CYBIT_LSF_MEAN[k] + p);
        st->lsfMem[k] = p;
    }
    int16_t floorVal = 0xCC;
    for (k = 0; k < 10; ++k) {
        if (out[k] < floorVal) {
            out[k] = floorVal;
        }
        floorVal = (int16_t)(out[k] + 0xCC);
    }
}

// params: [0] pitch lag Q4, [1..10] LSF, [11] gain, [12] voicing
static void unpack_frame(Bits *b, CybitState *st, int16_t *params, int voicingBoost)
{
    int16_t v = (int16_t)getbits(b, 7);
    params[0] = (v < 0x35) ? (int16_t)(v * 8 + 0x150) : (int16_t)((v - 5) * 16);
    int i1 = (int16_t)getbits(b, 8);
    int i2 = (int16_t)getbits(b, 8);
    int i3 = (int16_t)getbits(b, 7);
    lsf_decode(st, i1, i2, i3, params + 1);
    params[11] = CYBIT_GAIN[getbits(b, 6)];
    int voicing = (int16_t)getbits(b, 4);
    if (voicingBoost && voicing > 0) {
        voicing += voicingBoost;
        if (voicing > 15) {
            voicing = 15;
        }
    }
    params[12] = (int16_t)voicing;
}

static void gen_noise(CybitState *st, int16_t *out, int n)
{
    for (int k = 0; k < n; ++k) {
        int32_t seed = st->seed;
        int16_t s1 = (int16_t)seed;
        if (seed < 0x8000) {
            seed = 0x2BCD;
        } else {
            s1 = (int16_t)(s1 - 0x7FFF);
            seed = 0x55AB22;
        }
        int32_t x = w32((int64_t)seed + (int64_t)s1 * 0xAB);
        seed = x;
        int32_t y = mpy_32_16(x, 0x4EF2);
        x = w32((int64_t)seed + (int64_t)(int16_t)(y >> 15) * -0xCF85);
        seed = x;
        if (0xCF84 < x) {
            seed = w32((int64_t)x - 0xCF85);
        }
        st->seed = seed;
        out[k] = (int16_t)((int16_t)(seed >> 1) - 0x3394);
    }
}

static int32_t fir8(const int16_t *buf, int i, const int16_t *c)
{
    int64_t acc = (int64_t)buf[i] * c[0];
    for (int k = 1; k < 8; ++k) {
        acc += (int64_t)buf[i + k] * c[k];
    }
    return w32(acc);
}

static void shape_taps(const int16_t *table, int voicing, int16_t scale, int scaled, int16_t *c)
{
    int base = (int16_t)(voicing * 4);
    for (int k = 0; k < 4; ++k) {
        int16_t lo = table[base + k];
        int16_t hi = table[base + 3 - k];
        c[k] = scaled ? (int16_t)((lo * (int32_t)scale) >> 15) : lo;
        c[4 + k] = scaled ? (int16_t)((hi * (int32_t)scale) >> 15) : hi;
    }
}

static void noise_shape(int n, int16_t gain, int16_t voicing, int16_t *buf)
{
    int16_t c[8];
    shape_taps(CYBIT_NOISE_FIR, voicing, 0, 0, c);
    for (int i = 0; i < n; ++i) {
        int32_t acc = mpy_32_16(w32((int64_t)fir8(buf, i, c) * 2), 0x23);
        acc = mpy_32_16(acc, gain);
        buf[i] = (int16_t)(acc >> 7);
    }
}

static void noise_xfade(int n, int16_t *mem, const int16_t *buf, int16_t *out)
{
    int16_t wOld = 0x7FFF;
    int16_t wNew = 0;
    for (int i = 0; i < n; ++i) {
        int16_t t = (int16_t)((wOld * (int32_t)mem[i]) >> 15);
        int16_t wn = wNew;
        wOld = (int16_t)(wOld - 0xCC);
        wNew = (int16_t)(wNew + 0xCC);
        out[i] = (int16_t)((int16_t)((wn * (int32_t)buf[i]) >> 15) + t);
        mem[i] = buf[i];
    }
}

static int32_t pitch_interp(int16_t phase, int16_t end, int16_t prevPitch, int16_t curPitch)
{
    int16_t frac = (int16_t)(((int32_t)phase * 0x7FFF + (end >> 1)) / end);
    return ((int16_t)(0x7FFF - frac) * (int32_t)prevPitch + 0x4000 + frac * (int32_t)curPitch) >> 15;
}

static void pulse_excitation(CybitState *st, int n, const int16_t *prev, const int16_t *cur, int16_t *buf)
{
    int nn = n + 8;
    int16_t out[MAX_BLOCK];
    int16_t end = (int16_t)(n << 4);

    memset(buf, 0, sizeof(int16_t) * nn);
    memcpy(buf, st->pulseMem, sizeof st->pulseMem);

    int32_t uv12 = st->phase;
    uint16_t uv11 = (uint16_t)uv12;
    int32_t iv3 = cur[0];
    if ((int16_t)uv11 < end) {
        iv3 = pitch_interp((int16_t)uv11, end, prev[0], cur[0]);
    }
    while ((int16_t)uv11 < end + 0x80) {
        uv11 = (uint16_t)uv12;
        int kOff = -8;
        int16_t pos = (int16_t)(((int16_t)uv11 >> 4) - 2);
        for (int t = 0; t < 6; ++t) {
            if (pos >= 0 && pos < nn) {
                int16_t frac4 = (int16_t)(((int16_t)((uv11 + ((int16_t)uv11 >> 4) * -16) * 4) + 8) >> 4);
                int ampIdx = (int16_t)((int16_t)(((int16_t)iv3 + 8) >> 4) - 0x15);
                if (ampIdx < 0) {
                    ampIdx = 0;
                } else if (ampIdx > 101) {
                    ampIdx = 101;
                }
                int16_t shape = CYBIT_PULSE_SHAPE[PULSE_CENTER + kOff - frac4];
                buf[pos] = (int16_t)((shape * (int32_t)CYBIT_PULSE_AMP[ampIdx]) >> 15);
            }
            pos = (int16_t)(pos + 1);
            kOff += 4;
        }
        if (end <= (int16_t)uv11) {
            break;
        }
        uv12 = w32((int64_t)uv12 + iv3);
        uv11 = (uint16_t)uv12;
        iv3 = cur[0];
        if ((int16_t)uv11 < end) {
            iv3 = pitch_interp((int16_t)uv11, end, prev[0], cur[0]);
        }
    }
    st->phase = (uint16_t)(uv11 - end);
    memcpy(st->pulseMem, buf + n, sizeof st->pulseMem);

    for (int pass = 0; pass < 2; ++pass) {
        const int16_t *pp = pass == 0 ? prev : cur;
        int sh = norm_s16(pp[11]);
        int16_t c[8];
        shape_taps(CYBIT_PULSE_FIR, pp[12], (int16_t)(pp[11] << sh), 1, c);
        int16_t w = pass == 0 ? 0x7FFF : 0;
        int16_t step = pass == 0 ? -0xCC : 0xCC;
        for (int i = 0; i < n; ++i) {
            int32_t acc = mpy_32_16(w32((int64_t)fir8(buf, i, c) * 2), w);
            w = (int16_t)(w + step);
            int16_t v = (int16_t)((acc >> sh) >> 15);
            out[i] = pass == 0 ? v : (int16_t)(out[i] + v);
        }
    }
    memcpy(buf, out, sizeof(int16_t) * n);
}

static void get_lsp_pol(const int16_t *l, int32_t *f)
{
    f[0] = 0x1000000;
    f[1] = w32((int64_t)l[0] * -0x400);
    for (int i = 2; i < 6; ++i) {
        f[i] = f[i - 2];
        for (int j = i; j > 1; --j) {
            int32_t t = mpy_32_16(f[j - 1], l[i - 1]);
            f[j] = w32((int64_t)f[j] + f[j - 2] + (int64_t)t * -2);
        }
        f[1] = w32((int64_t)f[1] + (int64_t)l[i - 1] * -0x400);
    }
}

static void lsf_to_lpc(const int16_t *lsf, int16_t *a)
{
    int16_t even[5], odd[5];
    int32_t f1[6], f2[6];
    for (int k = 0; k < 10; ++k) {
        uint16_t u = (uint16_t)lsf[k];
        int idx = (int16_t)u >> 8;
        int16_t q = (int16_t)((int16_t)(((CYBIT_COS[idx + 1] - CYBIT_COS[idx]) * (int32_t)(u & 0xFF)) >> 8) + CYBIT_COS[idx]);
        if (k & 1) {
            odd[k / 2] = q;
        } else {
            even[k / 2] = q;
        }
    }
    get_lsp_pol(even, f1);
    get_lsp_pol(odd, f2);
    for (int i = 5; i > 0; --i) {
        f1[i] = w32((int64_t)f1[i] + f1[i - 1]);
        f2[i] = w32((int64_t)f2[i] - f2[i - 1]);
    }
    a[0] = 0x1000;
    for (int i = 1; i < 6; ++i) {
        int32_t t = w32((int64_t)f1[i] + f2[i]);
        a[i] = sat16((t >> 13) + ((t & 0x1000) ? 1 : 0));
        t = w32((int64_t)f1[i] - f2[i]);
        a[11 - i] = sat16((t >> 13) + ((t & 0x1000) ? 1 : 0));
    }
}

static void bw_expand(const int16_t *a, int16_t *out, int16_t gamma)
{
    int16_t w = 0x7FFF;
    out[0] = a[0];
    for (int i = 1; i < 11; ++i) {
        w = (int16_t)((gamma * (int32_t)w + 0x4000) >> 15);
        out[i] = (int16_t)((a[i] * (int32_t)w + 0x4000) >> 15);
    }
}

static void fir_filter(const int16_t *src, int16_t *dst, int n, const int16_t *c, int order)
{
    for (int j = 0; j < n; ++j) {
        int64_t acc = (int64_t)c[0] * src[j];
        for (int k = 1; k <= order; ++k) {
            acc += (int64_t)src[j - k] * c[k];
        }
        dst[j] = q12_round(acc);
    }
}

static void iir_filter(int16_t *buf, int n, const int16_t *a, int order)
{
    for (int j = 0; j < n; ++j) {
        int64_t acc = (int64_t)buf[j] * a[0];
        for (int k = 1; k <= order; ++k) {
            acc -= (int64_t)buf[j - k] * a[k];
        }
        buf[j] = q12_round(acc);
    }
}

// Same sequential order as LvCore, which matters when n < count.
static void hist_copy(int16_t *buf, int n, int count)
{
    for (int m = 0; m < count; ++m) {
        buf[-1 - m] = buf[n - 1 - m];
    }
}

static void dc_block(CybitState *st, int16_t *buf, int n)
{
    for (int j = 0; j < n; ++j) {
        int16_t xp = st->dcX;
        int16_t x = buf[j];
        int32_t y = st->dcY;
        int16_t hi = (int16_t)((uint32_t)y >> 16);
        int32_t lo = w32((int64_t)y - (int64_t)hi * 0x10000);
        st->dcX = x;
        y = w32(((int64_t)x - xp) * 0x10000 + ((int64_t)hi * 0x7EB7 + (((int16_t)(lo >> 1) * (int32_t)0x7EB7) >> 15)) * 2);
        st->dcY = y;
        buf[j] = (int16_t)((uint32_t)y >> 16);
    }
}

static int synth_frame(CybitState *st, int n, int mode, const int16_t *params, int frameIndex,
                       const CybitOptions *opt, int16_t *out)
{
    int16_t exc[MAX_BLOCK + 16];
    int16_t mix[MAX_BLOCK];
    int16_t prev[13];

    if (frameIndex == 0) {
        st->prevVoicing = params[12];
        memcpy(st->prevLsf, params + 1, sizeof st->prevLsf);
        st->prevPitch = params[0];
    }
    int L = mode == 4 ? 40 : n;
    int32_t step = mode == 4 ? 0x2000 : 0x6000;

    memset(exc, 0, sizeof exc);
    gen_noise(st, exc, n + 8);
    noise_shape(n, params[11], params[12], exc);
    noise_xfade(n, st->noiseMem, exc, mix);

    prev[0] = st->prevPitch;
    memcpy(prev + 1, st->prevLsf, sizeof st->prevLsf);
    prev[11] = st->prevGain;
    prev[12] = st->prevVoicing;
    pulse_excitation(st, n, prev, params, exc);
    for (int i = 0; i < n; ++i) {
        mix[i] = sat16((((int32_t)exc[i] + mix[i]) * 0x5EB8) >> 14);
    }

    int32_t wNew = 0x1000;
    int32_t wOld = 0x6FFF;
    int total = 0;
    int16_t tilt[2] = { 0x1000, opt->tilt };
    for (int sub = 0; sub < mode; ++sub) {
        int16_t lsfI[10], a[11], an[11], ad[11];
        for (int k = 0; k < 10; ++k) {
            lsfI[k] = (int16_t)((params[1 + k] * (int32_t)(int16_t)wNew + st->prevLsf[k] * (int32_t)(int16_t)wOld) >> 15);
        }
        wNew += step;
        wOld -= step;
        lsf_to_lpc(lsfI, a);

        int16_t *syn = st->synth + HIST;
        int16_t *tin = st->tiltIn + 1;
        int16_t *pin = st->postIn + HIST;
        int16_t *pout = st->postOut + HIST;

        memcpy(syn, mix + sub * L, sizeof(int16_t) * L);
        iir_filter(syn, L, a, 10);
        hist_copy(syn, L, HIST);

        memcpy(tin, syn, sizeof(int16_t) * L);
        fir_filter(tin, pin, L, tilt, 1);
        tin[-1] = tin[L - 1];

        bw_expand(a, an, opt->gamma_n);
        fir_filter(pin, pout, L, an, 10);
        hist_copy(pin, L, HIST);

        bw_expand(a, ad, opt->gamma_d);
        iir_filter(pout, L, ad, 10);
        hist_copy(pout, L, HIST);

        dc_block(st, pout, L);
        memcpy(out + total, pout, sizeof(int16_t) * L);
        total += L;
    }

    memcpy(st->prevLsf, params + 1, sizeof st->prevLsf);
    st->prevVoicing = params[12];
    st->prevGain = params[11];
    st->prevPitch = params[0];
    return total;
}

unsigned cybit_max_samples(unsigned inLen)
{
    return (inLen < 2 ? 0 : (inLen - 2) / 5) * 160 + 255;
}

int cybit_decode_unit(const uint8_t *in, unsigned inLen, int16_t *out, const CybitOptions *opt)
{
    CybitState st;
    Bits b;
    int16_t params[13];

    if (inLen < 4) {
        return 0;
    }
    state_init(&st);
    memset(params, 0, sizeof params);
    b.p = in + 2;
    b.end = in + inLen;
    b.cur = (uint16_t)(in[0] | (in[1] << 8));
    b.pos = 0;

    getbits(&b, 8);
    int tail = (int16_t)getbits(&b, 8);
    if (tail > MAX_BLOCK - 1) {
        tail = MAX_BLOCK - 1;
    }
    int frames = ((int)inLen - 2) / 5;
    int total = 0;
    for (int f = 0; f < frames; ++f) {
        unpack_frame(&b, &st, params, opt->voicing_boost);
        total += synth_frame(&st, 160, 4, params, f, opt, out + total);
    }
    params[11] = 0;
    total += synth_frame(&st, tail, 1, params, 1, opt, out + total);

    // FonixTTSuvCE.dll's unit dispatcher (FUN_001120ac) fades every decoded unit; LvCore v520 does not.
    if (opt->unit_fade && total > 0x80) {
        for (int i = 0; i < 64; ++i) {
            out[i] = (int16_t)((out[i] * i) / 63);
            out[total - 64 + i] = (int16_t)((out[total - 64 + i] * (63 - i)) / 63);
        }
    }
    if (opt->out_gain != 0 && opt->out_gain != 0x1000) {
        for (int i = 0; i < total; ++i) {
            out[i] = sat16((out[i] * (int32_t)opt->out_gain + 0x800) >> 12);
        }
    }
    return total;
}

const CybitOptions CYBIT_PPC = { -1024, 0x5333, 0x6000, 0, 1 };
