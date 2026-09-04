#include "aymos_trace.h"

#include <limits.h>
#include <stdatomic.h>
#include <string.h>

enum { OS_TRACE_FINAL_SEQUENCE_NONE = UINT32_MAX };

static void increment_saturating(uint32_t *value)
{
    if (*value != UINT32_MAX) {
        ++*value;
    }
}

bool os_trace_event_valid(os_trace_event_t event)
{
    return event >= OS_TRACE_KERNEL_START && event <= OS_TRACE_OVERFLOW;
}

bool os_trace_ring_init(os_trace_ring_t *ring, os_trace_record_t *storage,
                        size_t capacity)
{
    if (ring == NULL || storage == NULL || capacity == 0U ||
        capacity > (size_t)UINT32_MAX ||
        capacity > SIZE_MAX / sizeof(*storage) ||
        ((uintptr_t)storage % _Alignof(os_trace_record_t)) != 0U) {
        return false;
    }
    memset(ring, 0, sizeof(*ring));
    memset(storage, 0, capacity * sizeof(*storage));
    ring->records = storage;
    ring->capacity = capacity;
    ring->enabled = true;
    return true;
}

bool os_trace_ring_push(os_trace_ring_t *ring, uint64_t tick,
                        os_trace_event_t event, uint8_t flags, uint16_t task,
                        uint16_t related, uint32_t value0, uint32_t value1,
                        uint32_t value2)
{
    const os_trace_input_t input = {
        .event = event,
        .flags = flags,
        .task = task,
        .related = related,
        .value0 = value0,
        .value1 = value1,
        .value2 = value2,
    };
    return os_trace_ring_push_batch(ring, tick, &input, 1U);
}

bool os_trace_ring_push_batch(os_trace_ring_t *ring, uint64_t tick,
                              const os_trace_input_t *inputs, size_t count)
{
    if (ring == NULL || ring->records == NULL || ring->capacity == 0U ||
        !ring->enabled || inputs == NULL || count == 0U ||
        count > ring->capacity || count > OS_TRACE_MAX_BATCH_RECORDS) {
        return false;
    }
    for (size_t index = 0U; index < count; ++index) {
        if (!os_trace_event_valid(inputs[index].event) ||
            inputs[index].flags != 0U) {
            return false;
        }
    }

    const uint32_t count32 = (uint32_t)count;
    if (ring->sequence_exhausted ||
        ring->next_sequence > UINT32_MAX - (count32 - 1U)) {
        ring->sequence_exhausted = true;
        for (size_t index = 0U; index < count; ++index) {
            increment_saturating(&ring->attempted);
            increment_saturating(&ring->dropped);
        }
        return false;
    }
    const uint32_t first_sequence = ring->next_sequence;
    for (size_t index = 0U; index < count; ++index) {
        increment_saturating(&ring->attempted);
    }
    ring->next_sequence += count32;
    if (ring->next_sequence == 0U) {
        ring->sequence_exhausted = true;
    }
    if (count > ring->capacity - ring->count) {
        for (size_t index = 0U; index < count; ++index) {
            increment_saturating(&ring->dropped);
        }
        return false;
    }

    size_t write = ring->write_index;
    for (size_t index = 0U; index < count; ++index) {
        os_trace_record_t *const record = &ring->records[write];
        record->tick = tick;
        record->sequence = first_sequence + (uint32_t)index;
        record->schema = OS_TRACE_SCHEMA_VERSION;
        record->event = (uint8_t)inputs[index].event;
        record->flags = inputs[index].flags;
        record->task = inputs[index].task;
        record->related = inputs[index].related;
        record->value0 = inputs[index].value0;
        record->value1 = inputs[index].value1;
        record->value2 = inputs[index].value2;
        write = (write + 1U) % ring->capacity;
    }
    atomic_thread_fence(memory_order_release);
    ring->write_index = write;
    ring->count += count;
    for (size_t index = 0U; index < count; ++index) {
        increment_saturating(&ring->emitted);
    }
    return true;
}

bool os_trace_ring_pop(os_trace_ring_t *ring, os_trace_record_t *record)
{
    if (ring == NULL || record == NULL || ring->records == NULL ||
        ring->count == 0U) {
        return false;
    }
    atomic_thread_fence(memory_order_acquire);
    *record = ring->records[ring->read_index];
    ring->read_index = (ring->read_index + 1U) % ring->capacity;
    --ring->count;
    return true;
}

bool os_trace_ring_report_overflow(os_trace_ring_t *ring, uint64_t tick)
{
    if (ring == NULL || ring->records == NULL || !ring->enabled ||
        ring->capacity == 0U || ring->count >= ring->capacity ||
        ring->sequence_exhausted ||
        ring->dropped == ring->reported_dropped) {
        return false;
    }
    const uint32_t dropped = ring->dropped;
    const uint32_t attempted = ring->attempted;
    const uint32_t emitted = ring->emitted;
    if (!os_trace_ring_push(ring, tick, OS_TRACE_OVERFLOW, 0U,
                            OS_TRACE_TASK_INVALID, OS_TRACE_TASK_INVALID,
                            dropped, attempted, emitted)) {
        return false;
    }
    ring->reported_dropped = dropped;
    return true;
}

bool os_trace_ring_close(os_trace_ring_t *ring, uint64_t final_tick,
                         os_trace_final_t *final)
{
    if (ring == NULL || final == NULL || ring->records == NULL ||
        !ring->enabled) {
        return false;
    }
    ring->enabled = false;
    final->attempted = ring->attempted;
    final->emitted = ring->emitted;
    final->dropped = ring->dropped;
    final->final_sequence = ring->attempted == 0U
                                ? OS_TRACE_FINAL_SEQUENCE_NONE
                                : (ring->sequence_exhausted
                                       ? UINT32_MAX
                                       : ring->next_sequence - 1U);
    final->flags = ring->sequence_exhausted ? 1U : 0U;
    final->final_tick = final_tick;
    return true;
}
