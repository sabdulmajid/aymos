#include "aymos_trace_runtime.h"

#include "aymos_kernel.h"
#include "board.h"
#include "stm32f4xx.h"

#include <limits.h>
#include <stdint.h>

enum {
    TRACE_CAPACITY = 256U,
    TRACE_FRAME_HEADER_SIZE = 12U,
    TRACE_FRAME_CRC_SIZE = 4U,
    TRACE_RECORD_FRAME_SIZE = TRACE_FRAME_HEADER_SIZE + OS_TRACE_RECORD_SIZE +
                              TRACE_FRAME_CRC_SIZE,
    TRACE_FOOTER_PAYLOAD_SIZE = 28U,
    TRACE_FOOTER_FRAME_SIZE = TRACE_FRAME_HEADER_SIZE +
                              TRACE_FOOTER_PAYLOAD_SIZE + TRACE_FRAME_CRC_SIZE,
    TRACE_FRAME_VERSION = 1U,
    TRACE_FRAME_RECORD = 1U,
    TRACE_FRAME_FOOTER = 2U,
    TRACE_FINAL_FLAG_NONE = 0U
};

static _Alignas(8) os_trace_record_t trace_storage[TRACE_CAPACITY];
static os_trace_ring_t trace_ring;

_Static_assert(OS_TRACE_MAX_BATCH_RECORDS == OS_MAX_USER_TASKS + 1U,
               "selection batch must fit summary plus every user task");

static void put_u16(uint8_t *output, uint16_t value);
static void put_u32(uint8_t *output, uint32_t value);
static void put_u64(uint8_t *output, uint64_t value);
static uint32_t crc32(const uint8_t *data, size_t length);
static bool send_frame(uint8_t type, const uint8_t *payload,
                       uint16_t payload_length);
static void encode_record(const os_trace_record_t *record, uint8_t *payload);
static os_trace_select_reason_t selection_reason(
    const os_sched_task_t *tasks, size_t task_count, os_task_id_t selected,
    os_task_id_t incumbent, os_task_id_t excluded,
    os_trace_select_purpose_t purpose, size_t eligible_count);
static uint32_t ready_mask(const os_sched_task_t *tasks, size_t task_count);
static bool snapshot_outranks(const os_sched_task_t *candidate,
                              const os_sched_task_t *incumbent);

void os_trace_runtime_init(void)
{
    if (!os_trace_ring_init(&trace_ring, trace_storage, TRACE_CAPACITY)) {
        os_kernel_panic("TRACE_INIT");
    }
}

bool os_trace_emit(os_trace_event_t event, uint16_t task, uint16_t related,
                   uint32_t value0, uint32_t value1, uint32_t value2)
{
    const uint32_t saved_primask = __get_PRIMASK();
    __disable_irq();
    const uint64_t tick = os_monotonic_tick_count();
    (void)os_trace_ring_report_overflow(&trace_ring, tick);
    const bool emitted = os_trace_ring_push(
        &trace_ring, tick, event, 0U, task, related,
        value0, value1, value2);
    __set_PRIMASK(saved_primask);
    return emitted;
}

void os_trace_emit_selection(const os_sched_task_t *tasks, size_t task_count,
                             os_task_id_t selected, os_task_id_t incumbent,
                             os_task_id_t excluded,
                             os_trace_select_purpose_t purpose)
{
    if (tasks == NULL || task_count == 0U ||
        task_count > (size_t)OS_MAX_USER_TASKS + 1U ||
        selected >= task_count ||
        (purpose != OS_TRACE_SELECT_DISPATCH &&
         purpose != OS_TRACE_SELECT_SYSTICK_PROBE &&
         purpose != OS_TRACE_SELECT_RUNTIME_CREATE_PROBE)) {
        return;
    }
    const uint32_t saved_primask = __get_PRIMASK();
    __disable_irq();
    os_trace_input_t inputs[OS_MAX_USER_TASKS + 1U];
    size_t count = 0U;
    size_t eligible_count = 0U;
    for (size_t index = 1U; index < task_count; ++index) {
        const bool ready = tasks[index].state == OS_TASK_READY &&
                           tasks[index].job_active;
        const bool incumbent_running = index == incumbent &&
                                       tasks[index].state == OS_TASK_RUNNING &&
                                       tasks[index].job_active &&
                                       purpose != OS_TRACE_SELECT_DISPATCH;
        if (ready || incumbent_running) {
            ++count;
            if (index != excluded) {
                ++eligible_count;
            }
        }
    }
    const os_trace_select_reason_t reason =
        selection_reason(tasks, task_count, selected, incumbent, excluded,
                         purpose, eligible_count);
    const uint64_t tick = os_monotonic_tick_count();
    const uint32_t summary =
        (uint32_t)reason | ((uint32_t)purpose << 8U) |
        ((uint32_t)(excluded == OS_TASK_ID_INVALID ? UINT8_MAX : excluded)
         << 16U) |
        ((uint32_t)eligible_count << 24U);
    inputs[0] = (os_trace_input_t){
        .event = OS_TRACE_TASK_SELECT,
        .flags = 0U,
        .task = selected,
        .related = incumbent == OS_TASK_ID_INVALID
                       ? OS_TRACE_TASK_INVALID
                       : incumbent,
        .value0 = ready_mask(tasks, task_count),
        .value1 = summary,
        .value2 = (uint32_t)count,
    };
    size_t input_count = 1U;
    for (size_t index = 1U; index < task_count; ++index) {
        const bool ready = tasks[index].state == OS_TASK_READY &&
                           tasks[index].job_active;
        const bool incumbent_running = index == incumbent &&
                                       tasks[index].state == OS_TASK_RUNNING &&
                                       tasks[index].job_active &&
                                       purpose != OS_TRACE_SELECT_DISPATCH;
        if (!ready && !incumbent_running) {
            continue;
        }
        const uint64_t deadline = tasks[index].absolute_deadline_order;
        const uint32_t candidate_meta =
            tasks[index].priority | ((uint32_t)tasks[index].state << 8U) |
            ((index == excluded ? 1U : 0U) << 16U) |
            ((index == incumbent ? 1U : 0U) << 17U) |
            ((index != excluded ? 1U : 0U) << 18U);
        inputs[input_count] = (os_trace_input_t){
            .event = OS_TRACE_SELECT_CANDIDATE,
            .flags = 0U,
            .task = (uint16_t)index,
            .related = selected,
            .value0 = (uint32_t)deadline,
            .value1 = (uint32_t)(deadline >> 32U),
            .value2 = candidate_meta,
        };
        ++input_count;
    }
    (void)os_trace_ring_report_overflow(&trace_ring, tick);
    (void)os_trace_ring_push_batch(&trace_ring, tick, inputs, input_count);
    __set_PRIMASK(saved_primask);
}

int os_trace_finish(void)
{
    if (__get_IPSR() != 0U || os_current_task() != OS_TASK_ID_IDLE) {
        return 0;
    }

    os_trace_record_t record;
    os_trace_final_t final;
    const uint32_t close_primask = __get_PRIMASK();
    __disable_irq();
    const bool closed = os_trace_ring_close(
        &trace_ring, os_monotonic_tick_count(), &final);
    __set_PRIMASK(close_primask);
    if (!closed) {
        return 0;
    }

    for (;;) {
        const uint32_t pop_primask = __get_PRIMASK();
        __disable_irq();
        const bool available = os_trace_ring_pop(&trace_ring, &record);
        __set_PRIMASK(pop_primask);
        if (!available) {
            break;
        }
        uint8_t payload[OS_TRACE_RECORD_SIZE];
        encode_record(&record, payload);
        if (!send_frame(TRACE_FRAME_RECORD, payload,
                        (uint16_t)sizeof(payload))) {
            return 0;
        }
    }

    uint8_t payload[TRACE_FOOTER_PAYLOAD_SIZE];
    put_u32(&payload[0], final.attempted);
    put_u32(&payload[4], final.emitted);
    put_u32(&payload[8], final.dropped);
    put_u32(&payload[12], final.final_sequence);
    put_u32(&payload[16], final.flags | TRACE_FINAL_FLAG_NONE);
    put_u64(&payload[20], final.final_tick);
    return send_frame(TRACE_FRAME_FOOTER, payload,
                      (uint16_t)sizeof(payload))
               ? 1
               : 0;
}

static void put_u16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8U);
}

static void put_u32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8U);
    output[2] = (uint8_t)(value >> 16U);
    output[3] = (uint8_t)(value >> 24U);
}

static void put_u64(uint8_t *output, uint64_t value)
{
    put_u32(output, (uint32_t)value);
    put_u32(output + 4U, (uint32_t)(value >> 32U));
}

static uint32_t crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t index = 0U; index < length; ++index) {
        crc ^= data[index];
        for (uint32_t bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (UINT32_C(0xEDB88320) & mask);
        }
    }
    return ~crc;
}

static bool send_frame(uint8_t type, const uint8_t *payload,
                       uint16_t payload_length)
{
    uint8_t frame[TRACE_RECORD_FRAME_SIZE];
    if (payload == NULL ||
        (payload_length != OS_TRACE_RECORD_SIZE &&
         payload_length != TRACE_FOOTER_PAYLOAD_SIZE)) {
        return false;
    }
    frame[0] = 'A';
    frame[1] = 'Y';
    frame[2] = 'M';
    frame[3] = 'T';
    frame[4] = TRACE_FRAME_VERSION;
    frame[5] = type;
    put_u16(&frame[6], OS_TRACE_SCHEMA_VERSION);
    put_u16(&frame[8], payload_length);
    put_u16(&frame[10], 0U);
    for (uint16_t index = 0U; index < payload_length; ++index) {
        frame[TRACE_FRAME_HEADER_SIZE + index] = payload[index];
    }
    const size_t crc_offset = TRACE_FRAME_HEADER_SIZE + payload_length;
    put_u32(&frame[crc_offset],
            crc32(&frame[4], 8U + payload_length));
    return board_uart_write((const char *)frame,
                            crc_offset + TRACE_FRAME_CRC_SIZE) == HAL_OK;
}

static void encode_record(const os_trace_record_t *record, uint8_t *payload)
{
    put_u64(&payload[0], record->tick);
    put_u32(&payload[8], record->sequence);
    put_u16(&payload[12], record->schema);
    payload[14] = record->event;
    payload[15] = record->flags;
    put_u16(&payload[16], record->task);
    put_u16(&payload[18], record->related);
    put_u32(&payload[20], record->value0);
    put_u32(&payload[24], record->value1);
    put_u32(&payload[28], record->value2);
}

static os_trace_select_reason_t selection_reason(
    const os_sched_task_t *tasks, size_t task_count, os_task_id_t selected,
    os_task_id_t incumbent, os_task_id_t excluded,
    os_trace_select_purpose_t purpose, size_t eligible_count)
{
    if (selected == OS_TASK_ID_IDLE) {
        return OS_TRACE_SELECT_IDLE;
    }
    if (selected == excluded && eligible_count == 0U) {
        return OS_TRACE_SELECT_EXCLUDED_FALLBACK;
    }
    if (eligible_count <= 1U) {
        return OS_TRACE_SELECT_ONLY_READY;
    }
    os_task_id_t runner_up = OS_TASK_ID_INVALID;
    for (size_t index = 1U; index < task_count; ++index) {
        const bool ready = tasks[index].state == OS_TASK_READY &&
                           tasks[index].job_active;
        const bool incumbent_running = index == incumbent &&
                                       tasks[index].state == OS_TASK_RUNNING &&
                                       tasks[index].job_active &&
                                       purpose != OS_TRACE_SELECT_DISPATCH;
        if (index == selected || index == excluded ||
            (!ready && !incumbent_running)) {
            continue;
        }
        if (runner_up == OS_TASK_ID_INVALID ||
            snapshot_outranks(&tasks[index], &tasks[runner_up])) {
            runner_up = (os_task_id_t)index;
        }
    }
    if (runner_up == OS_TASK_ID_INVALID ||
        tasks[selected].absolute_deadline_order !=
            tasks[runner_up].absolute_deadline_order) {
        return OS_TRACE_SELECT_DEADLINE;
    }
    if (tasks[selected].priority != tasks[runner_up].priority) {
        return OS_TRACE_SELECT_PRIORITY;
    }
    return OS_TRACE_SELECT_STABLE_TID;
}

static bool snapshot_outranks(const os_sched_task_t *candidate,
                              const os_sched_task_t *incumbent)
{
    if (candidate->absolute_deadline_order !=
        incumbent->absolute_deadline_order) {
        return candidate->absolute_deadline_order <
               incumbent->absolute_deadline_order;
    }
    if (candidate->priority != incumbent->priority) {
        return candidate->priority < incumbent->priority;
    }
    return candidate->id < incumbent->id;
}

static uint32_t ready_mask(const os_sched_task_t *tasks, size_t task_count)
{
    uint32_t mask = 0U;
    for (size_t index = 1U; index < task_count; ++index) {
        if (tasks[index].state == OS_TASK_READY && tasks[index].job_active) {
            mask |= UINT32_C(1) << index;
        }
    }
    return mask;
}
