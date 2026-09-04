#include "fir_q15_internal.h"

#include <stddef.h>
#include <stdint.h>

static int32_t signed_lane(uint16_t value)
{
    if (value <= (uint16_t)INT16_MAX) {
        return (int32_t)value;
    }
    return -1 - (int32_t)(UINT16_MAX - value);
}

static int64_t portable_pair_mac(uint32_t sample_pair,
                                 uint32_t coefficient_pair)
{
    const int32_t sample_low = signed_lane((uint16_t)sample_pair);
    const int32_t sample_high = signed_lane((uint16_t)(sample_pair >> 16U));
    const int32_t coefficient_low =
        signed_lane((uint16_t)coefficient_pair);
    const int32_t coefficient_high =
        signed_lane((uint16_t)(coefficient_pair >> 16U));

    return ((int64_t)sample_low * (int64_t)coefficient_low) +
           ((int64_t)sample_high * (int64_t)coefficient_high);
}

aymos_fir_q15_status_t aymos_fir_q15_packed_portable(
    const int16_t *samples, size_t sample_count,
    const int16_t *coefficients, size_t tap_count,
    int16_t *output, size_t output_capacity)
{
    const aymos_fir_q15_status_t status =
        aymos_fir_q15_validate(samples, sample_count, coefficients, tap_count,
                               output, output_capacity);
    if (status != AYMOS_FIR_Q15_OK) {
        return status;
    }

    const size_t output_count =
        aymos_fir_q15_valid_output_count(sample_count, tap_count);
    for (size_t sample_index = 0U; sample_index < output_count; ++sample_index) {
        int64_t accumulator = 0;
        size_t tap_index = 0U;
        for (; (tap_index + 1U) < tap_count; tap_index += 2U) {
            const uint32_t sample_pair = aymos_fir_q15_pack(
                samples[sample_index + tap_index],
                samples[sample_index + tap_index + 1U]);
            const uint32_t coefficient_pair = aymos_fir_q15_pack(
                coefficients[tap_index], coefficients[tap_index + 1U]);
            accumulator += portable_pair_mac(sample_pair, coefficient_pair);
        }
        if (tap_index < tap_count) {
            accumulator += (int64_t)samples[sample_index + tap_index] *
                           (int64_t)coefficients[tap_index];
        }
        output[sample_index] =
            aymos_fir_q15_saturate(aymos_fir_q15_truncate(accumulator));
    }

    return AYMOS_FIR_Q15_OK;
}
