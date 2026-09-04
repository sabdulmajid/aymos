#ifndef AYMOS_FIR_Q15_INTERNAL_H
#define AYMOS_FIR_Q15_INTERNAL_H

#include "aymos_fir_q15.h"

#include <stddef.h>
#include <stdint.h>

aymos_fir_q15_status_t aymos_fir_q15_validate(
    const int16_t *samples, size_t sample_count,
    const int16_t *coefficients, size_t tap_count,
    int16_t *output, size_t output_capacity);

int32_t aymos_fir_q15_truncate(int64_t q30_value);
int16_t aymos_fir_q15_saturate(int32_t q15_value);

static inline uint32_t aymos_fir_q15_pack(int16_t low_lane,
                                          int16_t high_lane)
{
    return (uint32_t)(uint16_t)low_lane |
           ((uint32_t)(uint16_t)high_lane << 16U);
}

#endif
