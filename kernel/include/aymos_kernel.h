#ifndef AYMOS_KERNEL_H
#define AYMOS_KERNEL_H

#include "aymos_allocator.h"
#include "aymos_scheduler.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    OS_MIN_STACK_SIZE = 256U,
    OS_MAX_STACK_SIZE = 1024U,
    OS_MAX_SLEEP_TICKS = OS_MAX_TIME_INTERVAL
};

typedef void (*os_task_entry_t)(void *argument);

typedef struct {
    os_task_entry_t entry;
    void *argument;
    size_t stack_size;
    os_task_timing_config_t timing;
} os_task_config_t;

typedef struct {
    os_task_id_t id;
    os_task_state_t state;
    os_task_kind_t kind;
    uint64_t release_order;
    uint64_t next_release_order;
    uint64_t wake_order;
    uint64_t absolute_deadline_order;
    uint32_t release_tick;
    uint32_t next_release_tick;
    uint32_t wake_tick;
    uint32_t period_ticks;
    uint32_t relative_deadline_ticks;
    uint32_t absolute_deadline_tick;
    uint32_t execution_budget_ticks;
    uint32_t total_execution_ticks;
    uint32_t job_execution_ticks;
    uint32_t completed_job_count;
    uint32_t deadline_miss_count;
    uint32_t missed_release_count;
    uint32_t job_sequence;
    uint8_t priority;
    bool job_active;
} os_task_info_t;

int os_kernel_init(void);
os_task_config_t os_task_config_default(os_task_entry_t entry, void *argument);
int os_task_create(const os_task_config_t *config, os_task_id_t *created_id);
void os_kernel_start(void) __attribute__((noreturn));
void os_yield(void);
int os_sleep(uint32_t ticks);
int os_wait_next_period(void);
void os_task_exit(void) __attribute__((noreturn));

uint32_t os_tick_count(void);
uint64_t os_monotonic_tick_count(void);
os_task_id_t os_current_task(void);
os_task_id_t os_last_reclaimed_task(void);
uint32_t os_reclaim_count(void);
int os_task_info(os_task_id_t id, os_task_info_t *info);
void *os_memory_alloc(size_t size);
int os_memory_free(void *pointer);
int os_memory_stats(os_memory_stats_t *stats);
size_t os_memory_count_fragments(size_t requested_size);
bool os_memory_validate(void);
bool os_thread_uses_psp(void);
bool os_exception_configuration_valid(void);

/* Architecture entry points. Application code must not call these directly. */
void os_svc_dispatch(uint32_t *exception_frame, uint32_t exc_return);
uint32_t *os_pendsv_switch(uint32_t *saved_psp);
void os_kernel_tick(void);
void os_kernel_panic(const char *reason) __attribute__((noreturn));

extern volatile uint32_t os_arch_has_active_context;

#endif
