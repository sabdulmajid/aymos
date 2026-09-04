#include "aymos_kernel.h"
#include "aymos_trace_runtime.h"
#include "board.h"

#include <stdbool.h>
#include <stdint.h>

#ifndef AYMOS_DEADLINE_LAB_OVERLOAD
#define AYMOS_DEADLINE_LAB_OVERLOAD 0
#endif

#if AYMOS_DEADLINE_LAB_OVERLOAD != 0 && \
    AYMOS_DEADLINE_LAB_OVERLOAD != 1
#error "AYMOS_DEADLINE_LAB_OVERLOAD must be 0 or 1"
#endif

enum {
    TASK_SAMPLER = 1U,
    TASK_CONTROLLER = 2U,
    TASK_TELEMETRY = 3U,
    TASK_LOAD = 4U,
    TASK_COUNT = 5U,
    SAMPLER_JOBS = 3U,
    CONTROLLER_JOBS = 2U,
    TELEMETRY_JOBS = 2U,
    LOAD_JOBS = 1U,
    SAMPLER_EXECUTION_TICKS = 1U,
    CONTROLLER_EXECUTION_TICKS = 2U,
    TELEMETRY_EXECUTION_TICKS = 1U,
    LOAD_NORMAL_EXECUTION_TICKS = 1U,
    LOAD_OVERLOAD_EXECUTION_TICKS = 8U,
    FINAL_TICK = 21U
};

typedef enum {
    ROLE_SAMPLER,
    ROLE_CONTROLLER,
    ROLE_TELEMETRY,
    ROLE_LOAD
} task_role_t;

typedef struct {
    const char *name;
    task_role_t role;
    os_task_id_t id;
    uint32_t execution_ticks;
    uint32_t job_limit;
    uint32_t jobs_completed;
    volatile uint32_t checksum;
} workload_task_t;

static workload_task_t workload[TASK_COUNT] = {
    [TASK_SAMPLER] = {
        .name = "sampler",
        .role = ROLE_SAMPLER,
        .execution_ticks = SAMPLER_EXECUTION_TICKS,
        .job_limit = SAMPLER_JOBS,
    },
    [TASK_CONTROLLER] = {
        .name = "controller",
        .role = ROLE_CONTROLLER,
        .execution_ticks = CONTROLLER_EXECUTION_TICKS,
        .job_limit = CONTROLLER_JOBS,
    },
    [TASK_TELEMETRY] = {
        .name = "telemetry",
        .role = ROLE_TELEMETRY,
        .execution_ticks = TELEMETRY_EXECUTION_TICKS,
        .job_limit = TELEMETRY_JOBS,
    },
    [TASK_LOAD] = {
        .name = "load",
        .role = ROLE_LOAD,
#if AYMOS_DEADLINE_LAB_OVERLOAD
        .execution_ticks = LOAD_OVERLOAD_EXECUTION_TICKS,
#else
        .execution_ticks = LOAD_NORMAL_EXECUTION_TICKS,
#endif
        .job_limit = LOAD_JOBS,
    },
};

static volatile bool task_finished[TASK_COUNT];
static volatile uint32_t sensor_sample;
static volatile uint32_t control_output;
static volatile uint32_t telemetry_digest;
static bool trace_flushed;

static void workload_task(void *argument);
static void consume_execution(workload_task_t *task);
static void perform_work(workload_task_t *task, uint32_t job_sequence,
                         uint32_t execution_step);
static void require(bool condition, const char *reason);
static void write_line(const char *line);

int main(void)
{
    os_task_config_t sampler =
        os_task_config_default(workload_task, &workload[TASK_SAMPLER]);
    os_task_config_t controller =
        os_task_config_default(workload_task, &workload[TASK_CONTROLLER]);
    os_task_config_t telemetry =
        os_task_config_default(workload_task, &workload[TASK_TELEMETRY]);
    os_task_config_t load =
        os_task_config_default(workload_task, &workload[TASK_LOAD]);

    sampler.timing.kind = OS_TASK_PERIODIC;
    sampler.timing.period_ticks = 6U;
    sampler.timing.relative_deadline_ticks = 3U;
    sampler.timing.execution_budget_ticks = SAMPLER_EXECUTION_TICKS;
    sampler.timing.priority = 0U;

    controller.timing.kind = OS_TASK_PERIODIC;
    controller.timing.initial_release_delay_ticks = 1U;
    controller.timing.period_ticks = 10U;
    controller.timing.relative_deadline_ticks = 8U;
    controller.timing.execution_budget_ticks = CONTROLLER_EXECUTION_TICKS;
    controller.timing.priority = 1U;

    telemetry.timing.kind = OS_TASK_PERIODIC;
    telemetry.timing.initial_release_delay_ticks = 2U;
    telemetry.timing.period_ticks = 18U;
    telemetry.timing.relative_deadline_ticks = 14U;
    telemetry.timing.execution_budget_ticks = TELEMETRY_EXECUTION_TICKS;
    telemetry.timing.priority = 2U;

    load.timing.relative_deadline_ticks = 12U;
    load.timing.execution_budget_ticks = workload[TASK_LOAD].execution_ticks;
    load.timing.priority = 3U;

    require(board_init() == HAL_OK, "BOARD_INIT");
    write_line("AYMOS READY\r\n");
    write_line("TRACE BEGIN\r\n");
    require(os_kernel_init() == 1, "KERNEL_INIT");
    require(os_exception_configuration_valid(), "EXCEPTION_CONFIG");
    require(os_task_create(&sampler, &workload[TASK_SAMPLER].id) == 1 &&
                workload[TASK_SAMPLER].id == TASK_SAMPLER,
            "CREATE_SAMPLER");
    require(os_task_create(&controller, &workload[TASK_CONTROLLER].id) == 1 &&
                workload[TASK_CONTROLLER].id == TASK_CONTROLLER,
            "CREATE_CONTROLLER");
    require(os_task_create(&telemetry, &workload[TASK_TELEMETRY].id) == 1 &&
                workload[TASK_TELEMETRY].id == TASK_TELEMETRY,
            "CREATE_TELEMETRY");
    require(os_task_create(&load, &workload[TASK_LOAD].id) == 1 &&
                workload[TASK_LOAD].id == TASK_LOAD,
            "CREATE_LOAD");
    os_kernel_start();
}

void board_lifecycle_idle_hook(void)
{
    if (trace_flushed || !task_finished[TASK_SAMPLER] ||
        !task_finished[TASK_CONTROLLER] || !task_finished[TASK_TELEMETRY] ||
        !task_finished[TASK_LOAD]) {
        return;
    }

    require(os_thread_uses_psp(), "IDLE_PSP");
    require(os_tick_count() == FINAL_TICK && os_reclaim_count() == 4U &&
                os_last_reclaimed_task() == TASK_TELEMETRY &&
                sensor_sample != 0U && control_output != 0U &&
                telemetry_digest != 0U,
            "LAB_COMPLETE");
    trace_flushed = true;
    require(os_trace_finish() == 1, "TRACE_FLUSH");
    write_line("AYMOS TRACE DONE\r\n");
}

static void workload_task(void *argument)
{
    workload_task_t *const task = argument;
    require(task != NULL && task->id == os_current_task() &&
                task->id > OS_TASK_ID_IDLE && task->id < TASK_COUNT &&
                task->name != NULL && os_thread_uses_psp(),
            "TASK_START");

    for (;;) {
        os_task_info_t info;
        consume_execution(task);
        require(os_task_info(task->id, &info) == 1 && info.job_active &&
                    info.state == OS_TASK_RUNNING,
                "TASK_INFO");
        ++task->jobs_completed;
        if (task->jobs_completed == task->job_limit) {
            if (task->role == ROLE_LOAD) {
#if AYMOS_DEADLINE_LAB_OVERLOAD
                require(info.deadline_miss_count == 1U &&
                            os_tick_count() == 12U,
                        "OVERLOAD_MISS");
#else
                require(info.deadline_miss_count == 0U &&
                            os_tick_count() == 4U,
                        "NORMAL_LOAD");
#endif
            }
            task_finished[task->id] = true;
            return;
        }
        require(info.deadline_miss_count == 0U &&
                    os_wait_next_period() == 1,
                "WAIT_PERIOD");
    }
}

static void consume_execution(workload_task_t *task)
{
    uint32_t completed = 0U;
    while (completed < task->execution_ticks) {
        os_task_info_t info;
        require(os_task_info(task->id, &info) == 1 && info.job_active,
                "EXECUTION_INFO");
        while (completed < info.job_execution_ticks &&
               completed < task->execution_ticks) {
            perform_work(task, info.job_sequence, completed);
            ++completed;
        }
    }
}

static void perform_work(workload_task_t *task, uint32_t job_sequence,
                         uint32_t execution_step)
{
    uint32_t value = task->checksum ^ (uint32_t)task->id ^
                     (job_sequence << 8U) ^ execution_step ^
                     UINT32_C(0x9e3779b9);
    for (uint32_t round = 0U; round < 64U; ++round) {
        value ^= value << 13U;
        value ^= value >> 17U;
        value ^= value << 5U;
    }
    task->checksum = value;

    switch (task->role) {
    case ROLE_SAMPLER:
        sensor_sample = value;
        break;
    case ROLE_CONTROLLER:
        control_output = value ^ sensor_sample;
        break;
    case ROLE_TELEMETRY:
        telemetry_digest ^= value ^ control_output;
        break;
    case ROLE_LOAD:
        break;
    default:
        require(false, "TASK_ROLE");
        break;
    }
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
