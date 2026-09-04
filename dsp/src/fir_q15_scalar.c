#include "fir_q15_internal.h"

#include <stddef.h>
#include <stdint.h>

aymos_fir_q15_status_t aymos_fir_q15_scalar(
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
        for (size_t tap_index = 0U; tap_index < tap_count; ++tap_index) {
            accumulator += (int64_t)samples[sample_index + tap_index] *
                           (int64_t)coefficients[tap_index];
        }
        output[sample_index] =
            aymos_fir_q15_saturate(aymos_fir_q15_truncate(accumulator));
    }

    return AYMOS_FIR_Q15_OK;
}
