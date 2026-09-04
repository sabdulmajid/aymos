#include "aymos_kernel.h"
#include "aymos_edf_fixture.h"
#include "board.h"

#include <stdbool.h>
#include <stdint.h>

static os_task_id_t urgent_id;
static os_task_id_t relaxed_id;
static volatile bool relaxed_running;
static volatile bool urgent_finished;

static void write_line(const char *line);
static void require(bool condition, const char *reason);
static void urgent_task(void *argument);
static void relaxed_task(void *argument);

int main(void)
{
    os_task_config_t urgent_config = os_task_config_default(urgent_task, NULL);
    os_task_config_t relaxed_config =
        os_task_config_default(relaxed_task, NULL);

    urgent_config.timing.initial_release_delay_ticks =
        OS_EDF_FIXTURE_URGENT_RELEASE;
    urgent_config.timing.kind = OS_TASK_PERIODIC;
    urgent_config.timing.period_ticks = OS_EDF_FIXTURE_URGENT_PERIOD;
    urgent_config.timing.relative_deadline_ticks =
        OS_EDF_FIXTURE_URGENT_RELATIVE_DEADLINE;
    urgent_config.timing.priority = OS_EDF_FIXTURE_URGENT_PRIORITY;
    relaxed_config.timing.relative_deadline_ticks =
        OS_EDF_FIXTURE_RELAXED_RELATIVE_DEADLINE;
    relaxed_config.timing.priority = OS_EDF_FIXTURE_RELAXED_PRIORITY;

    require(board_init() == HAL_OK, "BOARD_INIT");
    write_line("AYMOS READY\r\n");
    require(os_kernel_init() == 1, "KERNEL_INIT");
    require(os_exception_configuration_valid(), "EXCEPTION_CONFIG");
    require(os_task_create(&urgent_config, &urgent_id) == 1,
            "CREATE_URGENT");
    require(os_task_create(&relaxed_config, &relaxed_id) == 1,
            "CREATE_RELAXED");
    require(urgent_id == OS_EDF_FIXTURE_URGENT_ID &&
                relaxed_id == OS_EDF_FIXTURE_RELAXED_ID,
            "TASK_IDS");
    write_line("EDF BEGIN\r\n");
    os_kernel_start();
}

void board_lifecycle_idle_hook(void)
{
    require(os_thread_uses_psp(), "IDLE_PSP");
    require(os_reclaim_count() == 2U &&
                os_last_reclaimed_task() == relaxed_id,
            "RECLAIM_RELAXED");
    write_line("EDF RECLAIM TASK=2\r\n");
    write_line("AYMOS EDF PASS\r\n");
}

static void urgent_task(void *argument)
{
    os_task_info_t info;
    require(argument == NULL && os_current_task() == urgent_id &&
                os_thread_uses_psp(),
            "URGENT_START");
    require(relaxed_running, "URGENT_NOT_PREEMPT");
    require(os_task_info(urgent_id, &info) == 1, "URGENT_INFO");
    require(info.state == OS_TASK_RUNNING && info.job_active &&
                info.release_tick == OS_EDF_FIXTURE_URGENT_RELEASE &&
                info.absolute_deadline_tick ==
                    OS_EDF_FIXTURE_URGENT_FIRST_DEADLINE &&
                info.deadline_miss_count == 0U,
            "URGENT_TIMING");
    require(info.kind == OS_TASK_PERIODIC && info.job_sequence == 1U &&
                info.next_release_tick ==
                    OS_EDF_FIXTURE_URGENT_SECOND_RELEASE,
            "URGENT_PERIOD_1");
    write_line("EDF RELEASE TASK=1 JOB=1 RELEASE=5 DEADLINE=15\r\n");
    write_line("EDF PREEMPT FROM=2 TO=1 JOB=1\r\n");
    write_line("EDF WAIT TASK=1 NEXT_RELEASE=20\r\n");
    require(os_wait_next_period() == 1, "URGENT_WAIT");

    require(os_current_task() == urgent_id && os_thread_uses_psp(),
            "URGENT_RESUME");
    require(os_task_info(urgent_id, &info) == 1 &&
                info.state == OS_TASK_RUNNING && info.job_active &&
                info.job_sequence == 2U && info.completed_job_count == 1U &&
                info.release_tick ==
                    OS_EDF_FIXTURE_URGENT_SECOND_RELEASE &&
                info.absolute_deadline_tick ==
                    OS_EDF_FIXTURE_URGENT_SECOND_DEADLINE &&
                info.deadline_miss_count == 0U,
            "URGENT_PERIOD_2");
    write_line("EDF RELEASE TASK=1 JOB=2 RELEASE=20 DEADLINE=30\r\n");
    write_line("EDF PREEMPT FROM=2 TO=1 JOB=2\r\n");
    urgent_finished = true;
    write_line("EDF EXIT TASK=1\r\n");
}

static void relaxed_task(void *argument)
{
    os_task_info_t info;
    require(argument == NULL && os_current_task() == relaxed_id &&
                os_thread_uses_psp(),
            "RELAXED_START");
    require(os_task_info(relaxed_id, &info) == 1, "RELAXED_INFO");
    require(info.state == OS_TASK_RUNNING && info.job_active &&
                info.release_tick == OS_EDF_FIXTURE_RELAXED_RELEASE &&
                info.absolute_deadline_tick ==
                    OS_EDF_FIXTURE_RELAXED_ABSOLUTE_DEADLINE &&
                info.deadline_miss_count == 0U,
            "RELAXED_TIMING");
    relaxed_running = true;
    write_line("EDF SELECT TASK=2 RELEASE=0 DEADLINE=50\r\n");
    while (!urgent_finished) {
        __asm volatile("nop");
    }

    require(os_reclaim_count() == 1U &&
                os_last_reclaimed_task() == urgent_id,
            "RECLAIM_URGENT");
    require(os_task_info(relaxed_id, &info) == 1 &&
                info.state == OS_TASK_RUNNING && info.job_active &&
                info.deadline_miss_count == 0U,
            "RELAXED_RESUME");
    write_line("EDF RECLAIM TASK=1\r\n");
    write_line("EDF RESUME TASK=2\r\n");
    write_line("EDF EXIT TASK=2\r\n");
}

static void write_line(const char *line)
{
    size_t length = 0U;
    while (line[length] != '\0') {
        ++length;
    }
    require(board_uart_write(line, length) == HAL_OK, "UART");
}

static void require(bool condition, const char *reason)
{
    if (!condition) {
        board_kernel_panic(reason);
    }
}
