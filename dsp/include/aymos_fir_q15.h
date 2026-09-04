#ifndef AYMOS_FIR_Q15_H
#define AYMOS_FIR_Q15_H

#include <stddef.h>
#include <stdint.h>

#define AYMOS_FIR_Q15_MAX_TAPS 64U

typedef enum {
    AYMOS_FIR_Q15_OK = 0,
    AYMOS_FIR_Q15_NULL_POINTER,
    AYMOS_FIR_Q15_INVALID_TAP_COUNT,
    AYMOS_FIR_Q15_INPUT_TOO_SHORT,
    AYMOS_FIR_Q15_OUTPUT_TOO_SMALL,
    AYMOS_FIR_Q15_SIZE_OVERFLOW,
    AYMOS_FIR_Q15_OVERLAPPING_OUTPUT
} aymos_fir_q15_status_t;

/* Return the number of outputs that contain a complete input window. */
size_t aymos_fir_q15_valid_output_count(size_t sample_count,
                                        size_t tap_count);

/* Portable reference implementation. */
aymos_fir_q15_status_t aymos_fir_q15_scalar(
    const int16_t *samples, size_t sample_count,
    const int16_t *coefficients, size_t tap_count,
    int16_t *output, size_t output_capacity);

/* Portable two-lane loop. This function does not execute Arm DSP instructions. */
aymos_fir_q15_status_t aymos_fir_q15_packed_portable(
    const int16_t *samples, size_t sample_count,
    const int16_t *coefficients, size_t tap_count,
    int16_t *output, size_t output_capacity);

/* Cortex-M4 implementation. Its translation unit requires Arm DSP support. */
aymos_fir_q15_status_t aymos_fir_q15_m4(
    const int16_t *samples, size_t sample_count,
    const int16_t *coefficients, size_t tap_count,
    int16_t *output, size_t output_capacity);

#endif
