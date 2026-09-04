#include "aymos_kernel.h"
#include "aymos_trace_runtime.h"
#include "board.h"
#include "stm32f4xx.h"

#include <stdbool.h>
#include <stdint.h>

enum {
    A_DEADLINE = 15U,
    B_RELEASE_DELAY = 1U,
    B_DEADLINE = 3U,
    B_SLEEP_TICKS = 2U,
    C_DEADLINE = 200U,
    D_DEADLINE = 5U,
    E_RELEASE_DELAY = 3U,
    E_DEADLINE = 20U,
    A_SLEEP_TICKS = 7U
};

static os_task_id_t task_a_id;
static os_task_id_t task_b_id;
static os_task_id_t task_c_id;
static os_task_id_t task_d_id;
static os_task_id_t task_e_id;
static volatile bool task_a_finished;
static volatile bool task_b_finished;
static volatile bool task_c_finished;
static volatile bool task_d_finished;
static volatile bool task_e_finished;
static bool trace_flushed;

static void task_a(void *argument);
static void task_b(void *argument);
static void task_c(void *argument);
static void task_d(void *argument);
static void task_e(void *argument);
static void require(bool condition, const char *reason);
static void write_line(const char *line);

int main(void)
{
    os_task_config_t a = os_task_config_default(task_a, NULL);
    os_task_config_t b = os_task_config_default(task_b, NULL);
    os_task_config_t c = os_task_config_default(task_c, NULL);
    a.timing.relative_deadline_ticks = A_DEADLINE;
    b.timing.initial_release_delay_ticks = B_RELEASE_DELAY;
    b.timing.relative_deadline_ticks = B_DEADLINE;
    c.timing.relative_deadline_ticks = C_DEADLINE;

    require(board_init() == HAL_OK, "BOARD_INIT");
    write_line("AYMOS READY\r\n");
    write_line("TRACE BEGIN\r\n");
    require(os_kernel_init() == 1, "KERNEL_INIT");
    require(os_exception_configuration_valid(), "EXCEPTION_CONFIG");
    require(os_task_create(&a, &task_a_id) == 1 && task_a_id == 1U,
            "CREATE_A");
    require(os_task_create(&b, &task_b_id) == 1 && task_b_id == 2U,
            "CREATE_B");
    require(os_task_create(&c, &task_c_id) == 1 && task_c_id == 3U,
            "CREATE_C");
    os_kernel_start();
}

void board_lifecycle_idle_hook(void)
{
    if (!task_a_finished || trace_flushed) {
        return;
    }
    require(os_thread_uses_psp(), "IDLE_PSP");
    require(task_b_finished && task_c_finished && task_d_finished &&
                task_e_finished && os_tick_count() == A_DEADLINE &&
                os_reclaim_count() == 5U &&
                os_last_reclaimed_task() == task_a_id,
            "TRACE_COMPLETE");
    trace_flushed = true;
    require(os_trace_finish() == 1, "TRACE_FLUSH");
    write_line("AYMOS TRACE DONE\r\n");
}

static void task_a(void *argument)
{
    require(argument == NULL && os_current_task() == task_a_id,
            "TASK_A_START");
    void *const memory = os_memory_alloc(24U);
    require(memory != NULL && ((uintptr_t)memory & 7U) == 0U,
            "TASK_A_ALLOC");
    require(os_memory_free(memory) == 1, "TASK_A_FREE");

    os_task_config_t d = os_task_config_default(task_d, NULL);
    d.timing.relative_deadline_ticks = D_DEADLINE;
    task_d_id = OS_TASK_ID_INVALID;
    __disable_irq();
    require(os_task_create(&d, &task_d_id) == 1 && task_d_id == 4U,
            "CREATE_D");
    require(__get_PRIMASK() == 1U, "CREATE_D_PRIMASK");
    while ((SCB->ICSR & SCB_ICSR_PENDSTSET_Msk) == 0U) {
        __NOP();
    }
    __enable_irq();

    require(__get_PRIMASK() == 0U && os_tick_count() == B_RELEASE_DELAY &&
                !task_b_finished && task_d_finished &&
                os_reclaim_count() == 1U &&
                os_last_reclaimed_task() == task_d_id,
            "COALESCED_PREEMPT");

    os_task_config_t e = os_task_config_default(task_e, NULL);
    e.timing.initial_release_delay_ticks = E_RELEASE_DELAY;
    e.timing.relative_deadline_ticks = E_DEADLINE;
    task_e_id = OS_TASK_ID_INVALID;
    require(os_task_create(&e, &task_e_id) == 1 &&
                task_e_id == task_d_id,
            "CREATE_E_REUSE");

    os_yield();
    require(os_current_task() == task_a_id && task_c_finished &&
                os_reclaim_count() == 2U &&
                os_last_reclaimed_task() == task_c_id,
            "TASK_A_RESUME");
    require(os_sleep(A_SLEEP_TICKS) == 1, "TASK_A_SLEEP");
    require(task_b_finished && task_e_finished &&
                os_current_task() == task_a_id &&
                os_reclaim_count() == 4U &&
                os_last_reclaimed_task() == task_e_id,
            "TASK_A_WAKE");
    while (os_tick_count() < A_DEADLINE) {
        __asm volatile("nop");
    }
    task_a_finished = true;
}

static void task_b(void *argument)
{
    require(argument == NULL && os_current_task() == task_b_id,
            "TASK_B_START");
    require(os_sleep(B_SLEEP_TICKS) == 1, "TASK_B_SLEEP");
    require(os_current_task() == task_b_id, "TASK_B_WAKE");
    task_b_finished = true;
}

static void task_c(void *argument)
{
    require(argument == NULL && os_current_task() == task_c_id,
            "TASK_C_START");
    task_c_finished = true;
}

static void task_d(void *argument)
{
    require(argument == NULL && os_current_task() == task_d_id,
            "TASK_D_START");
    task_d_finished = true;
}

static void task_e(void *argument)
{
    require(argument == NULL && os_current_task() == task_e_id,
            "TASK_E_START");
    task_e_finished = true;
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
