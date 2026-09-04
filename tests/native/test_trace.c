#include "aymos_trace.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>

static unsigned checks;

#define CHECK(expression)                                                       \
    do {                                                                        \
        ++checks;                                                               \
        if (!(expression)) {                                                    \
            fprintf(stderr, "trace check failed at %s:%d: %s\n", __FILE__,    \
                    __LINE__, #expression);                                     \
            return 1;                                                           \
        }                                                                       \
    } while (0)

int main(void)
{
    _Alignas(8) os_trace_record_t storage[5];
    os_trace_ring_t ring;
    os_trace_record_t record;
    os_trace_final_t final;

    CHECK(!os_trace_ring_init(NULL, storage, 3U));
    CHECK(!os_trace_ring_init(&ring, NULL, 3U));
    CHECK(!os_trace_ring_init(&ring, storage,
                              SIZE_MAX / sizeof(storage[0]) + 1U));
    CHECK(os_trace_ring_init(&ring, storage, 3U));
    CHECK(sizeof(os_trace_record_t) == 32U);
    CHECK(_Alignof(os_trace_record_t) >= 8U);
    for (unsigned event = (unsigned)OS_TRACE_KERNEL_START;
         event <= (unsigned)OS_TRACE_OVERFLOW; ++event) {
        CHECK(os_trace_event_valid((os_trace_event_t)event));
    }
    CHECK(!os_trace_event_valid((os_trace_event_t)0));
    CHECK(!os_trace_event_valid((os_trace_event_t)(OS_TRACE_OVERFLOW + 1)));
    CHECK(!os_trace_ring_push(&ring, 0U, (os_trace_event_t)0, 0U, 0U,
                              0U, 0U, 0U, 0U));
    CHECK(!os_trace_ring_push(&ring, 0U, OS_TRACE_KERNEL_START, 1U, 0U,
                              0U, 0U, 0U, 0U));

    CHECK(os_trace_ring_push(&ring, UINT64_C(7), OS_TRACE_TASK_CREATE, 0U,
                             1U, OS_TRACE_TASK_INVALID, 11U, 12U, 13U));
    CHECK(os_trace_ring_push(&ring, UINT64_C(8), OS_TRACE_TASK_RELEASE, 0U,
                             1U, 0U, 21U, 22U, 23U));
    CHECK(os_trace_ring_push(&ring, UINT64_C(9), OS_TRACE_TASK_START, 0U,
                             1U, 0U, 31U, 32U, 33U));
    CHECK(!os_trace_ring_push(&ring, UINT64_C(10), OS_TRACE_TASK_EXIT, 0U,
                              1U, 0U, 0U, 0U, 0U));
    CHECK(ring.attempted == 4U && ring.emitted == 3U && ring.dropped == 1U);
    CHECK(os_trace_ring_close(&ring, UINT64_C(10), &final));
    CHECK(final.attempted == 4U && final.emitted == 3U &&
          final.dropped == 1U && final.final_sequence == 3U);
    CHECK(final.final_tick == 10U);

    CHECK(os_trace_ring_pop(&ring, &record));
    CHECK(record.tick == 7U && record.sequence == 0U &&
          record.schema == OS_TRACE_SCHEMA_VERSION &&
          record.event == OS_TRACE_TASK_CREATE && record.task == 1U &&
          record.related == OS_TRACE_TASK_INVALID && record.value2 == 13U);
    CHECK(!os_trace_ring_report_overflow(&ring, UINT64_C(10)));
    CHECK(os_trace_ring_pop(&ring, &record) && record.sequence == 1U);
    CHECK(os_trace_ring_pop(&ring, &record) && record.sequence == 2U);
    CHECK(!os_trace_ring_pop(&ring, &record));
    CHECK(!os_trace_ring_push(&ring, 12U, OS_TRACE_TASK_EXIT, 0U, 1U, 0U,
                              0U, 0U, 0U));
    CHECK(!os_trace_ring_close(&ring, 12U, &final));

    CHECK(os_trace_ring_init(&ring, storage, 3U));
    CHECK(os_trace_ring_push(&ring, 1U, OS_TRACE_KERNEL_START, 0U, 0U, 0U,
                             0U, 0U, 0U));
    CHECK(os_trace_ring_push(&ring, 2U, OS_TRACE_TASK_CREATE, 0U, 1U, 0U,
                             0U, 0U, 0U));
    CHECK(os_trace_ring_push(&ring, 3U, OS_TRACE_TASK_RELEASE, 0U, 1U, 0U,
                             0U, 0U, 0U));
    CHECK(!os_trace_ring_push(&ring, 4U, OS_TRACE_TASK_START, 0U, 1U, 0U,
                              0U, 0U, 0U));
    const uint32_t full_attempted = ring.attempted;
    const uint32_t full_dropped = ring.dropped;
    CHECK(!os_trace_ring_report_overflow(&ring, 4U));
    CHECK(ring.attempted == full_attempted && ring.dropped == full_dropped);
    CHECK(os_trace_ring_pop(&ring, &record));
    CHECK(os_trace_ring_report_overflow(&ring, 5U));
    CHECK(!os_trace_ring_report_overflow(&ring, 5U));
    CHECK(os_trace_ring_close(&ring, 5U, &final));
    CHECK(final.attempted == 5U && final.emitted == 4U &&
          final.dropped == 1U && final.final_sequence == 4U);
    CHECK(os_trace_ring_pop(&ring, &record) && record.sequence == 1U);
    CHECK(os_trace_ring_pop(&ring, &record) && record.sequence == 2U);
    CHECK(os_trace_ring_pop(&ring, &record) && record.sequence == 4U &&
          record.event == OS_TRACE_OVERFLOW);

    CHECK(os_trace_ring_init(&ring, storage, 3U));
    ring.next_sequence = UINT32_MAX;
    ring.attempted = UINT32_MAX - 1U;
    CHECK(os_trace_ring_push(&ring, 1U, OS_TRACE_KERNEL_START, 0U, 0U, 0U,
                             0U, 0U, 0U));
    CHECK(ring.sequence_exhausted && ring.attempted == UINT32_MAX);
    CHECK(!os_trace_ring_push(&ring, 2U, OS_TRACE_KERNEL_START, 0U, 0U, 0U,
                              0U, 0U, 0U));
    CHECK(ring.dropped == 1U && ring.attempted == UINT32_MAX);

    const os_trace_input_t valid_batch[3] = {
        {OS_TRACE_TASK_CREATE, 0U, 1U, OS_TRACE_TASK_INVALID, 0U, 1U, 2U},
        {OS_TRACE_TASK_RELEASE, 0U, 1U, OS_TRACE_TASK_INVALID, 3U, 0U, 1U},
        {OS_TRACE_TASK_START, 0U, 1U, OS_TRACE_TASK_INVALID, 3U, 0U, 1U},
    };
    CHECK(os_trace_ring_init(&ring, storage, 5U));
    CHECK(!os_trace_ring_push_batch(&ring, 0U, valid_batch, 0U));
    CHECK(!os_trace_ring_push_batch(&ring, 0U, valid_batch,
                                    OS_TRACE_MAX_BATCH_RECORDS + 1U));
    CHECK(ring.attempted == 0U && ring.emitted == 0U && ring.dropped == 0U &&
          ring.count == 0U);
    os_trace_input_t invalid = valid_batch[0];
    invalid.event = (os_trace_event_t)0;
    CHECK(!os_trace_ring_push_batch(&ring, 0U, &invalid, 1U));
    invalid = valid_batch[0];
    invalid.flags = 1U;
    CHECK(!os_trace_ring_push_batch(&ring, 0U, &invalid, 1U));
    CHECK(ring.attempted == 0U && ring.next_sequence == 0U);

    CHECK(os_trace_ring_push_batch(&ring, 1U, valid_batch, 3U));
    CHECK(os_trace_ring_push(&ring, 2U, OS_TRACE_TASK_YIELD, 0U, 1U,
                             OS_TRACE_TASK_INVALID, 0U, 0U, 0U));
    CHECK(ring.count == 4U && ring.attempted == 4U && ring.emitted == 4U);
    CHECK(!os_trace_ring_push_batch(&ring, 3U, valid_batch, 2U));
    CHECK(ring.count == 4U && ring.attempted == 6U && ring.emitted == 4U &&
          ring.dropped == 2U && ring.next_sequence == 6U);
    CHECK(os_trace_ring_report_overflow(&ring, 3U));
    CHECK(ring.count == 5U && ring.attempted == 7U && ring.emitted == 5U &&
          ring.dropped == 2U && ring.next_sequence == 7U);
    const uint32_t reported_attempted = ring.attempted;
    CHECK(!os_trace_ring_report_overflow(&ring, 3U));
    CHECK(ring.attempted == reported_attempted);
    CHECK(os_trace_ring_close(&ring, 3U, &final));
    const uint32_t closed_attempted = ring.attempted;
    CHECK(!os_trace_ring_push(&ring, 4U, OS_TRACE_TASK_EXIT, 0U, 1U,
                              OS_TRACE_TASK_INVALID, 0U, 0U, 0U));
    CHECK(ring.attempted == closed_attempted);
    for (uint32_t sequence = 0U; sequence < 4U; ++sequence) {
        CHECK(os_trace_ring_pop(&ring, &record) &&
              record.sequence == sequence);
    }
    CHECK(os_trace_ring_pop(&ring, &record) && record.sequence == 6U &&
          record.event == OS_TRACE_OVERFLOW && record.value0 == 2U &&
          record.value1 == 6U && record.value2 == 4U);
    CHECK(!os_trace_ring_pop(&ring, &record));

    CHECK(os_trace_ring_init(&ring, storage, 5U));
    CHECK(os_trace_ring_push_batch(&ring, 1U, valid_batch, 3U));
    CHECK(os_trace_ring_pop(&ring, &record) && record.sequence == 0U);
    CHECK(os_trace_ring_pop(&ring, &record) && record.sequence == 1U);
    CHECK(os_trace_ring_push_batch(&ring, 2U, valid_batch, 3U));
    for (uint32_t sequence = 2U; sequence < 6U; ++sequence) {
        CHECK(os_trace_ring_pop(&ring, &record) &&
              record.sequence == sequence);
    }
    CHECK(!os_trace_ring_pop(&ring, &record));

    printf("trace native: %u checks passed\n", checks);
    return 0;
}
