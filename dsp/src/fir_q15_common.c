#include "fir_q15_internal.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool byte_range(const void *pointer, size_t count,
                       uintptr_t *begin, uintptr_t *end)
{
    const size_t element_size = sizeof(int16_t);

    if (count > (SIZE_MAX / element_size)) {
        return false;
    }

    const size_t bytes = count * element_size;
    const uintptr_t address = (uintptr_t)pointer;
    if (bytes > (size_t)(UINTPTR_MAX - address)) {
        return false;
    }

    *begin = address;
    *end = address + (uintptr_t)bytes;
    return true;
}

static bool ranges_overlap(uintptr_t first_begin, uintptr_t first_end,
                           uintptr_t second_begin, uintptr_t second_end)
{
    return (first_begin < second_end) && (second_begin < first_end);
}

size_t aymos_fir_q15_valid_output_count(size_t sample_count,
                                        size_t tap_count)
{
    if ((tap_count == 0U) || (tap_count > AYMOS_FIR_Q15_MAX_TAPS) ||
        (sample_count < tap_count)) {
        return 0U;
    }

    return (sample_count - tap_count) + 1U;
}

aymos_fir_q15_status_t aymos_fir_q15_validate(
    const int16_t *samples, size_t sample_count,
    const int16_t *coefficients, size_t tap_count,
    int16_t *output, size_t output_capacity)
{
    uintptr_t samples_begin;
    uintptr_t samples_end;
    uintptr_t coefficients_begin;
    uintptr_t coefficients_end;
    uintptr_t output_begin;
    uintptr_t output_end;

    if ((samples == NULL) || (coefficients == NULL) || (output == NULL)) {
        return AYMOS_FIR_Q15_NULL_POINTER;
    }
    if ((tap_count == 0U) || (tap_count > AYMOS_FIR_Q15_MAX_TAPS)) {
        return AYMOS_FIR_Q15_INVALID_TAP_COUNT;
    }
    if (sample_count < tap_count) {
        return AYMOS_FIR_Q15_INPUT_TOO_SHORT;
    }

    const size_t output_count =
        aymos_fir_q15_valid_output_count(sample_count, tap_count);
    if (output_capacity < output_count) {
        return AYMOS_FIR_Q15_OUTPUT_TOO_SMALL;
    }
    if (!byte_range(samples, sample_count, &samples_begin, &samples_end) ||
        !byte_range(coefficients, tap_count, &coefficients_begin,
                    &coefficients_end) ||
        !byte_range(output, output_count, &output_begin, &output_end)) {
        return AYMOS_FIR_Q15_SIZE_OVERFLOW;
    }
    if (ranges_overlap(output_begin, output_end, samples_begin, samples_end) ||
        ranges_overlap(output_begin, output_end, coefficients_begin,
                       coefficients_end)) {
        return AYMOS_FIR_Q15_OVERLAPPING_OUTPUT;
    }

    return AYMOS_FIR_Q15_OK;
}

int32_t aymos_fir_q15_truncate(int64_t q30_value)
{
    /* C11 signed division truncates toward zero. */
    return (int32_t)(q30_value / INT64_C(32768));
}

int16_t aymos_fir_q15_saturate(int32_t q15_value)
{
    if (q15_value > INT16_MAX) {
        return INT16_MAX;
    }
    if (q15_value < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)q15_value;
}
