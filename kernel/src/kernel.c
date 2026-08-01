#include "aymos_kernel.h"

#include "board.h"
#include "stm32f4xx.h"

#include <limits.h>
#include <stdint.h>

enum {
    OS_TASK_COUNT = OS_MAX_USER_TASKS + 1U,
    OS_INITIAL_FRAME_WORDS = 16U,
    OS_XPSR_THUMB = 1U << 24,
    OS_EXC_RETURN_THREAD_MSP = 0xFFFFFFF9U,
    OS_EXC_RETURN_THREAD_PSP = 0xFFFFFFFDU,
    OS_SVC_START = 0U,
    OS_SVC_YIELD = 1U,
    OS_SVC_SLEEP = 2U,
    OS_SVC_EXIT = 3U,
    OS_PRIORITY_SVC = 0x80U,
    OS_PRIORITY_SYSTICK = 0xE0U,
    OS_PRIORITY_PENDSV = 0xF0U,
    OS_FLASH_START = 0x08000000U,
    OS_FLASH_END = 0x08080000U
};

typedef struct {
    os_task_entry_t entry;
    void *argument;
    uint32_t *saved_psp;
    uint32_t *stack_low;
    uint32_t *stack_high;
    uint32_t deadline_ticks;
    uint32_t wake_tick;
    os_task_id_t id;
    os_task_state_t state;
    uint8_t priority;
    bool stack_claimed;
} os_tcb_t;

typedef struct {
    _Alignas(8) uint32_t words[OS_MAX_STACK_SIZE / sizeof(uint32_t)];
} os_stack_slot_t;

_Static_assert((OS_MAX_STACK_SIZE % 8U) == 0U,
               "task stack slots must preserve exception alignment");
_Static_assert(sizeof(os_stack_slot_t) == OS_MAX_STACK_SIZE,
               "task stack slot has unexpected padding");
_Static_assert(_Alignof(os_stack_slot_t) >= 8U,
               "task stack slots must be eight-byte aligned");

static os_tcb_t tasks[OS_TASK_COUNT];
static os_stack_slot_t stacks[OS_TASK_COUNT];
static os_task_id_t current_task = OS_TASK_ID_INVALID;
static os_task_id_t yield_excluded_task = OS_TASK_ID_INVALID;
static volatile uint32_t ticks;
static volatile uint32_t reclaim_counter;
static volatile os_task_id_t last_reclaimed = OS_TASK_ID_INVALID;
static bool initialized;
static bool running;

volatile uint32_t os_arch_has_active_context;

static void idle_entry(void *argument) __attribute__((noreturn));
static void task_entry_trampoline(void *argument) __attribute__((noreturn));
static void task_return_guard(void) __attribute__((noreturn));
static void reset_task(os_task_id_t id);
static void transition_task(os_task_id_t id, os_task_state_t expected,
                            os_task_state_t next, const char *reason);
static uint32_t *build_initial_frame(os_task_id_t id);
static os_task_id_t select_ready_task(os_task_id_t excluded);
static bool tick_reached(uint32_t now, uint32_t target);
static bool task_outranks(os_task_id_t candidate, os_task_id_t incumbent);
static void request_pendsv(void);
static void verify_state(const char *where);
static uint8_t decode_svc(const uint32_t *frame, uint32_t exc_return);
static bool valid_exc_return(uint32_t exc_return);
static bool valid_saved_psp(os_task_id_t id, const uint32_t *psp);
static bool valid_svc_frame(const uint32_t *frame, uint32_t exc_return);
static void reclaim_exiting_task(os_task_id_t id);

int os_kernel_init(void)
{
    if (initialized || running) {
        return 0;
    }

    for (os_task_id_t id = 0U; id < OS_TASK_COUNT; ++id) {
        reset_task(id);
    }

    tasks[OS_TASK_ID_IDLE].entry = idle_entry;
    tasks[OS_TASK_ID_IDLE].argument = NULL;
    tasks[OS_TASK_ID_IDLE].deadline_ticks = UINT32_MAX;
    tasks[OS_TASK_ID_IDLE].priority = UINT8_MAX;
    transition_task(OS_TASK_ID_IDLE, OS_TASK_DORMANT, OS_TASK_READY,
                    "IDLE_READY");
    tasks[OS_TASK_ID_IDLE].stack_claimed = true;
    tasks[OS_TASK_ID_IDLE].stack_low = stacks[OS_TASK_ID_IDLE].words;
    tasks[OS_TASK_ID_IDLE].stack_high =
        stacks[OS_TASK_ID_IDLE].words +
        (OS_MAX_STACK_SIZE / sizeof(uint32_t));
    tasks[OS_TASK_ID_IDLE].saved_psp = build_initial_frame(OS_TASK_ID_IDLE);

    ticks = 0U;
    reclaim_counter = 0U;
    last_reclaimed = OS_TASK_ID_INVALID;
    current_task = OS_TASK_ID_INVALID;
    yield_excluded_task = OS_TASK_ID_INVALID;
    os_arch_has_active_context = 0U;

    SCB->CCR |= SCB_CCR_STKALIGN_Msk;
    HAL_NVIC_SetPriority(SVCall_IRQn, OS_PRIORITY_SVC >> 4U, 0U);
    HAL_NVIC_SetPriority(SysTick_IRQn, OS_PRIORITY_SYSTICK >> 4U, 0U);
    HAL_NVIC_SetPriority(PendSV_IRQn, OS_PRIORITY_PENDSV >> 4U, 0U);

    initialized = true;
    verify_state("INIT");
    return 1;
}

int os_task_create(const os_task_config_t *config, os_task_id_t *created_id)
{
    if (!initialized || running || config == NULL || created_id == NULL ||
        config->entry == NULL || config->stack_size < OS_MIN_STACK_SIZE ||
        config->stack_size > OS_MAX_STACK_SIZE ||
        (config->stack_size & 7U) != 0U || config->deadline_ticks == 0U) {
        return 0;
    }

    os_task_id_t id = OS_TASK_ID_INVALID;
    for (os_task_id_t candidate = 1U; candidate < OS_TASK_COUNT; ++candidate) {
        if (tasks[candidate].state == OS_TASK_DORMANT &&
            !tasks[candidate].stack_claimed) {
            id = candidate;
            break;
        }
    }
    if (id == OS_TASK_ID_INVALID) {
        return 0;
    }

    tasks[id].entry = config->entry;
    tasks[id].argument = config->argument;
    tasks[id].deadline_ticks = config->deadline_ticks;
    tasks[id].priority = config->priority;
    tasks[id].stack_claimed = true;
    tasks[id].stack_low = stacks[id].words;
    tasks[id].stack_high =
        stacks[id].words + (config->stack_size / sizeof(uint32_t));
    tasks[id].saved_psp = build_initial_frame(id);
    transition_task(id, OS_TASK_DORMANT, OS_TASK_READY, "CREATE_READY");
    *created_id = id;

    verify_state("CREATE");
    return 1;
}

void os_kernel_start(void)
{
    if (!initialized || running) {
        os_kernel_panic("START_STATE");
    }
    running = true;
    __asm volatile("svc #0" ::: "memory");
    os_kernel_panic("START_RETURN");
}

void os_yield(void)
{
    if (!running || current_task == OS_TASK_ID_INVALID) {
        os_kernel_panic("YIELD_STATE");
    }
    __asm volatile("svc #1" ::: "memory");
}

int os_sleep(uint32_t sleep_ticks)
{
    if (!running || current_task == OS_TASK_ID_INVALID) {
        return 0;
    }
    if (sleep_ticks == 0U || sleep_ticks > OS_MAX_SLEEP_TICKS) {
        return 0;
    }
    register uint32_t argument __asm("r0") = sleep_ticks;
    __asm volatile("svc #2" : "+r"(argument) :: "memory");
    return 1;
}

void os_task_exit(void)
{
    if (!running || current_task == OS_TASK_ID_INVALID ||
        current_task == OS_TASK_ID_IDLE) {
        os_kernel_panic("EXIT_STATE");
    }
    __asm volatile("svc #3" ::: "memory");
    os_kernel_panic("EXIT_RETURN");
}

uint32_t os_tick_count(void)
{
    return ticks;
}

os_task_id_t os_current_task(void)
{
    return current_task;
}

os_task_id_t os_last_reclaimed_task(void)
{
    return last_reclaimed;
}

uint32_t os_reclaim_count(void)
{
    return reclaim_counter;
}

int os_task_info(os_task_id_t id, os_task_info_t *info)
{
    if (id >= OS_TASK_COUNT || info == NULL) {
        return 0;
    }
    const uint32_t saved_primask = __get_PRIMASK();
    __disable_irq();
    if (tasks[id].state == OS_TASK_DORMANT) {
        __set_PRIMASK(saved_primask);
        return 0;
    }
    info->id = id;
    info->state = tasks[id].state;
    info->deadline_ticks = tasks[id].deadline_ticks;
    info->priority = tasks[id].priority;
    __set_PRIMASK(saved_primask);
    return 1;
}

bool os_thread_uses_psp(void)
{
    if (__get_IPSR() != 0U || (__get_CONTROL() & CONTROL_SPSEL_Msk) == 0U ||
        current_task == OS_TASK_ID_INVALID) {
        return false;
    }
    const uintptr_t psp = (uintptr_t)__get_PSP();
    return (SCB->CCR & SCB_CCR_STKALIGN_Msk) != 0U &&
           (psp & 7U) == 0U &&
           psp >= (uintptr_t)tasks[current_task].stack_low &&
           psp <= (uintptr_t)tasks[current_task].stack_high &&
           __get_MSP() != __get_PSP();
}

bool os_exception_configuration_valid(void)
{
    return (SCB->CCR & SCB_CCR_STKALIGN_Msk) != 0U &&
           NVIC_GetPriority(SVCall_IRQn) == (OS_PRIORITY_SVC >> 4U) &&
           NVIC_GetPriority(SysTick_IRQn) == (OS_PRIORITY_SYSTICK >> 4U) &&
           NVIC_GetPriority(PendSV_IRQn) == (OS_PRIORITY_PENDSV >> 4U) &&
           NVIC_GetPriority(SysTick_IRQn) < NVIC_GetPriority(PendSV_IRQn);
}

void os_svc_dispatch(uint32_t *exception_frame, uint32_t exc_return)
{
    const uint8_t service = decode_svc(exception_frame, exc_return);

    switch (service) {
    case OS_SVC_START:
        if (exc_return != OS_EXC_RETURN_THREAD_MSP ||
            current_task != OS_TASK_ID_INVALID || os_arch_has_active_context != 0U) {
            os_kernel_panic("SVC_START");
        }
        break;
    case OS_SVC_YIELD:
        if (exc_return != OS_EXC_RETURN_THREAD_PSP ||
            current_task == OS_TASK_ID_INVALID ||
            tasks[current_task].state != OS_TASK_RUNNING) {
            os_kernel_panic("SVC_YIELD");
        }
        transition_task(current_task, OS_TASK_RUNNING, OS_TASK_READY,
                        "YIELD_READY");
        yield_excluded_task = current_task;
        break;
    case OS_SVC_SLEEP: {
        if (exc_return != OS_EXC_RETURN_THREAD_PSP ||
            current_task == OS_TASK_ID_INVALID ||
            tasks[current_task].state != OS_TASK_RUNNING ||
            exception_frame[0] == 0U ||
            exception_frame[0] > OS_MAX_SLEEP_TICKS) {
            os_kernel_panic("SVC_SLEEP");
        }
        tasks[current_task].wake_tick = ticks + exception_frame[0];
        transition_task(current_task, OS_TASK_RUNNING, OS_TASK_SLEEPING,
                        "TASK_SLEEP");
        break;
    }
    case OS_SVC_EXIT:
        if (exc_return != OS_EXC_RETURN_THREAD_PSP ||
            current_task == OS_TASK_ID_INVALID ||
            current_task == OS_TASK_ID_IDLE ||
            tasks[current_task].state != OS_TASK_RUNNING) {
            os_kernel_panic("SVC_EXIT");
        }
        transition_task(current_task, OS_TASK_RUNNING, OS_TASK_EXITING,
                        "TASK_EXIT");
        break;
    default:
        os_kernel_panic("SVC_NUMBER");
    }

    request_pendsv();
}

uint32_t *os_pendsv_switch(uint32_t *saved_psp)
{
    const os_task_id_t outgoing = current_task;

    if (os_arch_has_active_context != 0U) {
        if (outgoing == OS_TASK_ID_INVALID ||
            !valid_saved_psp(outgoing, saved_psp)) {
            os_kernel_panic("SAVE_PSP");
        }
        tasks[outgoing].saved_psp = saved_psp;
    } else if (saved_psp != NULL || outgoing != OS_TASK_ID_INVALID) {
        os_kernel_panic("FIRST_PSP");
    }

    /* A direct preemption request can arrive while the outgoing task is still
       RUNNING. Commit that transition only here, with PendSV holding PRIMASK. */
    if (outgoing != OS_TASK_ID_INVALID &&
        tasks[outgoing].state == OS_TASK_RUNNING) {
        transition_task(outgoing, OS_TASK_RUNNING, OS_TASK_READY,
                        "PEND_READY");
    }

    if (outgoing != OS_TASK_ID_INVALID &&
        tasks[outgoing].state == OS_TASK_EXITING) {
        reclaim_exiting_task(outgoing);
    }

    os_task_id_t next = select_ready_task(yield_excluded_task);
    yield_excluded_task = OS_TASK_ID_INVALID;
    if (next == OS_TASK_ID_INVALID) {
        next = OS_TASK_ID_IDLE;
    }
    if (tasks[next].state != OS_TASK_READY ||
        !valid_saved_psp(next, tasks[next].saved_psp)) {
        os_kernel_panic("NEXT_TASK");
    }

    transition_task(next, OS_TASK_READY, OS_TASK_RUNNING, "SELECT_RUNNING");
    current_task = next;
    os_arch_has_active_context = 1U;
    verify_state("SWITCH");
    return tasks[next].saved_psp;
}

void os_kernel_tick(void)
{
    ++ticks;
    if (!running) {
        return;
    }

    bool awakened = false;
    for (os_task_id_t id = 1U; id < OS_TASK_COUNT; ++id) {
        if (tasks[id].state == OS_TASK_SLEEPING &&
            tick_reached(ticks, tasks[id].wake_tick)) {
            transition_task(id, OS_TASK_SLEEPING, OS_TASK_READY,
                            "WAKE_READY");
            awakened = true;
        }
    }

    if (awakened && current_task != OS_TASK_ID_INVALID &&
        tasks[current_task].state == OS_TASK_RUNNING) {
        const os_task_id_t candidate = select_ready_task(OS_TASK_ID_INVALID);
        if (candidate != OS_TASK_ID_INVALID &&
            task_outranks(candidate, current_task)) {
            transition_task(current_task, OS_TASK_RUNNING, OS_TASK_READY,
                            "PREEMPT_READY");
            request_pendsv();
        }
    }
}

void os_kernel_panic(const char *reason)
{
    board_kernel_panic(reason);
}

static void idle_entry(void *argument)
{
    (void)argument;
    board_lifecycle_idle_hook();
    for (;;) {
        __WFI();
    }
}

static void task_entry_trampoline(void *argument)
{
    if (current_task == OS_TASK_ID_INVALID ||
        tasks[current_task].state != OS_TASK_RUNNING ||
        tasks[current_task].entry == NULL || tasks[current_task].argument != argument) {
        os_kernel_panic("TRAMPOLINE");
    }
    tasks[current_task].entry(argument);
    os_task_exit();
}

static void task_return_guard(void)
{
    os_kernel_panic("TASK_RETURN");
}

static void reset_task(os_task_id_t id)
{
    tasks[id].entry = NULL;
    tasks[id].argument = NULL;
    tasks[id].saved_psp = NULL;
    tasks[id].stack_low = NULL;
    tasks[id].stack_high = NULL;
    tasks[id].deadline_ticks = 0U;
    tasks[id].wake_tick = 0U;
    tasks[id].id = id;
    tasks[id].state = OS_TASK_DORMANT;
    tasks[id].priority = 0U;
    tasks[id].stack_claimed = false;
}

static void transition_task(os_task_id_t id, os_task_state_t expected,
                            os_task_state_t next, const char *reason)
{
    if (id >= OS_TASK_COUNT || tasks[id].state != expected || expected == next) {
        os_kernel_panic(reason);
    }
    tasks[id].state = next;
}

static uint32_t *build_initial_frame(os_task_id_t id)
{
    os_tcb_t *const task = &tasks[id];
    if (task->stack_low == NULL || task->stack_high == NULL ||
        ((uintptr_t)task->stack_low & 7U) != 0U ||
        ((uintptr_t)task->stack_high & 7U) != 0U ||
        (size_t)((uint8_t *)task->stack_high - (uint8_t *)task->stack_low) <
            OS_INITIAL_FRAME_WORDS * sizeof(uint32_t)) {
        os_kernel_panic("STACK_LAYOUT");
    }

    uint32_t *const frame = task->stack_high - OS_INITIAL_FRAME_WORDS;
    for (size_t index = 0U; index < OS_INITIAL_FRAME_WORDS; ++index) {
        frame[index] = 0U;
    }
    /* R4-R11 occupy words 0-7. The hardware frame starts at word 8. */
    frame[8] = (uint32_t)(uintptr_t)task->argument;
    frame[13] = (uint32_t)(uintptr_t)task_return_guard;
    frame[14] = (uint32_t)(uintptr_t)task_entry_trampoline;
    frame[15] = OS_XPSR_THUMB;
    return frame;
}

static os_task_id_t select_ready_task(os_task_id_t excluded)
{
    os_task_id_t best = OS_TASK_ID_INVALID;
    for (os_task_id_t id = 1U; id < OS_TASK_COUNT; ++id) {
        if (id == excluded || tasks[id].state != OS_TASK_READY) {
            continue;
        }
        if (best == OS_TASK_ID_INVALID || task_outranks(id, best)) {
            best = id;
        }
    }
    if (best == OS_TASK_ID_INVALID && excluded != OS_TASK_ID_INVALID &&
        tasks[excluded].state == OS_TASK_READY) {
        best = excluded;
    }
    return best;
}

static bool tick_reached(uint32_t now, uint32_t target)
{
    return (int32_t)(now - target) >= 0;
}

static bool task_outranks(os_task_id_t candidate, os_task_id_t incumbent)
{
    if (incumbent == OS_TASK_ID_INVALID || incumbent == OS_TASK_ID_IDLE) {
        return candidate != OS_TASK_ID_IDLE;
    }
    if (candidate == OS_TASK_ID_IDLE) {
        return false;
    }
    if (tasks[candidate].deadline_ticks != tasks[incumbent].deadline_ticks) {
        return tasks[candidate].deadline_ticks < tasks[incumbent].deadline_ticks;
    }
    if (tasks[candidate].priority != tasks[incumbent].priority) {
        return tasks[candidate].priority < tasks[incumbent].priority;
    }
    return candidate < incumbent;
}

static void request_pendsv(void)
{
    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
    __DSB();
    __ISB();
}

static void verify_state(const char *where)
{
    uint32_t running_count = 0U;
    os_task_id_t running_id = OS_TASK_ID_INVALID;
    for (os_task_id_t id = 0U; id < OS_TASK_COUNT; ++id) {
        if (tasks[id].state == OS_TASK_RUNNING) {
            ++running_count;
            running_id = id;
        }
    }
    if ((!running && running_count != 0U) ||
        (running && os_arch_has_active_context != 0U &&
         (running_count != 1U || running_id != current_task))) {
        os_kernel_panic(where);
    }
}

static uint8_t decode_svc(const uint32_t *frame, uint32_t exc_return)
{
    if (!valid_svc_frame(frame, exc_return) ||
        (frame[7] & OS_XPSR_THUMB) == 0U || (frame[7] & 0x1FFU) != 0U) {
        os_kernel_panic("SVC_FRAME");
    }
    const uintptr_t return_pc = (uintptr_t)frame[6];
    if ((return_pc & 1U) != 0U || return_pc < OS_FLASH_START + 2U ||
        return_pc >= OS_FLASH_END) {
        os_kernel_panic("SVC_PC");
    }
    const uintptr_t instruction_address = return_pc - 2U;
    const uint16_t instruction = *(const volatile uint16_t *)instruction_address;
    if ((instruction & 0xFF00U) != 0xDF00U) {
        os_kernel_panic("SVC_OPCODE");
    }
    return (uint8_t)(instruction & 0x00FFU);
}

static bool valid_exc_return(uint32_t exc_return)
{
    return exc_return == OS_EXC_RETURN_THREAD_MSP ||
           exc_return == OS_EXC_RETURN_THREAD_PSP;
}

static bool valid_saved_psp(os_task_id_t id, const uint32_t *psp)
{
    if (id >= OS_TASK_COUNT || psp == NULL || tasks[id].stack_low == NULL ||
        tasks[id].stack_high == NULL) {
        return false;
    }
    const uintptr_t address = (uintptr_t)psp;
    const uintptr_t low = (uintptr_t)tasks[id].stack_low;
    const uintptr_t high = (uintptr_t)tasks[id].stack_high;
    const uintptr_t frame_bytes = OS_INITIAL_FRAME_WORDS * sizeof(uint32_t);
    return high >= low && (high - low) >= frame_bytes &&
           (address & 7U) == 0U && address >= low &&
           address <= high - frame_bytes;
}

static bool valid_svc_frame(const uint32_t *frame, uint32_t exc_return)
{
    extern uint32_t __msp_stack_limit__;
    extern uint32_t __msp_stack_top__;

    if (frame == NULL || ((uintptr_t)frame & 7U) != 0U ||
        !valid_exc_return(exc_return)) {
        return false;
    }
    const uintptr_t address = (uintptr_t)frame;
    const uintptr_t frame_end = address + (8U * sizeof(uint32_t));
    if (frame_end < address) {
        return false;
    }
    if (exc_return == OS_EXC_RETURN_THREAD_MSP) {
        return address >= (uintptr_t)&__msp_stack_limit__ &&
               frame_end <= (uintptr_t)&__msp_stack_top__;
    }
    return current_task != OS_TASK_ID_INVALID &&
           address >= (uintptr_t)tasks[current_task].stack_low &&
           frame_end <= (uintptr_t)tasks[current_task].stack_high;
}

static void reclaim_exiting_task(os_task_id_t id)
{
    if (id == OS_TASK_ID_IDLE || id >= OS_TASK_COUNT ||
        tasks[id].state != OS_TASK_EXITING || !tasks[id].stack_claimed) {
        os_kernel_panic("RECLAIM");
    }
    transition_task(id, OS_TASK_EXITING, OS_TASK_DORMANT, "RECLAIM_STATE");
    reset_task(id);
    last_reclaimed = id;
    ++reclaim_counter;
}
