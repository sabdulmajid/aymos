#include "aymos_kernel.h"
#include "board.h"

#include <stdbool.h>
#include <stdint.h>

enum {
    TASK_A_ARGUMENT = 0xA5U,
    TASK_B_ARGUMENT = 0x5AU,
    PREEMPT_SLEEP_TICKS = 50U
};

static os_task_id_t task_a_id;
static os_task_id_t task_b_id;
static volatile bool task_b_waiting;

extern uint32_t lifecycle_yield_register_probe(void);
extern uint32_t lifecycle_preempt_register_probe(void);

static void write_line(const char *line);
static void require(bool condition, const char *reason);
static void task_a(void *argument);
static void task_b(void *argument);

int main(void)
{
    static const os_task_config_t task_a_config = {
        .entry = task_a,
        .argument = (void *)(uintptr_t)TASK_A_ARGUMENT,
        .stack_size = OS_MAX_STACK_SIZE,
        .deadline_ticks = 10U,
        .priority = 1U,
    };
    static const os_task_config_t task_b_config = {
        .entry = task_b,
        .argument = (void *)(uintptr_t)TASK_B_ARGUMENT,
        .stack_size = OS_MAX_STACK_SIZE,
        .deadline_ticks = 10U,
        .priority = 2U,
    };

    require(board_init() == HAL_OK, "BOARD_INIT");
    write_line("AYMOS READY\r\n");
    require(os_kernel_init() == 1, "KERNEL_INIT");
    require(os_exception_configuration_valid(), "EXCEPTION_CONFIG");
    require(os_task_create(&task_a_config, &task_a_id) == 1,
            "CREATE_A");
    require(os_task_create(&task_b_config, &task_b_id) == 1,
            "CREATE_B");
    require(task_a_id == 1U && task_b_id == 2U, "TASK_IDS");
    write_line("LIFECYCLE BEGIN\r\n");
    os_kernel_start();
}

void board_lifecycle_idle_hook(void)
{
    require(os_thread_uses_psp(), "IDLE_PSP");
    require(os_reclaim_count() == 2U &&
                os_last_reclaimed_task() == task_b_id,
            "RECLAIM_B");
    write_line("LIFECYCLE RECLAIM B\r\n");
    write_line("LIFECYCLE IDLE PSP=1\r\n");
    write_line("AYMOS LIFECYCLE PASS\r\n");
}

static void task_a(void *argument)
{
    require((uintptr_t)argument == TASK_A_ARGUMENT, "ARG_A");
    require(os_current_task() == task_a_id && os_thread_uses_psp(),
            "START_A");
    write_line("LIFECYCLE START A ARG=165 PSP=1\r\n");
    write_line("LIFECYCLE YIELD A_TO_B\r\n");
    require(lifecycle_yield_register_probe() == 1U, "REG_YIELD_A");

    require(os_current_task() == task_a_id, "RESUME_A");
    require(os_sleep(0U) == 0, "SLEEP_ZERO");
    require(os_sleep((uint32_t)OS_MAX_SLEEP_TICKS + 1U) == 0,
            "SLEEP_HALF_RANGE");
    write_line("LIFECYCLE RESUME A\r\n");
    write_line("LIFECYCLE SLEEP A\r\n");
    require(os_sleep(PREEMPT_SLEEP_TICKS) == 1, "SLEEP_VALID");

    require(task_b_waiting, "NO_PREEMPT");
    require(os_current_task() == task_a_id, "PREEMPT_A");
    write_line("LIFECYCLE PREEMPT B_TO_A\r\n");
    write_line("LIFECYCLE RETURN A\r\n");
}

static void task_b(void *argument)
{
    require((uintptr_t)argument == TASK_B_ARGUMENT, "ARG_B");
    require(os_current_task() == task_b_id && os_thread_uses_psp(),
            "START_B");
    write_line("LIFECYCLE START B ARG=90 PSP=1\r\n");
    write_line("LIFECYCLE YIELD B_TO_A\r\n");
    require(lifecycle_yield_register_probe() == 1U, "REG_YIELD_B");

    require(os_current_task() == task_b_id, "RESUME_B");
    write_line("LIFECYCLE RESUME B\r\n");
    task_b_waiting = true;
    require(lifecycle_preempt_register_probe() == 1U, "REG_PREEMPT");

    require(os_reclaim_count() == 1U &&
                os_last_reclaimed_task() == task_a_id,
            "RECLAIM_A");
    write_line("LIFECYCLE RECLAIM A\r\n");
    write_line("LIFECYCLE CONTINUE B\r\n");
    write_line("LIFECYCLE RETURN B\r\n");
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
