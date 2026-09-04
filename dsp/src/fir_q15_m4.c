#include "fir_q15_internal.h"

#if !defined(__ARM_FEATURE_DSP) || (__ARM_FEATURE_DSP != 1)
#error "aymos_fir_q15_m4 requires Arm DSP extension support"
#endif

#include "cmsis_compiler.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

static int64_t signed_accumulator(uint64_t value)
{
    if (value <= (uint64_t)INT64_MAX) {
        return (int64_t)value;
    }
    return -INT64_C(1) - (int64_t)(UINT64_MAX - value);
}

aymos_fir_q15_status_t aymos_fir_q15_m4(
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
        uint64_t accumulator = 0U;
        size_t tap_index = 0U;
        for (; (tap_index + 1U) < tap_count; tap_index += 2U) {
            const uint32_t sample_pair = aymos_fir_q15_pack(
                samples[sample_index + tap_index],
                samples[sample_index + tap_index + 1U]);
            const uint32_t coefficient_pair = aymos_fir_q15_pack(
                coefficients[tap_index], coefficients[tap_index + 1U]);
            accumulator = __SMLALD(sample_pair, coefficient_pair, accumulator);
        }
        if (tap_index < tap_count) {
            const int64_t tail =
                (int64_t)samples[sample_index + tap_index] *
                (int64_t)coefficients[tap_index];
            accumulator += (uint64_t)tail;
        }

        const int32_t scaled =
            aymos_fir_q15_truncate(signed_accumulator(accumulator));
        output[sample_index] = (int16_t)__SSAT(scaled, 16U);
    }

    return AYMOS_FIR_Q15_OK;
}
