#include "aymos_fir_q15.h"
#include "aymos_kernel.h"
#include "aymos_trace_runtime.h"
#include "board.h"
#include "stm32f4xx.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef AYMOS_SIGNAL_IMPL_M4
#define AYMOS_SIGNAL_IMPL_M4 0
#endif

#if AYMOS_SIGNAL_IMPL_M4 != 0 && AYMOS_SIGNAL_IMPL_M4 != 1
#error "AYMOS_SIGNAL_IMPL_M4 must be 0 or 1"
#endif

enum {
    TASK_SAMPLER = 1U,
    TASK_PROCESSOR = 2U,
    TASK_VERIFIER = 3U,
    FRAME_COUNT = 4U,
    SAMPLE_COUNT = 128U,
    TAP_COUNT = 16U,
    VALID_OUTPUT_COUNT = SAMPLE_COUNT - TAP_COUNT + 1U,
    TOTAL_OUTPUT_COUNT = FRAME_COUNT * VALID_OUTPUT_COUNT,
    WORK_UNITS = TOTAL_OUTPUT_COUNT * TAP_COUNT,
    PERIOD_TICKS = 6U,
    FINAL_TICK = 21U,
    RESULT_LINE_CAPACITY = 192U
};

#define SIGNAL_SEED UINT32_C(0x1A2B3C4D)
#define EXPECTED_INPUT_CRC UINT32_C(0x0CAF72FD)
#define EXPECTED_OUTPUT_CRC UINT32_C(0xAFC277C1)

static const int16_t coefficients[TAP_COUNT] = {
    -128, -256, -256, 256, 1280, 3072, 5120, 7296,
    7296, 5120, 3072, 1280, 256, -256, -256, -128,
};
static const uint32_t expected_frame_input_crc[FRAME_COUNT] = {
    UINT32_C(0x730ADC09), UINT32_C(0x516D1F6C),
    UINT32_C(0xB4AAC437), UINT32_C(0x241DBA95),
};
static const uint32_t expected_frame_output_crc[FRAME_COUNT] = {
    UINT32_C(0x4C2C86C2), UINT32_C(0x0D108262),
    UINT32_C(0x65642ED9), UINT32_C(0x469B7F1A),
};

static int16_t samples[SAMPLE_COUNT];
static int16_t outputs[VALID_OUTPUT_COUNT];
static uint32_t frame_input_crc[FRAME_COUNT];
static uint32_t frame_output_crc[FRAME_COUNT];
static volatile uint32_t sampled_generation;
static volatile uint32_t processed_generation;
static volatile uint32_t verified_generation;
static volatile bool sampler_finished;
static volatile bool processor_finished;
static volatile bool verifier_finished;
static uint32_t lfsr_state = SIGNAL_SEED;
static uint32_t aggregate_input_state = UINT32_MAX;
static uint32_t aggregate_output_state = UINT32_MAX;
static bool trace_flushed;

static void sampler_task(void *argument);
static void processor_task(void *argument);
static void verifier_task(void *argument);
static void wait_for_one_execution_tick(os_task_id_t task);
static uint32_t lfsr_next(void);
static int16_t signed_sample(uint16_t bits);
static uint32_t crc32_byte(uint32_t state, uint8_t value);
static uint32_t crc32_q15(uint32_t state, const int16_t *values,
                          size_t count);
static void require(bool condition, const char *reason);
static void write_line(const char *line);
static void write_result(void);
static bool append_text(char *line, size_t capacity, size_t *length,
                        const char *text);
static bool append_decimal(char *line, size_t capacity, size_t *length,
                           uint32_t value);
static bool append_hex32(char *line, size_t capacity, size_t *length,
                         uint32_t value);

int main(void)
{
    os_task_config_t sampler = os_task_config_default(sampler_task, NULL);
    os_task_config_t processor = os_task_config_default(processor_task, NULL);
    os_task_config_t verifier = os_task_config_default(verifier_task, NULL);
    os_task_id_t sampler_id = OS_TASK_ID_INVALID;
    os_task_id_t processor_id = OS_TASK_ID_INVALID;
    os_task_id_t verifier_id = OS_TASK_ID_INVALID;

    sampler.timing.kind = OS_TASK_PERIODIC;
    sampler.timing.period_ticks = PERIOD_TICKS;
    sampler.timing.relative_deadline_ticks = 3U;
    sampler.timing.execution_budget_ticks = 1U;
    sampler.timing.priority = 0U;

    processor.timing.kind = OS_TASK_PERIODIC;
    processor.timing.initial_release_delay_ticks = 1U;
    processor.timing.period_ticks = PERIOD_TICKS;
    processor.timing.relative_deadline_ticks = 3U;
    processor.timing.execution_budget_ticks = 1U;
    processor.timing.priority = 1U;

    verifier.timing.kind = OS_TASK_PERIODIC;
    verifier.timing.initial_release_delay_ticks = 2U;
    verifier.timing.period_ticks = PERIOD_TICKS;
    verifier.timing.relative_deadline_ticks = 3U;
    verifier.timing.execution_budget_ticks = 1U;
    verifier.timing.priority = 2U;

    require(board_init() == HAL_OK, "BOARD_INIT");
    write_line("AYMOS READY\r\n");
    write_line("TRACE BEGIN\r\n");
    require(os_kernel_init() == 1, "KERNEL_INIT");
    require(os_exception_configuration_valid(), "EXCEPTION_CONFIG");
    require(os_task_create(&sampler, &sampler_id) == 1 &&
                sampler_id == TASK_SAMPLER,
            "CREATE_SAMPLER");
    require(os_task_create(&processor, &processor_id) == 1 &&
                processor_id == TASK_PROCESSOR,
            "CREATE_PROCESSOR");
    require(os_task_create(&verifier, &verifier_id) == 1 &&
                verifier_id == TASK_VERIFIER,
            "CREATE_VERIFIER");
    os_kernel_start();
}

void board_lifecycle_idle_hook(void)
{
    if (trace_flushed || !sampler_finished || !processor_finished ||
        !verifier_finished) {
        return;
    }

    require(os_thread_uses_psp() && os_tick_count() == FINAL_TICK &&
                os_reclaim_count() == 3U &&
                os_last_reclaimed_task() == TASK_VERIFIER &&
                sampled_generation == FRAME_COUNT &&
                processed_generation == FRAME_COUNT &&
                verified_generation == FRAME_COUNT &&
                ~aggregate_input_state == EXPECTED_INPUT_CRC &&
                ~aggregate_output_state == EXPECTED_OUTPUT_CRC,
            "LAB_COMPLETE");
    trace_flushed = true;
    require(os_trace_finish() == 1, "TRACE_FLUSH");
    write_line("AYMOS TRACE DONE\r\n");
    write_result();
}

static void sampler_task(void *argument)
{
    require(argument == NULL && os_current_task() == TASK_SAMPLER &&
                os_thread_uses_psp(),
            "SAMPLER_START");
    for (uint32_t frame = 0U; frame < FRAME_COUNT; ++frame) {
        require(sampled_generation == frame && processed_generation == frame &&
                    verified_generation == frame,
                "SAMPLER_GENERATION");
        uint32_t frame_state = UINT32_MAX;
        for (size_t index = 0U; index < SAMPLE_COUNT; ++index) {
            samples[index] = signed_sample((uint16_t)lfsr_next());
            frame_state = crc32_q15(frame_state, &samples[index], 1U);
            aggregate_input_state =
                crc32_q15(aggregate_input_state, &samples[index], 1U);
        }
        frame_input_crc[frame] = ~frame_state;
        sampled_generation = frame + 1U;
        wait_for_one_execution_tick(TASK_SAMPLER);
        if ((frame + 1U) == FRAME_COUNT) {
            sampler_finished = true;
            return;
        }
        require(os_wait_next_period() == 1, "SAMPLER_WAIT");
    }
    require(false, "SAMPLER_LOOP");
}

static void processor_task(void *argument)
{
    require(argument == NULL && os_current_task() == TASK_PROCESSOR &&
                os_thread_uses_psp(),
            "PROCESSOR_START");
    for (uint32_t frame = 0U; frame < FRAME_COUNT; ++frame) {
        require(sampled_generation == (frame + 1U) &&
                    processed_generation == frame &&
                    verified_generation == frame,
                "PROCESSOR_GENERATION");
#if AYMOS_SIGNAL_IMPL_M4
        const aymos_fir_q15_status_t status = aymos_fir_q15_m4(
            samples, SAMPLE_COUNT, coefficients, TAP_COUNT, outputs,
            VALID_OUTPUT_COUNT);
#else
        const aymos_fir_q15_status_t status = aymos_fir_q15_scalar(
            samples, SAMPLE_COUNT, coefficients, TAP_COUNT, outputs,
            VALID_OUTPUT_COUNT);
#endif
        require(status == AYMOS_FIR_Q15_OK, "FIR");
        uint32_t frame_state = UINT32_MAX;
        frame_state = crc32_q15(frame_state, outputs, VALID_OUTPUT_COUNT);
        aggregate_output_state =
            crc32_q15(aggregate_output_state, outputs, VALID_OUTPUT_COUNT);
        frame_output_crc[frame] = ~frame_state;
        processed_generation = frame + 1U;
        wait_for_one_execution_tick(TASK_PROCESSOR);
        if ((frame + 1U) == FRAME_COUNT) {
            processor_finished = true;
            return;
        }
        require(os_wait_next_period() == 1, "PROCESSOR_WAIT");
    }
    require(false, "PROCESSOR_LOOP");
}

static void verifier_task(void *argument)
{
    require(argument == NULL && os_current_task() == TASK_VERIFIER &&
                os_thread_uses_psp(),
            "VERIFIER_START");
    for (uint32_t frame = 0U; frame < FRAME_COUNT; ++frame) {
        require(sampled_generation == (frame + 1U) &&
                    processed_generation == (frame + 1U) &&
                    verified_generation == frame &&
                    frame_input_crc[frame] == expected_frame_input_crc[frame] &&
                    frame_output_crc[frame] == expected_frame_output_crc[frame],
                "VERIFY_FRAME");
        verified_generation = frame + 1U;
        wait_for_one_execution_tick(TASK_VERIFIER);
        if ((frame + 1U) == FRAME_COUNT) {
            require(~aggregate_input_state == EXPECTED_INPUT_CRC &&
                        ~aggregate_output_state == EXPECTED_OUTPUT_CRC,
                    "VERIFY_AGGREGATE");
            verifier_finished = true;
            return;
        }
        require(os_wait_next_period() == 1, "VERIFIER_WAIT");
    }
    require(false, "VERIFIER_LOOP");
}

static void wait_for_one_execution_tick(os_task_id_t task)
{
    os_task_info_t info;

    for (;;) {
        const uint32_t saved_primask = __get_PRIMASK();
        __disable_irq();
        const int info_status = os_task_info(task, &info);
        require(__get_PRIMASK() == 1U && info_status == 1 &&
                    info.job_active && info.state == OS_TASK_RUNNING,
                "EXECUTION_INFO");

        /* Keep the sample and WFI atomic with respect to SysTick. If SysTick
           becomes pending after the sample, it wakes WFI. Restoring PRIMASK
           then lets the handler update the execution count. */
        if (info.job_execution_ticks == 0U) {
            __DSB();
            __WFI();
        }
        __set_PRIMASK(saved_primask);
        __ISB();
        if (info.job_execution_ticks != 0U) {
            break;
        }
    }
    require(info.job_execution_ticks == 1U &&
                info.deadline_miss_count == 0U,
            "EXECUTION_TICK");
}

static uint32_t lfsr_next(void)
{
    const uint32_t feedback = 0U - (lfsr_state & 1U);
    lfsr_state = (lfsr_state >> 1U) ^
                 (UINT32_C(0x80200003) & feedback);
    return lfsr_state;
}

static int16_t signed_sample(uint16_t bits)
{
    if (bits <= (uint16_t)INT16_MAX) {
        return (int16_t)bits;
    }
    return (int16_t)(-1 - (int32_t)(UINT16_MAX - bits));
}

static uint32_t crc32_byte(uint32_t state, uint8_t value)
{
    state ^= value;
    for (uint32_t bit = 0U; bit < 8U; ++bit) {
        const uint32_t mask = 0U - (state & 1U);
        state = (state >> 1U) ^ (UINT32_C(0xEDB88320) & mask);
    }
    return state;
}

static uint32_t crc32_q15(uint32_t state, const int16_t *values,
                          size_t count)
{
    for (size_t index = 0U; index < count; ++index) {
        const uint16_t bits = (uint16_t)values[index];
        state = crc32_byte(state, (uint8_t)bits);
        state = crc32_byte(state, (uint8_t)(bits >> 8U));
    }
    return state;
}

static void require(bool condition, const char *reason)
{
    if (!condition) {
        board_kernel_panic(reason);
    }
}

static void write_line(const char *line)
{
    size_t length = 0U;
    while (line[length] != '\0') {
        ++length;
    }
    require(board_uart_write(line, length) == HAL_OK, "UART");
}

static void write_result(void)
{
    char line[RESULT_LINE_CAPACITY];
    size_t length = 0U;

    require(append_text(line, sizeof(line), &length,
                        "AYMOS SIGNAL PASS IMPL=") &&
#if AYMOS_SIGNAL_IMPL_M4
                append_text(line, sizeof(line), &length, "m4") &&
#else
                append_text(line, sizeof(line), &length, "scalar") &&
#endif
                append_text(line, sizeof(line), &length, " FRAMES=") &&
                append_decimal(line, sizeof(line), &length, FRAME_COUNT) &&
                append_text(line, sizeof(line), &length, " SAMPLES=") &&
                append_decimal(line, sizeof(line), &length, SAMPLE_COUNT) &&
                append_text(line, sizeof(line), &length, " TAPS=") &&
                append_decimal(line, sizeof(line), &length, TAP_COUNT) &&
                append_text(line, sizeof(line), &length, " VALID=") &&
                append_decimal(line, sizeof(line), &length,
                               VALID_OUTPUT_COUNT) &&
                append_text(line, sizeof(line), &length, " OUTPUTS=") &&
                append_decimal(line, sizeof(line), &length,
                               TOTAL_OUTPUT_COUNT) &&
                append_text(line, sizeof(line), &length, " WORK=") &&
                append_decimal(line, sizeof(line), &length, WORK_UNITS) &&
                append_text(line, sizeof(line), &length, " SEED=0x") &&
                append_hex32(line, sizeof(line), &length, SIGNAL_SEED) &&
                append_text(line, sizeof(line), &length, " INPUT_CRC=0x") &&
                append_hex32(line, sizeof(line), &length,
                             EXPECTED_INPUT_CRC) &&
                append_text(line, sizeof(line), &length, " OUTPUT_CRC=0x") &&
                append_hex32(line, sizeof(line), &length,
                             EXPECTED_OUTPUT_CRC) &&
                append_text(line, sizeof(line), &length, "\r\n"),
            "RESULT_FORMAT");
    require(board_uart_write(line, length) == HAL_OK, "RESULT_UART");
}

static bool append_text(char *line, size_t capacity, size_t *length,
                        const char *text)
{
    size_t index = 0U;
    while (text[index] != '\0') {
        if (*length >= capacity) {
            return false;
        }
        line[*length] = text[index];
        ++*length;
        ++index;
    }
    return true;
}

static bool append_decimal(char *line, size_t capacity, size_t *length,
                           uint32_t value)
{
    char reversed[10];
    size_t count = 0U;
    do {
        reversed[count] = (char)('0' + (value % 10U));
        value /= 10U;
        ++count;
    } while (value != 0U);
    if (count > (capacity - *length)) {
        return false;
    }
    while (count > 0U) {
        --count;
        line[*length] = reversed[count];
        ++*length;
    }
    return true;
}

static bool append_hex32(char *line, size_t capacity, size_t *length,
                         uint32_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    if (8U > (capacity - *length)) {
        return false;
    }
    for (uint32_t shift = 32U; shift > 0U; shift -= 4U) {
        line[*length] = digits[(value >> (shift - 4U)) & 0xFU];
        ++*length;
    }
    return true;
}
