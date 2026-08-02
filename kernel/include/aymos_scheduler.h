#ifndef AYMOS_SCHEDULER_H
#define AYMOS_SCHEDULER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t os_task_id_t;

enum {
    OS_TASK_ID_IDLE = 0U,
    OS_TASK_ID_INVALID = UINT8_MAX,
    OS_MAX_USER_TASKS = 4U,
    OS_MAX_TIME_INTERVAL = INT32_MAX
};

typedef enum {
    OS_TASK_DORMANT = 0,
    OS_TASK_WAITING_RELEASE,
    OS_TASK_READY,
    OS_TASK_RUNNING,
    OS_TASK_SLEEPING,
    OS_TASK_EXITING
} os_task_state_t;

typedef enum {
    OS_TASK_ONE_SHOT = 0,
    OS_TASK_PERIODIC
} os_task_kind_t;

typedef struct {
    os_task_kind_t kind;
    uint32_t initial_release_delay_ticks;
    uint32_t period_ticks;
    uint32_t relative_deadline_ticks;
    uint32_t execution_budget_ticks;
    uint8_t priority;
} os_task_timing_config_t;

/* This is kernel-owned state. It is public only so the architecture-neutral
 * policy can be compiled unchanged into the firmware and native tests. */
typedef struct {
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
    os_task_id_t id;
    os_task_state_t state;
    os_task_kind_t kind;
    uint8_t priority;
    bool job_active;
    bool deadline_miss_latched;
    bool time_range_exhausted;
} os_sched_task_t;

bool os_tick_interval_valid(uint32_t interval);
bool os_tick_before(uint64_t first, uint64_t second);
bool os_tick_reached(uint64_t now, uint64_t target);

bool os_sched_config_valid(const os_task_timing_config_t *config);
void os_sched_task_reset(os_sched_task_t *task, os_task_id_t id);
bool os_sched_task_configure(os_sched_task_t *task,
                             const os_task_timing_config_t *config,
                             uint64_t now);
bool os_sched_transition(os_sched_task_t *task, os_task_state_t expected,
                         os_task_state_t next);
bool os_sched_release_due(os_sched_task_t *task, uint64_t now);
bool os_sched_wake_due(os_sched_task_t *task, uint64_t now);
bool os_sched_sleep(os_sched_task_t *task, uint64_t now,
                    uint32_t sleep_ticks);
bool os_sched_complete_job(os_sched_task_t *task, uint64_t now);
bool os_sched_wait_next_period(os_sched_task_t *task, uint64_t now);
bool os_sched_record_deadline_miss(os_sched_task_t *task, uint64_t now);
bool os_sched_record_active_release(os_sched_task_t *task, uint64_t now);
void os_sched_account_running_tick(os_sched_task_t *task);
bool os_sched_outranks(const os_sched_task_t *candidate,
                       const os_sched_task_t *incumbent);
os_task_id_t os_sched_select(const os_sched_task_t *tasks, size_t task_count,
                             os_task_id_t excluded);

#endif
