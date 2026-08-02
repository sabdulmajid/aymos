#include "aymos_scheduler.h"

#include <limits.h>
#include <string.h>

static bool activate_job(os_sched_task_t *task);
static void increment_saturating(uint32_t *value);

bool os_tick_interval_valid(uint32_t interval)
{
    return interval <= (uint32_t)OS_MAX_TIME_INTERVAL;
}

bool os_tick_before(uint64_t first, uint64_t second)
{
    return first < second;
}

bool os_tick_reached(uint64_t now, uint64_t target)
{
    return !os_tick_before(now, target);
}

bool os_sched_config_valid(const os_task_timing_config_t *config)
{
    if (config == NULL ||
        (config->kind != OS_TASK_ONE_SHOT &&
         config->kind != OS_TASK_PERIODIC) ||
        config->relative_deadline_ticks == 0U ||
        !os_tick_interval_valid(config->initial_release_delay_ticks) ||
        !os_tick_interval_valid(config->period_ticks) ||
        !os_tick_interval_valid(config->relative_deadline_ticks) ||
        !os_tick_interval_valid(config->execution_budget_ticks) ||
        config->initial_release_delay_ticks >
            (uint32_t)OS_MAX_TIME_INTERVAL -
                config->relative_deadline_ticks) {
        return false;
    }
    if (config->kind == OS_TASK_ONE_SHOT) {
        return config->period_ticks == 0U;
    }
    return config->period_ticks != 0U &&
           config->relative_deadline_ticks <= config->period_ticks;
}

void os_sched_task_reset(os_sched_task_t *task, os_task_id_t id)
{
    if (task == NULL) {
        return;
    }
    memset(task, 0, sizeof(*task));
    task->id = id;
    task->state = OS_TASK_DORMANT;
    task->kind = OS_TASK_ONE_SHOT;
}

bool os_sched_task_configure(os_sched_task_t *task,
                             const os_task_timing_config_t *config,
                             uint64_t now)
{
    if (task == NULL || task->state != OS_TASK_DORMANT ||
        !os_sched_config_valid(config) ||
        now > UINT64_MAX - config->initial_release_delay_ticks ||
        now + config->initial_release_delay_ticks >
            UINT64_MAX - config->relative_deadline_ticks) {
        return false;
    }

    const os_task_id_t id = task->id;
    os_sched_task_reset(task, id);
    const uint64_t release_order =
        now + config->initial_release_delay_ticks;
    if (config->kind == OS_TASK_PERIODIC &&
        release_order > UINT64_MAX - config->period_ticks) {
        return false;
    }
    task->kind = config->kind;
    task->release_order = release_order;
    task->release_tick = (uint32_t)task->release_order;
    task->next_release_order = task->release_order;
    task->next_release_tick = task->release_tick;
    task->period_ticks = config->period_ticks;
    task->relative_deadline_ticks = config->relative_deadline_ticks;
    task->execution_budget_ticks = config->execution_budget_ticks;
    task->priority = config->priority;
    task->state = OS_TASK_WAITING_RELEASE;

    if (config->initial_release_delay_ticks == 0U) {
        return activate_job(task);
    }
    return true;
}

bool os_sched_transition(os_sched_task_t *task, os_task_state_t expected,
                         os_task_state_t next)
{
    if (task == NULL || task->state != expected || expected == next) {
        return false;
    }
    task->state = next;
    return true;
}

bool os_sched_release_due(os_sched_task_t *task, uint64_t now)
{
    if (task == NULL || task->state != OS_TASK_WAITING_RELEASE ||
        task->job_active || !os_tick_reached(now, task->next_release_order)) {
        return false;
    }
    task->release_order = task->next_release_order;
    task->release_tick = task->next_release_tick;
    return activate_job(task);
}

bool os_sched_wake_due(os_sched_task_t *task, uint64_t now)
{
    if (task == NULL || task->state != OS_TASK_SLEEPING ||
        !task->job_active || !os_tick_reached(now, task->wake_order)) {
        return false;
    }
    task->state = OS_TASK_READY;
    return true;
}

bool os_sched_sleep(os_sched_task_t *task, uint64_t now,
                    uint32_t sleep_ticks)
{
    if (task == NULL || task->state != OS_TASK_RUNNING ||
        !task->job_active || sleep_ticks == 0U ||
        !os_tick_interval_valid(sleep_ticks) ||
        now > UINT64_MAX - sleep_ticks) {
        return false;
    }
    task->wake_order = now + sleep_ticks;
    task->wake_tick = (uint32_t)task->wake_order;
    task->state = OS_TASK_SLEEPING;
    return true;
}

bool os_sched_complete_job(os_sched_task_t *task, uint64_t now)
{
    if (task == NULL || task->state != OS_TASK_RUNNING || !task->job_active) {
        return false;
    }
    (void)os_sched_record_deadline_miss(task, now);
    task->job_active = false;
    increment_saturating(&task->completed_job_count);
    return true;
}

bool os_sched_wait_next_period(os_sched_task_t *task, uint64_t now)
{
    if (task == NULL || task->kind != OS_TASK_PERIODIC ||
        !os_sched_complete_job(task, now)) {
        return false;
    }
    task->state = OS_TASK_WAITING_RELEASE;
    return true;
}

bool os_sched_record_deadline_miss(os_sched_task_t *task, uint64_t now)
{
    if (task == NULL || !task->job_active || task->deadline_miss_latched ||
        !os_tick_reached(now, task->absolute_deadline_order)) {
        return false;
    }
    task->deadline_miss_latched = true;
    increment_saturating(&task->deadline_miss_count);
    return true;
}

bool os_sched_record_active_release(os_sched_task_t *task, uint64_t now)
{
    if (task == NULL || task->kind != OS_TASK_PERIODIC ||
        !task->job_active || task->state == OS_TASK_DORMANT ||
        task->state == OS_TASK_EXITING ||
        !os_tick_reached(now, task->next_release_order)) {
        return false;
    }

    /* PR 4 intentionally supports one active job per periodic task. A release
     * that arrives while its predecessor is active is counted, not queued or
     * silently converted into a completion-driven release. */
    if (task->period_ticks == 0U ||
        task->next_release_order > UINT64_MAX - task->period_ticks) {
        task->time_range_exhausted = true;
        return false;
    }
    increment_saturating(&task->missed_release_count);
    task->next_release_order += task->period_ticks;
    task->next_release_tick = (uint32_t)task->next_release_order;
    return true;
}

void os_sched_account_running_tick(os_sched_task_t *task)
{
    if (task == NULL || task->state != OS_TASK_RUNNING || !task->job_active) {
        return;
    }
    increment_saturating(&task->total_execution_ticks);
    increment_saturating(&task->job_execution_ticks);
}

bool os_sched_outranks(const os_sched_task_t *candidate,
                       const os_sched_task_t *incumbent)
{
    if (candidate == NULL || candidate->state != OS_TASK_READY ||
        !candidate->job_active) {
        return false;
    }
    if (incumbent == NULL || incumbent->id == OS_TASK_ID_IDLE) {
        return candidate->id != OS_TASK_ID_IDLE;
    }
    if (candidate->id == OS_TASK_ID_IDLE || !incumbent->job_active ||
        (incumbent->state != OS_TASK_READY &&
         incumbent->state != OS_TASK_RUNNING)) {
        return false;
    }
    if (candidate->absolute_deadline_order !=
        incumbent->absolute_deadline_order) {
        return os_tick_before(candidate->absolute_deadline_order,
                              incumbent->absolute_deadline_order);
    }
    if (candidate->priority != incumbent->priority) {
        return candidate->priority < incumbent->priority;
    }
    return candidate->id < incumbent->id;
}

os_task_id_t os_sched_select(const os_sched_task_t *tasks, size_t task_count,
                             os_task_id_t excluded)
{
    if (tasks == NULL || task_count == 0U ||
        task_count > (size_t)OS_TASK_ID_INVALID) {
        return OS_TASK_ID_INVALID;
    }

    os_task_id_t best = OS_TASK_ID_INVALID;
    for (size_t index = 1U; index < task_count; ++index) {
        const os_sched_task_t *const candidate = &tasks[index];
        if (candidate->id != (os_task_id_t)index ||
            candidate->id == excluded || candidate->state != OS_TASK_READY ||
            !candidate->job_active) {
            continue;
        }
        if (best == OS_TASK_ID_INVALID ||
            os_sched_outranks(candidate, &tasks[best])) {
            best = candidate->id;
        }
    }
    if (best == OS_TASK_ID_INVALID && excluded != OS_TASK_ID_INVALID &&
        (size_t)excluded < task_count && excluded != OS_TASK_ID_IDLE &&
        tasks[excluded].id == excluded &&
        tasks[excluded].state == OS_TASK_READY &&
        tasks[excluded].job_active) {
        best = excluded;
    }
    if (best == OS_TASK_ID_INVALID && tasks[OS_TASK_ID_IDLE].id == OS_TASK_ID_IDLE &&
        tasks[OS_TASK_ID_IDLE].state == OS_TASK_READY) {
        best = OS_TASK_ID_IDLE;
    }
    return best;
}

static bool activate_job(os_sched_task_t *task)
{
    if (task == NULL || task->state != OS_TASK_WAITING_RELEASE ||
        task->job_active) {
        return false;
    }
    if (task->release_order >
            UINT64_MAX - task->relative_deadline_ticks ||
        (task->kind == OS_TASK_PERIODIC &&
         task->release_order > UINT64_MAX - task->period_ticks)) {
        task->time_range_exhausted = true;
        return false;
    }
    task->absolute_deadline_order =
        task->release_order + task->relative_deadline_ticks;
    task->absolute_deadline_tick = (uint32_t)task->absolute_deadline_order;
    task->job_execution_ticks = 0U;
    task->deadline_miss_latched = false;
    task->job_active = true;
    increment_saturating(&task->job_sequence);
    if (task->kind == OS_TASK_PERIODIC) {
        task->next_release_order = task->release_order + task->period_ticks;
        task->next_release_tick = (uint32_t)task->next_release_order;
    } else {
        task->next_release_order = 0U;
        task->next_release_tick = 0U;
    }
    task->state = OS_TASK_READY;
    return true;
}

static void increment_saturating(uint32_t *value)
{
    if (*value != UINT32_MAX) {
        ++*value;
    }
}
