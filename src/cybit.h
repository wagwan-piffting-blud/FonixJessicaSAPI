// cybit.h - Cybit64 unit decoder, reimplemented from LvCore.dll v520 (FUN_10048dd0 and callees).
//
// With CYBIT_V520 options the output is bit-identical to LvCore's own decoder.
#ifndef CYBIT_H
#define CYBIT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// LvCore v520: CALL rel32 inside the unit-decode dispatcher, and the decoder it targets.
#define CYBIT_LVCORE_CALL_RVA     0xD846
#define CYBIT_LVCORE_DECODER_RVA  0x48DD0

typedef struct {
    int16_t tilt;           // Q12 coefficient of the 1st-order tilt FIR ahead of the postfilter
    int16_t gamma_n;        // Q15 postfilter numerator bandwidth expansion
    int16_t gamma_d;        // Q15 postfilter denominator bandwidth expansion
    int     voicing_boost;  // added to non-zero 4-bit voicing values, clamped to 15
    int     unit_fade;      // Pocket PC engine only: 64-sample linear fade in/out on units over 128 samples
    int16_t out_gain;       // Q12 saturating output gain; 0 means unity (not part of either original decoder)
} CybitOptions;

extern const CybitOptions CYBIT_V520;  // LvCore.dll v520 desktop decoder
extern const CybitOptions CYBIT_PPC;   // FonixTTSuvCE.dll (ARM) decoder: v520 plus unit fades

// Upper bound of samples cybit_decode_unit() can write for an input of inLen bytes.
unsigned cybit_max_samples(unsigned inLen);

// Decodes one sound bank unit to 8 kHz PCM; returns the number of samples written.
int cybit_decode_unit(const uint8_t *in, unsigned inLen, int16_t *out, const CybitOptions *opt);

#ifdef __cplusplus
}
#endif

#endif
