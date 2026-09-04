#ifndef AYMOS_TRACE_H
#define AYMOS_TRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    OS_TRACE_SCHEMA_VERSION = 1U,
    OS_TRACE_RECORD_SIZE = 32U,
    OS_TRACE_MAX_BATCH_RECORDS = 5U,
    OS_TRACE_TASK_INVALID = UINT16_MAX
};

typedef enum {
    OS_TRACE_KERNEL_START = 1,
    OS_TRACE_TASK_CREATE,
    OS_TRACE_TASK_RELEASE,
    OS_TRACE_TASK_SELECT,
    OS_TRACE_SELECT_CANDIDATE,
    OS_TRACE_TASK_START,
    OS_TRACE_TASK_PREEMPT,
    OS_TRACE_TASK_YIELD,
    OS_TRACE_TASK_SLEEP,
    OS_TRACE_TASK_WAKE,
    OS_TRACE_TASK_EXIT,
    OS_TRACE_CONTEXT_SWITCH,
    OS_TRACE_IDLE_START,
    OS_TRACE_IDLE_STOP,
    OS_TRACE_DEADLINE_MET,
    OS_TRACE_DEADLINE_MISS,
    OS_TRACE_ALLOC,
    OS_TRACE_FREE,
    OS_TRACE_TASK_WAIT_PERIOD,
    OS_TRACE_TASK_RELEASE_SKIPPED,
    OS_TRACE_OWNER_RELEASE,
    OS_TRACE_OVERFLOW
} os_trace_event_t;

typedef enum {
    OS_TRACE_SELECT_ONLY_READY = 1,
    OS_TRACE_SELECT_DEADLINE,
    OS_TRACE_SELECT_PRIORITY,
    OS_TRACE_SELECT_STABLE_TID,
    OS_TRACE_SELECT_IDLE,
    OS_TRACE_SELECT_EXCLUDED_FALLBACK
} os_trace_select_reason_t;

typedef enum {
    OS_TRACE_SELECT_DISPATCH = 1,
    OS_TRACE_SELECT_SYSTICK_PROBE,
    OS_TRACE_SELECT_RUNTIME_CREATE_PROBE
} os_trace_select_purpose_t;

typedef struct {
    uint64_t tick;
    uint32_t sequence;
    uint16_t schema;
    uint8_t event;
    uint8_t flags;
    uint16_t task;
    uint16_t related;
    uint32_t value0;
    uint32_t value1;
    uint32_t value2;
} os_trace_record_t;

typedef struct {
    os_trace_event_t event;
    uint8_t flags;
    uint16_t task;
    uint16_t related;
    uint32_t value0;
    uint32_t value1;
    uint32_t value2;
} os_trace_input_t;

typedef struct {
    os_trace_record_t *records;
    size_t capacity;
    size_t read_index;
    size_t write_index;
    size_t count;
    uint32_t next_sequence;
    uint32_t attempted;
    uint32_t emitted;
    uint32_t dropped;
    uint32_t reported_dropped;
    bool enabled;
    bool sequence_exhausted;
} os_trace_ring_t;

typedef struct {
    uint32_t attempted;
    uint32_t emitted;
    uint32_t dropped;
    uint32_t final_sequence;
    uint32_t flags;
    uint64_t final_tick;
} os_trace_final_t;

_Static_assert(sizeof(os_trace_record_t) == OS_TRACE_RECORD_SIZE,
               "trace record must remain 32 bytes");
_Static_assert(_Alignof(os_trace_record_t) >= 8U,
               "trace record must be naturally eight-byte aligned");
_Static_assert(offsetof(os_trace_record_t, tick) == 0U,
               "trace tick offset changed");
_Static_assert(offsetof(os_trace_record_t, sequence) == 8U,
               "trace sequence offset changed");
_Static_assert(offsetof(os_trace_record_t, schema) == 12U,
               "trace schema offset changed");
_Static_assert(offsetof(os_trace_record_t, event) == 14U,
               "trace event offset changed");
_Static_assert(offsetof(os_trace_record_t, flags) == 15U,
               "trace flags offset changed");
_Static_assert(offsetof(os_trace_record_t, task) == 16U,
               "trace task offset changed");
_Static_assert(offsetof(os_trace_record_t, related) == 18U,
               "trace related offset changed");
_Static_assert(offsetof(os_trace_record_t, value0) == 20U,
               "trace value0 offset changed");
_Static_assert(offsetof(os_trace_record_t, value1) == 24U,
               "trace value1 offset changed");
_Static_assert(offsetof(os_trace_record_t, value2) == 28U,
               "trace value2 offset changed");

bool os_trace_ring_init(os_trace_ring_t *ring, os_trace_record_t *storage,
                        size_t capacity);
bool os_trace_ring_push(os_trace_ring_t *ring, uint64_t tick,
                        os_trace_event_t event, uint8_t flags, uint16_t task,
                        uint16_t related, uint32_t value0, uint32_t value1,
                        uint32_t value2);
bool os_trace_ring_push_batch(os_trace_ring_t *ring, uint64_t tick,
                              const os_trace_input_t *inputs, size_t count);
bool os_trace_ring_pop(os_trace_ring_t *ring, os_trace_record_t *record);
bool os_trace_ring_report_overflow(os_trace_ring_t *ring, uint64_t tick);
bool os_trace_ring_close(os_trace_ring_t *ring, uint64_t final_tick,
                         os_trace_final_t *final);
bool os_trace_event_valid(os_trace_event_t event);

#endif
