#ifndef AYMOS_TRACE_RUNTIME_H
#define AYMOS_TRACE_RUNTIME_H

#include "aymos_scheduler.h"
#include "aymos_trace.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(AYMOS_TRACE_ENABLED)
void os_trace_runtime_init(void);
bool os_trace_emit(os_trace_event_t event, uint16_t task, uint16_t related,
                   uint32_t value0, uint32_t value1, uint32_t value2);
void os_trace_emit_selection(const os_sched_task_t *tasks, size_t task_count,
                             os_task_id_t selected, os_task_id_t incumbent,
                             os_task_id_t excluded,
                             os_trace_select_purpose_t purpose);
int os_trace_finish(void);
#else
static inline void os_trace_runtime_init(void) {}
static inline bool os_trace_emit(os_trace_event_t event, uint16_t task,
                                 uint16_t related, uint32_t value0,
                                 uint32_t value1, uint32_t value2)
{
    (void)event;
    (void)task;
    (void)related;
    (void)value0;
    (void)value1;
    (void)value2;
    return false;
}
static inline void os_trace_emit_selection(const os_sched_task_t *tasks,
                                            size_t task_count,
                                            os_task_id_t selected,
                                            os_task_id_t incumbent,
                                            os_task_id_t excluded,
                                            os_trace_select_purpose_t purpose)
{
    (void)tasks;
    (void)task_count;
    (void)selected;
    (void)incumbent;
    (void)excluded;
    (void)purpose;
}
#endif

#endif
