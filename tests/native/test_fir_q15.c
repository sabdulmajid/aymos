#include "aymos_fir_q15.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef aymos_fir_q15_status_t (*fir_function_t)(
    const int16_t *, size_t, const int16_t *, size_t, int16_t *, size_t);

static unsigned checks;

#define CHECK(expression)                                                       \
    do {                                                                        \
        ++checks;                                                               \
        if (!(expression)) {                                                    \
            fprintf(stderr, "FIR check failed at %s:%d: %s\n", __FILE__,     \
                    __LINE__, #expression);                                     \
            return 1;                                                           \
        }                                                                       \
    } while (0)

static int16_t reference_output(const int16_t *samples,
                                const int16_t *coefficients,
                                size_t tap_count)
{
    int64_t accumulator = 0;
    for (size_t tap = 0U; tap < tap_count; ++tap) {
        accumulator +=
            (int64_t)samples[tap] * (int64_t)coefficients[tap];
    }

    const int64_t scaled = accumulator / INT64_C(32768);
    if (scaled > INT16_MAX) {
        return INT16_MAX;
    }
    if (scaled < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)scaled;
}

static int check_invalid_inputs(fir_function_t fir)
{
    int16_t samples[4] = {1, 2, 3, 4};
    int16_t coefficients[3] = {1, 2, 3};
    int16_t output[4] = {1234, 1234, 1234, 1234};

    CHECK(fir(NULL, 4U, coefficients, 3U, output, 2U) ==
          AYMOS_FIR_Q15_NULL_POINTER);
    CHECK(fir(samples, 4U, NULL, 3U, output, 2U) ==
          AYMOS_FIR_Q15_NULL_POINTER);
    CHECK(fir(samples, 4U, coefficients, 3U, NULL, 2U) ==
          AYMOS_FIR_Q15_NULL_POINTER);
    CHECK(fir(samples, 4U, coefficients, 0U, output, 4U) ==
          AYMOS_FIR_Q15_INVALID_TAP_COUNT);
    CHECK(fir(samples, 4U, coefficients, AYMOS_FIR_Q15_MAX_TAPS + 1U,
              output, 4U) == AYMOS_FIR_Q15_INVALID_TAP_COUNT);
    CHECK(fir(samples, 2U, coefficients, 3U, output, 4U) ==
          AYMOS_FIR_Q15_INPUT_TOO_SHORT);
    CHECK(fir(samples, 4U, coefficients, 3U, output, 1U) ==
          AYMOS_FIR_Q15_OUTPUT_TOO_SMALL);
    CHECK(fir(samples, SIZE_MAX, coefficients, 3U, output, SIZE_MAX) ==
          AYMOS_FIR_Q15_SIZE_OVERFLOW);
    CHECK(fir(samples, 4U, coefficients, 3U, samples, 2U) ==
          AYMOS_FIR_Q15_OVERLAPPING_OUTPUT);
    CHECK(fir(samples, 3U, coefficients, 3U, coefficients, 1U) ==
          AYMOS_FIR_Q15_OVERLAPPING_OUTPUT);
    for (size_t index = 0U; index < 4U; ++index) {
        CHECK(output[index] == 1234);
    }

    return 0;
}

static int check_known_vectors(fir_function_t fir)
{
    const int16_t zero_samples[5] = {0, 0, 0, 0, 0};
    const int16_t zero_coefficients[3] = {12000, -23000, 32767};
    int16_t output[5] = {7, 7, 7, 7, 7};
    CHECK(fir(zero_samples, 5U, zero_coefficients, 3U, output, 3U) ==
          AYMOS_FIR_Q15_OK);
    CHECK(output[0] == 0 && output[1] == 0 && output[2] == 0);

    const int16_t order_samples[4] = {10000, 20000, 30000, -20000};
    const int16_t oldest_only[2] = {16384, 0};
    CHECK(fir(order_samples, 4U, oldest_only, 2U, output, 3U) ==
          AYMOS_FIR_Q15_OK);
    CHECK(output[0] == 5000 && output[1] == 10000 && output[2] == 15000);

    const int16_t impulse_samples[5] = {0, 16384, 0, 0, 0};
    const int16_t impulse_coefficients[3] = {8192, 16384, -8192};
    CHECK(fir(impulse_samples, 5U, impulse_coefficients, 3U, output, 3U) ==
          AYMOS_FIR_Q15_OK);
    CHECK(output[0] == 8192 && output[1] == 4096 && output[2] == 0);

    const int16_t negative_sample[1] = {-1};
    const int16_t half_scale[1] = {16384};
    CHECK(fir(negative_sample, 1U, half_scale, 1U, output, 1U) ==
          AYMOS_FIR_Q15_OK);
    CHECK(output[0] == 0);

    return 0;
}

static int check_saturation(fir_function_t fir)
{
    int16_t positive_samples[AYMOS_FIR_Q15_MAX_TAPS];
    int16_t negative_samples[AYMOS_FIR_Q15_MAX_TAPS];
    int16_t coefficients[AYMOS_FIR_Q15_MAX_TAPS];
    int16_t output[1] = {0};

    for (size_t index = 0U; index < AYMOS_FIR_Q15_MAX_TAPS; ++index) {
        positive_samples[index] = INT16_MAX;
        negative_samples[index] = INT16_MIN;
        coefficients[index] = INT16_MAX;
    }
    CHECK(fir(positive_samples, AYMOS_FIR_Q15_MAX_TAPS, coefficients,
              AYMOS_FIR_Q15_MAX_TAPS, output, 1U) == AYMOS_FIR_Q15_OK);
    CHECK(output[0] == INT16_MAX);
    CHECK(fir(negative_samples, AYMOS_FIR_Q15_MAX_TAPS, coefficients,
              AYMOS_FIR_Q15_MAX_TAPS, output, 1U) == AYMOS_FIR_Q15_OK);
    CHECK(output[0] == INT16_MIN);

    return 0;
}

static uint32_t next_random(uint32_t *state)
{
    *state = (*state * UINT32_C(1664525)) + UINT32_C(1013904223);
    return *state;
}

static int16_t signed_test_value(uint16_t value)
{
    if (value <= (uint16_t)INT16_MAX) {
        return (int16_t)value;
    }
    return (int16_t)(-1 - (int32_t)(UINT16_MAX - value));
}

static int check_deterministic_corpus(void)
{
    int16_t samples[96];
    int16_t coefficients[AYMOS_FIR_Q15_MAX_TAPS];
    int16_t scalar_output[96];
    int16_t packed_output[96];
    uint32_t state = UINT32_C(0x6d5a56e9);

    for (size_t index = 0U; index < 96U; ++index) {
        samples[index] =
            signed_test_value((uint16_t)(next_random(&state) >> 8U));
    }
    for (size_t index = 0U; index < AYMOS_FIR_Q15_MAX_TAPS; ++index) {
        coefficients[index] = signed_test_value(
            (uint16_t)(next_random(&state) >> 7U));
    }

    for (size_t tap_count = 1U; tap_count <= AYMOS_FIR_Q15_MAX_TAPS;
         ++tap_count) {
        const size_t sample_count = tap_count + 16U;
        const size_t output_count =
            aymos_fir_q15_valid_output_count(sample_count, tap_count);
        CHECK(output_count == 17U);
        CHECK(aymos_fir_q15_scalar(samples, sample_count, coefficients,
                                   tap_count, scalar_output, output_count) ==
              AYMOS_FIR_Q15_OK);
        CHECK(aymos_fir_q15_packed_portable(
                  samples, sample_count, coefficients, tap_count,
                  packed_output, output_count) == AYMOS_FIR_Q15_OK);
        for (size_t output_index = 0U; output_index < output_count;
             ++output_index) {
            const int16_t expected = reference_output(
                &samples[output_index], coefficients, tap_count);
            CHECK(scalar_output[output_index] == expected);
            CHECK(packed_output[output_index] == expected);
        }
    }

    return 0;
}

int main(void)
{
    CHECK(aymos_fir_q15_valid_output_count(4U, 3U) == 2U);
    CHECK(aymos_fir_q15_valid_output_count(3U, 3U) == 1U);
    CHECK(aymos_fir_q15_valid_output_count(2U, 3U) == 0U);
    CHECK(aymos_fir_q15_valid_output_count(4U, 0U) == 0U);
    CHECK(aymos_fir_q15_valid_output_count(
              65U, AYMOS_FIR_Q15_MAX_TAPS + 1U) == 0U);

    CHECK(check_invalid_inputs(aymos_fir_q15_scalar) == 0);
    CHECK(check_invalid_inputs(aymos_fir_q15_packed_portable) == 0);
    CHECK(check_known_vectors(aymos_fir_q15_scalar) == 0);
    CHECK(check_known_vectors(aymos_fir_q15_packed_portable) == 0);
    CHECK(check_saturation(aymos_fir_q15_scalar) == 0);
    CHECK(check_saturation(aymos_fir_q15_packed_portable) == 0);
    CHECK(check_deterministic_corpus() == 0);

    printf("FIR Q15 native tests passed: %u checks\n", checks);
    return 0;
}
