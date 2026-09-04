#include "aymos_scheduler.h"
#include "aymos_edf_fixture.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

enum { TASK_COUNT = OS_MAX_USER_TASKS + 1U };

static unsigned int checks;

#define CHECK(condition)                                                       \
    do {                                                                       \
        ++checks;                                                              \
        if (!(condition)) {                                                    \
            (void)fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,     \
                          __LINE__, #condition);                               \
            exit(EXIT_FAILURE);                                                \
        }                                                                      \
    } while (false)

static os_task_timing_config_t one_shot(uint32_t delay, uint32_t deadline,
                                        uint8_t priority);
static os_task_timing_config_t periodic(uint32_t delay, uint32_t period,
                                        uint32_t deadline, uint8_t priority);
static void reset_all(os_sched_task_t tasks[TASK_COUNT]);
static void test_earliest_deadline_and_ties(void);
static void test_sleep_and_exact_wake(void);
static void test_deadline_miss_is_latched(void);
static void test_periodic_release_cadence(void);
static void test_active_periodic_release_is_visible(void);
static void test_tick_wraparound(void);
static void test_idle_and_delayed_release(void);
static void test_deterministic_slot_reuse(void);
static void test_configuration_boundaries(void);
static void test_arm_edf_fixture(void);
static void test_one_shot_termination(void);
static void test_strict_deadline_order_for_overdue_jobs(void);
static void test_edf_across_ordinary_wrap(void);
static void test_counter_saturation(void);
static void test_reachable_long_overdue_periodic_release(void);
static void test_monotonic_exhaustion_is_fail_stop(void);

int main(void)
{
    test_earliest_deadline_and_ties();
    test_sleep_and_exact_wake();
    test_deadline_miss_is_latched();
    test_periodic_release_cadence();
    test_active_periodic_release_is_visible();
    test_tick_wraparound();
    test_idle_and_delayed_release();
    test_deterministic_slot_reuse();
    test_configuration_boundaries();
    test_arm_edf_fixture();
    test_one_shot_termination();
    test_strict_deadline_order_for_overdue_jobs();
    test_edf_across_ordinary_wrap();
    test_counter_saturation();
    test_reachable_long_overdue_periodic_release();
    test_monotonic_exhaustion_is_fail_stop();
    (void)printf("scheduler-native: %u checks passed\n", checks);
    return EXIT_SUCCESS;
}

static os_task_timing_config_t one_shot(uint32_t delay, uint32_t deadline,
                                        uint8_t priority)
{
    const os_task_timing_config_t config = {
        .kind = OS_TASK_ONE_SHOT,
        .initial_release_delay_ticks = delay,
        .period_ticks = 0U,
        .relative_deadline_ticks = deadline,
        .execution_budget_ticks = 0U,
        .priority = priority,
    };
    return config;
}

static os_task_timing_config_t periodic(uint32_t delay, uint32_t period_ticks,
                                        uint32_t deadline, uint8_t priority)
{
    const os_task_timing_config_t config = {
        .kind = OS_TASK_PERIODIC,
        .initial_release_delay_ticks = delay,
        .period_ticks = period_ticks,
        .relative_deadline_ticks = deadline,
        .execution_budget_ticks = 0U,
        .priority = priority,
    };
    return config;
}

static void reset_all(os_sched_task_t tasks[TASK_COUNT])
{
    for (os_task_id_t id = 0U; id < TASK_COUNT; ++id) {
        os_sched_task_reset(&tasks[id], id);
    }
    CHECK(os_sched_transition(&tasks[OS_TASK_ID_IDLE], OS_TASK_DORMANT,
                              OS_TASK_READY));
}

static void test_earliest_deadline_and_ties(void)
{
    os_sched_task_t tasks[TASK_COUNT];
    reset_all(tasks);
    os_task_timing_config_t first = one_shot(0U, 20U, 10U);
    os_task_timing_config_t second = one_shot(0U, 10U, 200U);
    CHECK(os_sched_task_configure(&tasks[1], &first, 100U));
    CHECK(os_sched_task_configure(&tasks[2], &second, 100U));
    CHECK(os_sched_select(tasks, TASK_COUNT, OS_TASK_ID_INVALID) == 2U);

    reset_all(tasks);
    first = one_shot(0U, 10U, 2U);
    second = one_shot(0U, 10U, 1U);
    CHECK(os_sched_task_configure(&tasks[1], &first, 0U));
    CHECK(os_sched_task_configure(&tasks[2], &second, 0U));
    CHECK(os_sched_select(tasks, TASK_COUNT, OS_TASK_ID_INVALID) == 2U);

    reset_all(tasks);
    first = one_shot(0U, 10U, 1U);
    second = one_shot(0U, 10U, 1U);
    CHECK(os_sched_task_configure(&tasks[1], &first, 0U));
    CHECK(os_sched_task_configure(&tasks[2], &second, 0U));
    CHECK(os_sched_select(tasks, TASK_COUNT, OS_TASK_ID_INVALID) == 1U);
    CHECK(os_sched_select(tasks, TASK_COUNT, 1U) == 2U);
}

static void test_sleep_and_exact_wake(void)
{
    os_sched_task_t tasks[TASK_COUNT];
    reset_all(tasks);
    const os_task_timing_config_t config = one_shot(0U, 20U, 1U);
    CHECK(os_sched_task_configure(&tasks[1], &config, 100U));
    CHECK(os_sched_transition(&tasks[1], OS_TASK_READY, OS_TASK_RUNNING));
    os_sched_account_running_tick(&tasks[1]);
    const uint32_t execution_before_sleep = tasks[1].job_execution_ticks;
    const uint32_t total_execution_before_sleep =
        tasks[1].total_execution_ticks;
    const uint32_t job_sequence_before_sleep = tasks[1].job_sequence;
    const uint64_t deadline_before_sleep = tasks[1].absolute_deadline_order;
    const uint64_t release_before_sleep = tasks[1].release_order;
    CHECK(os_sched_sleep(&tasks[1], 101U, 5U));
    CHECK(tasks[1].wake_tick == 106U);
    CHECK(tasks[1].wake_order == 106U);
    CHECK(tasks[1].job_active);
    CHECK(tasks[1].absolute_deadline_order == deadline_before_sleep);
    CHECK(tasks[1].release_order == release_before_sleep);
    CHECK(tasks[1].total_execution_ticks == total_execution_before_sleep);
    CHECK(tasks[1].job_sequence == job_sequence_before_sleep);
    os_sched_account_running_tick(&tasks[1]);
    CHECK(tasks[1].job_execution_ticks == execution_before_sleep);
    CHECK(!os_sched_wake_due(&tasks[1], 105U));
    CHECK(tasks[1].state == OS_TASK_SLEEPING);
    CHECK(os_sched_select(tasks, TASK_COUNT, OS_TASK_ID_INVALID) ==
          OS_TASK_ID_IDLE);
    CHECK(os_sched_wake_due(&tasks[1], 106U));
    CHECK(tasks[1].state == OS_TASK_READY);
    CHECK(tasks[1].release_order == release_before_sleep);
    CHECK(tasks[1].absolute_deadline_order == deadline_before_sleep);
    CHECK(tasks[1].job_execution_ticks == execution_before_sleep);
    CHECK(tasks[1].total_execution_ticks == total_execution_before_sleep);
    CHECK(tasks[1].job_sequence == job_sequence_before_sleep);
}

static void test_deadline_miss_is_latched(void)
{
    os_sched_task_t task;
    os_sched_task_reset(&task, 1U);
    const os_task_timing_config_t config = one_shot(0U, 5U, 1U);
    CHECK(os_sched_task_configure(&task, &config, 10U));
    CHECK(task.absolute_deadline_tick == 15U);
    CHECK(!os_sched_record_deadline_miss(&task, 14U));
    CHECK(os_sched_record_deadline_miss(&task, 15U));
    CHECK(task.deadline_miss_count == 1U);
    CHECK(task.deadline_miss_latched);
    CHECK(!os_sched_record_deadline_miss(&task, 16U));
    CHECK(task.deadline_miss_count == 1U);
}

static void test_periodic_release_cadence(void)
{
    os_sched_task_t task;
    os_sched_task_reset(&task, 1U);
    const os_task_timing_config_t config = periodic(0U, 10U, 7U, 1U);
    CHECK(os_sched_task_configure(&task, &config, 100U));
    CHECK(task.release_tick == 100U);
    CHECK(task.next_release_tick == 110U);
    CHECK(task.absolute_deadline_tick == 107U);
    CHECK(task.job_sequence == 1U);
    CHECK(os_sched_transition(&task, OS_TASK_READY, OS_TASK_RUNNING));
    os_sched_account_running_tick(&task);
    CHECK(os_sched_wait_next_period(&task, 105U));
    CHECK(task.state == OS_TASK_WAITING_RELEASE);
    CHECK(task.completed_job_count == 1U);
    CHECK(task.total_execution_ticks == 1U);
    CHECK(!os_sched_release_due(&task, 109U));
    CHECK(os_sched_release_due(&task, 110U));
    CHECK(task.release_tick == 110U);
    CHECK(task.next_release_tick == 120U);
    CHECK(task.absolute_deadline_tick == 117U);
    CHECK(task.job_sequence == 2U);
    CHECK(task.job_execution_ticks == 0U);
}

static void test_active_periodic_release_is_visible(void)
{
    os_sched_task_t task;
    os_sched_task_reset(&task, 1U);
    const os_task_timing_config_t config = periodic(0U, 5U, 5U, 1U);
    CHECK(os_sched_task_configure(&task, &config, 20U));
    CHECK(os_sched_transition(&task, OS_TASK_READY, OS_TASK_RUNNING));
    CHECK(os_sched_record_deadline_miss(&task, 25U));
    CHECK(os_sched_record_active_release(&task, 25U));
    CHECK(task.missed_release_count == 1U);
    CHECK(task.next_release_tick == 30U);
    CHECK(!os_sched_record_active_release(&task, 26U));
    CHECK(os_sched_wait_next_period(&task, 26U));
    CHECK(!os_sched_release_due(&task, 29U));
    CHECK(os_sched_release_due(&task, 30U));
    CHECK(task.release_tick == 30U);
    CHECK(task.absolute_deadline_tick == 35U);
    CHECK(task.deadline_miss_count == 1U);
}

static void test_tick_wraparound(void)
{
    os_sched_task_t task;
    os_sched_task_reset(&task, 1U);
    const os_task_timing_config_t config = one_shot(4U, 3U, 1U);
    CHECK(os_sched_task_configure(&task, &config, UINT32_MAX - 2U));
    const uint64_t wrapped_release = UINT64_C(0x100000001);
    CHECK(task.next_release_order == wrapped_release);
    CHECK(task.next_release_tick == 1U);
    CHECK(os_tick_before(wrapped_release - 1U, wrapped_release));
    CHECK(!os_tick_reached(wrapped_release - 1U, wrapped_release));
    CHECK(!os_sched_release_due(&task, wrapped_release - 1U));
    CHECK(os_sched_release_due(&task, wrapped_release));
    CHECK(task.absolute_deadline_tick == 4U);
    CHECK(task.absolute_deadline_order == UINT64_C(0x100000004));
    CHECK(!os_sched_record_deadline_miss(&task,
                                         UINT64_C(0x100000003)));
    CHECK(os_sched_record_deadline_miss(&task,
                                        UINT64_C(0x100000004)));
}

static void test_idle_and_delayed_release(void)
{
    os_sched_task_t tasks[TASK_COUNT];
    reset_all(tasks);
    const os_task_timing_config_t config = one_shot(5U, 5U, 1U);
    CHECK(os_sched_task_configure(&tasks[1], &config, 0U));
    CHECK(tasks[1].state == OS_TASK_WAITING_RELEASE);
    CHECK(os_sched_select(tasks, TASK_COUNT, OS_TASK_ID_INVALID) ==
          OS_TASK_ID_IDLE);
    CHECK(!os_sched_release_due(&tasks[1], 4U));
    CHECK(os_sched_release_due(&tasks[1], 5U));
    CHECK(os_sched_select(tasks, TASK_COUNT, OS_TASK_ID_INVALID) == 1U);
}

static void test_deterministic_slot_reuse(void)
{
    os_sched_task_t tasks[TASK_COUNT];
    reset_all(tasks);
    os_task_timing_config_t config = one_shot(0U, 2U, 1U);
    CHECK(os_sched_task_configure(&tasks[1], &config, 0U));
    CHECK(os_sched_record_deadline_miss(&tasks[1], 2U));
    CHECK(os_sched_transition(&tasks[1], OS_TASK_READY, OS_TASK_RUNNING));
    os_sched_account_running_tick(&tasks[1]);
    CHECK(os_sched_complete_job(&tasks[1], 3U));
    CHECK(os_sched_transition(&tasks[1], OS_TASK_RUNNING, OS_TASK_EXITING));
    os_sched_task_reset(&tasks[1], 1U);

    config = one_shot(0U, 10U, 5U);
    CHECK(os_sched_task_configure(&tasks[1], &config, 100U));
    CHECK(tasks[1].deadline_miss_count == 0U);
    CHECK(tasks[1].total_execution_ticks == 0U);
    CHECK(tasks[1].completed_job_count == 0U);
    CHECK(tasks[1].job_sequence == 1U);
    CHECK(tasks[1].absolute_deadline_tick == 110U);

    os_task_timing_config_t other = one_shot(0U, 10U, 5U);
    CHECK(os_sched_task_configure(&tasks[2], &other, 100U));
    CHECK(os_sched_select(tasks, TASK_COUNT, OS_TASK_ID_INVALID) == 1U);
}

static void test_configuration_boundaries(void)
{
    os_task_timing_config_t config = one_shot(0U, 1U, 1U);
    CHECK(os_sched_config_valid(&config));
    config.relative_deadline_ticks = 0U;
    CHECK(!os_sched_config_valid(&config));
    config = one_shot((uint32_t)OS_MAX_TIME_INTERVAL,
                      (uint32_t)OS_MAX_TIME_INTERVAL, 1U);
    CHECK(!os_sched_config_valid(&config));
    config = periodic(0U, 9U, 10U, 1U);
    CHECK(!os_sched_config_valid(&config));
    config = periodic(0U, 10U, 10U, 1U);
    CHECK(os_sched_config_valid(&config));
    config.period_ticks = 0U;
    CHECK(!os_sched_config_valid(&config));
    config = one_shot(0U, 10U, 1U);
    config.period_ticks = 1U;
    CHECK(!os_sched_config_valid(&config));
    config = one_shot(0U, 1U, 1U);
    config.kind = (os_task_kind_t)99;
    CHECK(!os_sched_config_valid(&config));
    config = one_shot(UINT32_C(0x80000000), 1U, 1U);
    CHECK(!os_sched_config_valid(&config));
    config = one_shot(0U, UINT32_C(0x80000000), 1U);
    CHECK(!os_sched_config_valid(&config));
    config = one_shot(0U, 1U, 1U);
    config.execution_budget_ticks = UINT32_C(0x80000000);
    CHECK(!os_sched_config_valid(&config));
    config = periodic(0U, UINT32_C(0x80000000), 1U, 1U);
    CHECK(!os_sched_config_valid(&config));

    os_sched_task_t task;
    os_sched_task_reset(&task, 1U);
    config = one_shot(1U, 1U, 1U);
    CHECK(!os_sched_task_configure(&task, &config, UINT64_MAX));
    CHECK(task.state == OS_TASK_DORMANT);
}

static void test_arm_edf_fixture(void)
{
    os_sched_task_t tasks[TASK_COUNT];
    reset_all(tasks);
    const os_task_timing_config_t urgent = periodic(
        OS_EDF_FIXTURE_URGENT_RELEASE,
        OS_EDF_FIXTURE_URGENT_PERIOD,
        OS_EDF_FIXTURE_URGENT_RELATIVE_DEADLINE,
        OS_EDF_FIXTURE_URGENT_PRIORITY);
    const os_task_timing_config_t relaxed = one_shot(
        OS_EDF_FIXTURE_RELAXED_RELEASE,
        OS_EDF_FIXTURE_RELAXED_RELATIVE_DEADLINE,
        OS_EDF_FIXTURE_RELAXED_PRIORITY);
    CHECK(os_sched_task_configure(&tasks[OS_EDF_FIXTURE_URGENT_ID], &urgent,
                                  0U));
    CHECK(os_sched_task_configure(&tasks[OS_EDF_FIXTURE_RELAXED_ID], &relaxed,
                                  0U));
    CHECK(os_sched_select(tasks, TASK_COUNT, OS_TASK_ID_INVALID) ==
          OS_EDF_FIXTURE_RELAXED_ID);
    CHECK(os_sched_transition(&tasks[OS_EDF_FIXTURE_RELAXED_ID], OS_TASK_READY,
                              OS_TASK_RUNNING));
    CHECK(!os_sched_release_due(&tasks[OS_EDF_FIXTURE_URGENT_ID], 4U));
    CHECK(os_sched_release_due(&tasks[OS_EDF_FIXTURE_URGENT_ID], 5U));
    CHECK(tasks[OS_EDF_FIXTURE_URGENT_ID].absolute_deadline_tick ==
          OS_EDF_FIXTURE_URGENT_FIRST_DEADLINE);
    CHECK(tasks[OS_EDF_FIXTURE_RELAXED_ID].absolute_deadline_tick ==
          OS_EDF_FIXTURE_RELAXED_ABSOLUTE_DEADLINE);
    CHECK(os_sched_outranks(&tasks[OS_EDF_FIXTURE_URGENT_ID],
                            &tasks[OS_EDF_FIXTURE_RELAXED_ID]));
    CHECK(os_sched_transition(&tasks[OS_EDF_FIXTURE_RELAXED_ID],
                              OS_TASK_RUNNING, OS_TASK_READY));
    CHECK(os_sched_transition(&tasks[OS_EDF_FIXTURE_URGENT_ID], OS_TASK_READY,
                              OS_TASK_RUNNING));
    CHECK(os_sched_wait_next_period(&tasks[OS_EDF_FIXTURE_URGENT_ID], 6U));
    CHECK(tasks[OS_EDF_FIXTURE_URGENT_ID].next_release_tick ==
          OS_EDF_FIXTURE_URGENT_SECOND_RELEASE);
    CHECK(os_sched_select(tasks, TASK_COUNT, OS_TASK_ID_INVALID) ==
          OS_EDF_FIXTURE_RELAXED_ID);
    CHECK(!os_sched_release_due(&tasks[OS_EDF_FIXTURE_URGENT_ID], 19U));
    CHECK(os_sched_release_due(&tasks[OS_EDF_FIXTURE_URGENT_ID], 20U));
    CHECK(tasks[OS_EDF_FIXTURE_URGENT_ID].absolute_deadline_tick ==
          OS_EDF_FIXTURE_URGENT_SECOND_DEADLINE);
    CHECK(os_sched_outranks(&tasks[OS_EDF_FIXTURE_URGENT_ID],
                            &tasks[OS_EDF_FIXTURE_RELAXED_ID]));
}

static void test_one_shot_termination(void)
{
    os_sched_task_t tasks[TASK_COUNT];
    reset_all(tasks);
    const os_task_timing_config_t config = one_shot(0U, 10U, 1U);
    CHECK(os_sched_task_configure(&tasks[1], &config, 0U));
    CHECK(os_sched_transition(&tasks[1], OS_TASK_READY, OS_TASK_RUNNING));
    CHECK(os_sched_complete_job(&tasks[1], 1U));
    CHECK(!tasks[1].job_active);
    CHECK(tasks[1].completed_job_count == 1U);
    CHECK(os_sched_transition(&tasks[1], OS_TASK_RUNNING, OS_TASK_EXITING));
    os_sched_task_reset(&tasks[1], 1U);
    CHECK(tasks[1].state == OS_TASK_DORMANT);
    CHECK(os_sched_select(tasks, TASK_COUNT, OS_TASK_ID_INVALID) ==
          OS_TASK_ID_IDLE);
}

static void test_strict_deadline_order_for_overdue_jobs(void)
{
    os_sched_task_t tasks[TASK_COUNT];
    reset_all(tasks);
    const os_task_timing_config_t old_job = one_shot(0U, 1U, 1U);
    const os_task_timing_config_t half_range_new = one_shot(0U, 1U, 1U);
    CHECK(os_sched_task_configure(&tasks[1], &old_job, 0U));
    CHECK(os_sched_task_configure(&tasks[2], &half_range_new,
                                  UINT64_C(0x80000000)));
    CHECK(tasks[2].absolute_deadline_order -
              tasks[1].absolute_deadline_order ==
          UINT64_C(0x80000000));
    CHECK(os_sched_outranks(&tasks[1], &tasks[2]));
    CHECK(!os_sched_outranks(&tasks[2], &tasks[1]));
    CHECK(!os_sched_outranks(&tasks[1], &tasks[1]));
    CHECK(os_sched_select(tasks, TASK_COUNT, OS_TASK_ID_INVALID) == 1U);
    CHECK(os_sched_select(tasks, TASK_COUNT, OS_TASK_ID_INVALID) == 1U);

    reset_all(tasks);
    const os_task_timing_config_t ancient = one_shot(0U, 10U, 1U);
    const os_task_timing_config_t new_periodic = periodic(0U, 20U, 10U, 1U);
    CHECK(os_sched_task_configure(&tasks[1], &ancient, 0U));
    CHECK(os_sched_transition(&tasks[1], OS_TASK_READY, OS_TASK_RUNNING));
    const uint64_t much_later = UINT64_C(0x100000000) + 100U;
    CHECK(os_sched_task_configure(&tasks[2], &new_periodic, much_later));
    CHECK(much_later - tasks[1].absolute_deadline_order >
          UINT64_C(0x80000000));
    CHECK(!os_sched_outranks(&tasks[2], &tasks[1]));
    CHECK(os_sched_record_deadline_miss(&tasks[1], much_later));
    CHECK(tasks[1].deadline_miss_count == 1U);
    CHECK(!os_sched_outranks(&tasks[2], &tasks[1]));
}

static void test_edf_across_ordinary_wrap(void)
{
    os_sched_task_t tasks[TASK_COUNT];
    reset_all(tasks);
    const uint64_t near_wrap = UINT64_C(0xFFFFFFFF) - 2U;
    const os_task_timing_config_t earlier = one_shot(0U, 3U, 10U);
    const os_task_timing_config_t later = one_shot(0U, 5U, 1U);
    CHECK(os_sched_task_configure(&tasks[1], &earlier, near_wrap));
    CHECK(os_sched_task_configure(&tasks[2], &later, near_wrap));
    CHECK(tasks[1].absolute_deadline_tick == 0U);
    CHECK(tasks[2].absolute_deadline_tick == 2U);
    CHECK(tasks[1].absolute_deadline_order == UINT64_C(0x100000000));
    CHECK(tasks[2].absolute_deadline_order == UINT64_C(0x100000002));
    CHECK(os_sched_outranks(&tasks[1], &tasks[2]));
    CHECK(!os_sched_outranks(&tasks[2], &tasks[1]));
    CHECK(os_sched_select(tasks, TASK_COUNT, OS_TASK_ID_INVALID) == 1U);
}

static void test_counter_saturation(void)
{
    os_sched_task_t task;
    os_sched_task_reset(&task, 1U);
    const os_task_timing_config_t config = periodic(0U, 5U, 5U, 1U);
    CHECK(os_sched_task_configure(&task, &config, 0U));
    CHECK(os_sched_transition(&task, OS_TASK_READY, OS_TASK_RUNNING));
    task.total_execution_ticks = UINT32_MAX;
    task.job_execution_ticks = UINT32_MAX;
    os_sched_account_running_tick(&task);
    CHECK(task.total_execution_ticks == UINT32_MAX);
    CHECK(task.job_execution_ticks == UINT32_MAX);

    task.deadline_miss_count = UINT32_MAX;
    CHECK(os_sched_record_deadline_miss(&task, 5U));
    CHECK(task.deadline_miss_count == UINT32_MAX);
    task.missed_release_count = UINT32_MAX;
    CHECK(os_sched_record_active_release(&task, 5U));
    CHECK(task.missed_release_count == UINT32_MAX);
    CHECK(task.next_release_order == 10U);
    task.completed_job_count = UINT32_MAX;
    CHECK(os_sched_wait_next_period(&task, 6U));
    CHECK(task.completed_job_count == UINT32_MAX);

    os_sched_task_reset(&task, 1U);
    const os_task_timing_config_t delayed = one_shot(1U, 1U, 1U);
    CHECK(os_sched_task_configure(&task, &delayed, 0U));
    task.job_sequence = UINT32_MAX;
    CHECK(os_sched_release_due(&task, 1U));
    CHECK(task.job_sequence == UINT32_MAX);
}

static void test_reachable_long_overdue_periodic_release(void)
{
    const uint32_t long_period = (uint32_t)OS_MAX_TIME_INTERVAL;
    const uint32_t periodic_deadlines[] = {2U, long_period};

    for (size_t scenario = 0U;
         scenario < sizeof(periodic_deadlines) / sizeof(periodic_deadlines[0]);
         ++scenario) {
        os_sched_task_t tasks[TASK_COUNT];
        reset_all(tasks);
        const os_task_timing_config_t ancient = one_shot(0U, 1U, 1U);
        const os_task_timing_config_t periodic_task =
            periodic(0U, long_period, periodic_deadlines[scenario], 1U);
        CHECK(os_sched_task_configure(&tasks[1], &ancient, 0U));
        CHECK(os_sched_task_configure(&tasks[2], &periodic_task, 0U));

        /* Both tasks exist at kernel time zero. The ancient task runs first,
         * sleeps, and lets the periodic task complete its first job. */
        CHECK(os_sched_select(tasks, TASK_COUNT, OS_TASK_ID_INVALID) == 1U);
        CHECK(os_sched_transition(&tasks[1], OS_TASK_READY, OS_TASK_RUNNING));
        CHECK(os_sched_sleep(&tasks[1], 0U, 1U));
        CHECK(os_sched_select(tasks, TASK_COUNT, OS_TASK_ID_INVALID) == 2U);
        CHECK(os_sched_transition(&tasks[2], OS_TASK_READY, OS_TASK_RUNNING));
        CHECK(os_sched_wait_next_period(&tasks[2], 0U));
        CHECK(tasks[2].next_release_order == long_period);
        CHECK(os_sched_record_deadline_miss(&tasks[1], 1U));
        CHECK(os_sched_wake_due(&tasks[1], 1U));
        CHECK(os_sched_transition(&tasks[1], OS_TASK_READY, OS_TASK_RUNNING));

        /* The old one-shot remains active until the periodic cadence release.
         * This sequence is reachable with pre-start-only task creation. */
        CHECK(os_sched_release_due(&tasks[2], long_period));
        const uint64_t separation = tasks[2].absolute_deadline_order -
                                    tasks[1].absolute_deadline_order;
        if (scenario == 0U) {
            CHECK(separation == UINT64_C(0x80000000));
        } else {
            CHECK(separation > UINT64_C(0x80000000));
        }
        CHECK(!os_sched_outranks(&tasks[2], &tasks[1]));
        CHECK(tasks[1].deadline_miss_count == 1U);
        CHECK(tasks[1].job_active);

        CHECK(os_sched_transition(&tasks[1], OS_TASK_RUNNING, OS_TASK_READY));
        CHECK(os_sched_outranks(&tasks[1], &tasks[2]));
        CHECK(!os_sched_outranks(&tasks[2], &tasks[1]));
        CHECK(os_sched_select(tasks, TASK_COUNT, OS_TASK_ID_INVALID) == 1U);
        CHECK(os_sched_select(tasks, TASK_COUNT, OS_TASK_ID_INVALID) == 1U);
    }
}

static void test_monotonic_exhaustion_is_fail_stop(void)
{
    os_sched_task_t task;
    os_sched_task_reset(&task, 1U);
    const os_task_timing_config_t final_one_shot = one_shot(0U, 1U, 1U);
    CHECK(os_sched_task_configure(&task, &final_one_shot, UINT64_MAX - 1U));
    CHECK(task.absolute_deadline_order == UINT64_MAX);
    CHECK(os_sched_record_deadline_miss(&task, UINT64_MAX));

    os_sched_task_reset(&task, 1U);
    const os_task_timing_config_t periodic_task = periodic(0U, 5U, 5U, 1U);
    CHECK(os_sched_task_configure(&task, &periodic_task, 0U));
    task.next_release_order = UINT64_MAX - 1U;
    task.next_release_tick = UINT32_MAX - 1U;
    CHECK(!os_sched_record_active_release(&task, UINT64_MAX - 1U));
    CHECK(task.time_range_exhausted);
    CHECK(task.next_release_order == UINT64_MAX - 1U);
}
