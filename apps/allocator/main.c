#include "aymos_kernel.h"
#include "board.h"
#include "stm32f4xx.h"

#include <stdbool.h>
#include <stdint.h>

enum {
    ALLOCATOR_ITERATIONS = 8U,
    MANAGER_ARGUMENT = 0xA5U,
    MANAGER_DEADLINE = 1000000U,
    WORKER_DEADLINE = 100U
};

static os_task_id_t manager_id;
static os_task_id_t worker_id;
static void *manager_allocation;
static volatile uint32_t worker_completed_iteration;

static void write_line(const char *line);
static void write_worker_iteration(uint32_t iteration);
static void fill_bytes(void *pointer, size_t size, uint8_t value);
static bool bytes_equal(const void *pointer, size_t size, uint8_t value);
static void require(bool condition, const char *reason);
static void manager_task(void *argument);
static void worker_task(void *argument);

int main(void)
{
    os_memory_stats_t initial_stats;
    os_task_config_t manager_config = os_task_config_default(
        manager_task, (void *)(uintptr_t)MANAGER_ARGUMENT);
    manager_config.timing.relative_deadline_ticks = MANAGER_DEADLINE;
    manager_config.timing.priority = 200U;

    require(board_init() == HAL_OK, "BOARD_INIT");
    write_line("AYMOS READY\r\n");
    require(os_kernel_init() == 1, "KERNEL_INIT");
    require(os_exception_configuration_valid(), "EXCEPTION_CONFIG");
    require(os_memory_validate(), "HEAP_INITIAL");
    require(os_memory_stats(&initial_stats) == 1 &&
                initial_stats.allocated_blocks == 0U &&
                initial_stats.free_blocks == 1U &&
                initial_stats.free_bytes ==
                    initial_stats.largest_free_block_bytes,
            "HEAP_INITIAL_STATS");
    require(os_memory_alloc(OS_MEMORY_ALIGNMENT) == NULL,
            "ALLOC_BEFORE_START");
    require(os_task_create(&manager_config, &manager_id) == 1 &&
                manager_id == 1U,
            "CREATE_MANAGER");
    write_line("ALLOCATOR BEGIN\r\n");
    os_kernel_start();
}

void board_lifecycle_idle_hook(void)
{
    os_memory_stats_t final_stats;
    os_task_id_t rejected_id = OS_TASK_ID_INVALID;
    os_task_config_t rejected_config =
        os_task_config_default(worker_task, NULL);
    require(os_thread_uses_psp(), "IDLE_PSP");
    require(__get_PRIMASK() == 0U &&
                os_task_create(&rejected_config, &rejected_id) == 0 &&
                rejected_id == OS_TASK_ID_INVALID &&
                __get_PRIMASK() == 0U,
            "IDLE_CREATE_REJECT");
    require(os_reclaim_count() == ALLOCATOR_ITERATIONS + 1U &&
                os_last_reclaimed_task() == manager_id,
            "MANAGER_RECLAIM");
    require(os_memory_validate(), "HEAP_FINAL");
    require(os_memory_stats(&final_stats) == 1, "FINAL_STATS");
    require(final_stats.allocated_blocks == 0U &&
                final_stats.allocated_bytes == 0U &&
                final_stats.free_blocks == 1U &&
                final_stats.free_bytes ==
                    final_stats.largest_free_block_bytes &&
                final_stats.successful_allocations == 25U &&
                final_stats.successful_frees == 25U &&
                final_stats.failed_allocations == ALLOCATOR_ITERATIONS &&
                final_stats.invalid_frees ==
                    (2U * ALLOCATOR_ITERATIONS),
            "FINAL_COUNTS");
    write_line("ALLOCATOR STATS ALLOCATED=0 FREE_BLOCKS=1 INVALID_FREES=16\r\n");
    write_line("AYMOS ALLOCATOR PASS\r\n");
}

static void manager_task(void *argument)
{
    require((uintptr_t)argument == MANAGER_ARGUMENT &&
                os_current_task() == manager_id && os_thread_uses_psp(),
            "MANAGER_START");
    manager_allocation = os_memory_alloc(24U);
    require(manager_allocation != NULL &&
                ((uintptr_t)manager_allocation % OS_MEMORY_ALIGNMENT) == 0U,
            "MANAGER_ALLOC");
    fill_bytes(manager_allocation, 24U, 0x5AU);

    for (uint32_t iteration = 1U; iteration <= ALLOCATOR_ITERATIONS;
         ++iteration) {
        os_task_config_t worker_config = os_task_config_default(
            worker_task, (void *)(uintptr_t)iteration);
        worker_config.timing.relative_deadline_ticks = WORKER_DEADLINE;
        worker_config.timing.priority = 1U;
        worker_id = OS_TASK_ID_INVALID;

        require(__get_PRIMASK() == 0U, "CREATE_PRIMASK_BEFORE");
        require(os_task_create(&worker_config, &worker_id) == 1,
                "CREATE_WORKER");
        require(__get_PRIMASK() == 0U, "CREATE_PRIMASK_AFTER");
        require(worker_id == 2U, "WORKER_SLOT");
        require(worker_completed_iteration == iteration,
                "RUNTIME_CREATE_PREEMPT");
        require(os_reclaim_count() == iteration &&
                    os_last_reclaimed_task() == worker_id,
                "WORKER_RECLAIM");
        require(os_task_info(worker_id, &(os_task_info_t){0}) == 0,
                "WORKER_DORMANT");
        require(os_memory_validate(), "HEAP_AFTER_WORKER");
        require(bytes_equal(manager_allocation, 24U, 0x5AU),
                "MANAGER_CANARY");

        os_memory_stats_t loop_stats;
        require(os_memory_stats(&loop_stats) == 1 &&
                    loop_stats.allocated_blocks == 1U &&
                    loop_stats.allocated_bytes == 24U &&
                    loop_stats.successful_allocations == 1U + (3U * iteration) &&
                    loop_stats.successful_frees == 3U * iteration &&
                    loop_stats.failed_allocations == iteration &&
                    loop_stats.invalid_frees == 2U * iteration,
                "LOOP_STATS");
        write_worker_iteration(iteration);
    }

    require(os_memory_free(NULL) == 1, "FREE_NULL");
    require(os_memory_free(manager_allocation) == 1, "FREE_MANAGER");
    manager_allocation = NULL;
    require(os_memory_validate(), "HEAP_MANAGER_DONE");
    write_line("ALLOCATOR REUSED SLOT=2 COUNT=8\r\n");
}

static void worker_task(void *argument)
{
    const uint32_t iteration = (uint32_t)(uintptr_t)argument;
    require(iteration >= 1U && iteration <= ALLOCATOR_ITERATIONS &&
                os_current_task() == worker_id && os_thread_uses_psp(),
            "WORKER_START");
    require(__get_PRIMASK() == 0U, "WORKER_PRIMASK");

    require(os_memory_free(manager_allocation) == 0,
            "OWNER_VIOLATION");
    require(__get_PRIMASK() == 0U, "OWNER_PRIMASK");
    require(bytes_equal(manager_allocation, 24U, 0x5AU),
            "OWNER_CANARY");
    require(os_memory_alloc(SIZE_MAX) == NULL, "ALLOC_OVERFLOW");
    require(__get_PRIMASK() == 0U, "ALLOC_ERROR_PRIMASK");

    void *const first = os_memory_alloc(13U);
    void *const middle = os_memory_alloc(1024U);
    void *const last = os_memory_alloc(9U);
    require(first != NULL && middle != NULL && last != NULL,
            "WORKER_ALLOC");
    require(((uintptr_t)first % OS_MEMORY_ALIGNMENT) == 0U &&
                ((uintptr_t)middle % OS_MEMORY_ALIGNMENT) == 0U &&
                ((uintptr_t)last % OS_MEMORY_ALIGNMENT) == 0U,
            "WORKER_ALIGN");
    fill_bytes(first, 13U, 0x11U);
    fill_bytes(middle, 1024U, 0x22U);
    fill_bytes(last, 9U, 0x33U);
    require(bytes_equal(first, 13U, 0x11U) &&
                bytes_equal(middle, 1024U, 0x22U) &&
                bytes_equal(last, 9U, 0x33U),
            "WORKER_CANARY_INITIAL");
    require(os_memory_free(middle) == 1, "WORKER_FREE");
    require(os_memory_free(middle) == 0, "WORKER_DOUBLE_FREE");
    require(__get_PRIMASK() == 0U, "DOUBLE_FREE_PRIMASK");
    require(bytes_equal(first, 13U, 0x11U) &&
                bytes_equal(last, 9U, 0x33U),
            "WORKER_CANARY_FINAL");
    require(os_memory_validate(), "WORKER_HEAP");

    /* first and last intentionally remain owned by this task. PendSV must
       release them after this function returns and leaves the task PSP. */
    worker_completed_iteration = iteration;
}

static void write_worker_iteration(uint32_t iteration)
{
    char line[] = "ALLOCATOR WORKER ITER=0 SLOT=2\r\n";
    require(iteration >= 1U && iteration <= 9U, "ITERATION_FORMAT");
    line[22] = (char)('0' + iteration);
    write_line(line);
}

static void write_line(const char *line)
{
    size_t length = 0U;
    while (line[length] != '\0') {
        ++length;
    }
    require(board_uart_write(line, length) == HAL_OK, "UART");
}

static void fill_bytes(void *pointer, size_t size, uint8_t value)
{
    uint8_t *const bytes = pointer;
    for (size_t index = 0U; index < size; ++index) {
        bytes[index] = value;
    }
}

static bool bytes_equal(const void *pointer, size_t size, uint8_t value)
{
    const uint8_t *const bytes = pointer;
    for (size_t index = 0U; index < size; ++index) {
        if (bytes[index] != value) {
            return false;
        }
    }
    return true;
}

static void require(bool condition, const char *reason)
{
    if (!condition) {
        board_kernel_panic(reason);
    }
}
