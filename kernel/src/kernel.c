#include "aymos_kernel.h"
#include "aymos_trace_runtime.h"

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
    OS_SVC_WAIT_NEXT_PERIOD = 4U,
    OS_PRIORITY_SVC = 0x80U,
    OS_PRIORITY_SYSTICK = 0xE0U,
    OS_PRIORITY_PENDSV = 0xF0U,
    OS_SWITCH_START = 1U,
    OS_SWITCH_YIELD = 2U,
    OS_SWITCH_SLEEP = 3U,
    OS_SWITCH_EXIT = 4U,
    OS_SWITCH_WAIT_PERIOD = 5U,
    OS_SWITCH_SYSTICK_PREEMPT = 6U,
    OS_SWITCH_RUNTIME_CREATE = 7U,
    OS_SWITCH_COALESCED_PREEMPT = 8U,
    OS_PREEMPT_SOURCE_SYSTICK = 1U,
    OS_PREEMPT_SOURCE_RUNTIME_CREATE = 2U,
    OS_FLASH_START = 0x08000000U,
    OS_FLASH_END = 0x08080000U
};

typedef struct {
    os_task_entry_t entry;
    void *argument;
    uint32_t *saved_psp;
    uint32_t *stack_low;
    uint32_t *stack_high;
    bool stack_claimed;
    bool started;
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
static os_sched_task_t schedules[OS_TASK_COUNT];
static os_stack_slot_t stacks[OS_TASK_COUNT];
static os_task_id_t current_task = OS_TASK_ID_INVALID;
static os_task_id_t yield_excluded_task = OS_TASK_ID_INVALID;
static uint32_t pending_switch_cause;
static uint32_t pending_preempt_sources;
static volatile uint64_t monotonic_ticks;
static volatile uint32_t reclaim_counter;
static volatile os_task_id_t last_reclaimed = OS_TASK_ID_INVALID;
static os_allocator_t memory_allocator;
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
static void request_pendsv(void);
static void verify_state(const char *where);
static uint8_t decode_svc(const uint32_t *frame, uint32_t exc_return);
static bool valid_exc_return(uint32_t exc_return);
static bool valid_saved_psp(os_task_id_t id, const uint32_t *psp);
static bool valid_svc_frame(const uint32_t *frame, uint32_t exc_return);
static void reclaim_exiting_task(os_task_id_t id);
static bool caller_is_running_task(void);
static void trace_deadline(os_trace_event_t event, os_task_id_t id);
static uint16_t trace_task(os_task_id_t id);
static uint32_t trace_pointer_offset(const void *pointer);
static void record_preemption_request(uint32_t source);

int os_kernel_init(void)
{
    extern uint8_t __aymos_heap_end__;
    extern uint8_t __aymos_heap_start__;

    if (initialized || running) {
        return 0;
    }

    for (os_task_id_t id = 0U; id < OS_TASK_COUNT; ++id) {
        reset_task(id);
    }

    tasks[OS_TASK_ID_IDLE].entry = idle_entry;
    tasks[OS_TASK_ID_IDLE].argument = NULL;
    transition_task(OS_TASK_ID_IDLE, OS_TASK_DORMANT, OS_TASK_READY,
                    "IDLE_READY");
    tasks[OS_TASK_ID_IDLE].stack_claimed = true;
    tasks[OS_TASK_ID_IDLE].stack_low = stacks[OS_TASK_ID_IDLE].words;
    tasks[OS_TASK_ID_IDLE].stack_high =
        stacks[OS_TASK_ID_IDLE].words +
        (OS_MAX_STACK_SIZE / sizeof(uint32_t));
    tasks[OS_TASK_ID_IDLE].saved_psp = build_initial_frame(OS_TASK_ID_IDLE);

    monotonic_ticks = 0U;
    reclaim_counter = 0U;
    last_reclaimed = OS_TASK_ID_INVALID;
    current_task = OS_TASK_ID_INVALID;
    yield_excluded_task = OS_TASK_ID_INVALID;
    pending_switch_cause = 0U;
    pending_preempt_sources = 0U;
    os_arch_has_active_context = 0U;
    os_trace_runtime_init();

    const uintptr_t heap_begin = (uintptr_t)&__aymos_heap_start__;
    const uintptr_t heap_end = (uintptr_t)&__aymos_heap_end__;
    if (heap_end <= heap_begin ||
        !os_allocator_init(&memory_allocator, (void *)heap_begin,
                           (size_t)(heap_end - heap_begin))) {
        os_kernel_panic("HEAP_INIT");
    }

    SCB->CCR |= SCB_CCR_STKALIGN_Msk;
    HAL_NVIC_SetPriority(SVCall_IRQn, OS_PRIORITY_SVC >> 4U, 0U);
    HAL_NVIC_SetPriority(SysTick_IRQn, OS_PRIORITY_SYSTICK >> 4U, 0U);
    HAL_NVIC_SetPriority(PendSV_IRQn, OS_PRIORITY_PENDSV >> 4U, 0U);

    initialized = true;
    verify_state("INIT");
    return 1;
}

os_task_config_t os_task_config_default(os_task_entry_t entry, void *argument)
{
    const os_task_config_t config = {
        .entry = entry,
        .argument = argument,
        .stack_size = OS_MAX_STACK_SIZE,
        .timing = {
            .kind = OS_TASK_ONE_SHOT,
            .initial_release_delay_ticks = 0U,
            .period_ticks = 0U,
            .relative_deadline_ticks = 1U,
            .execution_budget_ticks = 0U,
            .priority = 128U,
        },
    };
    return config;
}

int os_task_create(const os_task_config_t *config, os_task_id_t *created_id)
{
    if (!initialized || config == NULL || created_id == NULL ||
        config->entry == NULL || config->stack_size < OS_MIN_STACK_SIZE ||
        config->stack_size > OS_MAX_STACK_SIZE ||
        (config->stack_size & 7U) != 0U ||
        !os_sched_config_valid(&config->timing) || __get_IPSR() != 0U ||
        (running &&
         (!caller_is_running_task() || current_task == OS_TASK_ID_IDLE))) {
        return 0;
    }

    const uint32_t saved_primask = __get_PRIMASK();
    __disable_irq();
    if (running &&
        (!caller_is_running_task() || current_task == OS_TASK_ID_IDLE)) {
        __set_PRIMASK(saved_primask);
        return 0;
    }

    os_task_id_t id = OS_TASK_ID_INVALID;
    for (os_task_id_t candidate = 1U; candidate < OS_TASK_COUNT; ++candidate) {
        if (schedules[candidate].state == OS_TASK_DORMANT &&
            !tasks[candidate].stack_claimed) {
            id = candidate;
            break;
        }
    }
    if (id == OS_TASK_ID_INVALID) {
        __set_PRIMASK(saved_primask);
        return 0;
    }

    tasks[id].entry = config->entry;
    tasks[id].argument = config->argument;
    tasks[id].stack_claimed = true;
    tasks[id].stack_low = stacks[id].words;
    tasks[id].stack_high =
        stacks[id].words + (config->stack_size / sizeof(uint32_t));
    if (!os_sched_task_configure(&schedules[id], &config->timing,
                                 monotonic_ticks)) {
        reset_task(id);
        __set_PRIMASK(saved_primask);
        return 0;
    }
    tasks[id].saved_psp = build_initial_frame(id);
    *created_id = id;

    const os_task_id_t creator = running ? current_task : OS_TASK_ID_INVALID;
    (void)os_trace_emit(OS_TRACE_TASK_CREATE, id, trace_task(creator),
                        (uint32_t)schedules[id].kind,
                        schedules[id].priority,
                        (uint32_t)schedules[id].state);
    if (schedules[id].state == OS_TASK_READY) {
        trace_deadline(OS_TRACE_TASK_RELEASE, id);
    }

    if (running && schedules[id].state == OS_TASK_READY &&
        os_sched_outranks(&schedules[id], &schedules[current_task])) {
        os_trace_emit_selection(schedules, OS_TASK_COUNT, id, current_task,
                                OS_TASK_ID_INVALID,
                                OS_TRACE_SELECT_RUNTIME_CREATE_PROBE);
        record_preemption_request(OS_PREEMPT_SOURCE_RUNTIME_CREATE);
        request_pendsv();
    }

    verify_state("CREATE");
    __set_PRIMASK(saved_primask);
    return 1;
}

void os_kernel_start(void)
{
    if (!initialized || running) {
        os_kernel_panic("START_STATE");
    }
    running = true;
    pending_switch_cause = OS_SWITCH_START;
    (void)os_trace_emit(OS_TRACE_KERNEL_START, OS_TRACE_TASK_INVALID,
                        OS_TRACE_TASK_INVALID, 0U, OS_TASK_COUNT, 0U);
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

int os_wait_next_period(void)
{
    if (!running || current_task == OS_TASK_ID_INVALID ||
        schedules[current_task].kind != OS_TASK_PERIODIC) {
        return 0;
    }
    __asm volatile("svc #4" ::: "memory");
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
    return (uint32_t)os_monotonic_tick_count();
}

uint64_t os_monotonic_tick_count(void)
{
    const uint32_t saved_primask = __get_PRIMASK();
    __disable_irq();
    const uint64_t now = monotonic_ticks;
    __set_PRIMASK(saved_primask);
    return now;
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
    if (schedules[id].state == OS_TASK_DORMANT) {
        __set_PRIMASK(saved_primask);
        return 0;
    }
    info->id = id;
    info->state = schedules[id].state;
    info->kind = schedules[id].kind;
    info->release_order = schedules[id].release_order;
    info->next_release_order = schedules[id].next_release_order;
    info->wake_order = schedules[id].wake_order;
    info->absolute_deadline_order = schedules[id].absolute_deadline_order;
    info->release_tick = schedules[id].release_tick;
    info->next_release_tick = schedules[id].next_release_tick;
    info->wake_tick = schedules[id].wake_tick;
    info->period_ticks = schedules[id].period_ticks;
    info->relative_deadline_ticks = schedules[id].relative_deadline_ticks;
    info->absolute_deadline_tick = schedules[id].absolute_deadline_tick;
    info->execution_budget_ticks = schedules[id].execution_budget_ticks;
    info->total_execution_ticks = schedules[id].total_execution_ticks;
    info->job_execution_ticks = schedules[id].job_execution_ticks;
    info->completed_job_count = schedules[id].completed_job_count;
    info->deadline_miss_count = schedules[id].deadline_miss_count;
    info->missed_release_count = schedules[id].missed_release_count;
    info->job_sequence = schedules[id].job_sequence;
    info->priority = schedules[id].priority;
    info->job_active = schedules[id].job_active;
    __set_PRIMASK(saved_primask);
    return 1;
}

void *os_memory_alloc(size_t size)
{
    if (__get_IPSR() != 0U || !caller_is_running_task() ||
        current_task == OS_TASK_ID_IDLE) {
        return NULL;
    }
    const uint32_t saved_primask = __get_PRIMASK();
    __disable_irq();
    void *result = NULL;
    if (caller_is_running_task() && current_task != OS_TASK_ID_IDLE) {
        result = os_allocator_alloc(&memory_allocator, size,
                                    (os_memory_owner_t)current_task);
        (void)os_trace_emit(OS_TRACE_ALLOC, current_task,
                            OS_TRACE_TASK_INVALID, (uint32_t)size,
                            trace_pointer_offset(result),
                            result != NULL ? 1U : 0U);
    }
    __set_PRIMASK(saved_primask);
    return result;
}

int os_memory_free(void *pointer)
{
    if (__get_IPSR() != 0U || !caller_is_running_task() ||
        current_task == OS_TASK_ID_IDLE) {
        return 0;
    }
    const uint32_t saved_primask = __get_PRIMASK();
    __disable_irq();
    bool result = false;
    if (caller_is_running_task() && current_task != OS_TASK_ID_IDLE) {
        const uint32_t pointer_offset = trace_pointer_offset(pointer);
        result = os_allocator_free(&memory_allocator, pointer,
                                   (os_memory_owner_t)current_task);
        (void)os_trace_emit(OS_TRACE_FREE, current_task,
                            OS_TRACE_TASK_INVALID, pointer_offset, 1U,
                            result ? 1U : 0U);
    }
    __set_PRIMASK(saved_primask);
    return result ? 1 : 0;
}

int os_memory_stats(os_memory_stats_t *stats)
{
    if (!initialized || __get_IPSR() != 0U || stats == NULL) {
        return 0;
    }
    const uint32_t saved_primask = __get_PRIMASK();
    __disable_irq();
    const bool result = os_allocator_get_stats(&memory_allocator, stats);
    __set_PRIMASK(saved_primask);
    return result ? 1 : 0;
}

size_t os_memory_count_fragments(size_t requested_size)
{
    if (!initialized || __get_IPSR() != 0U) {
        return 0U;
    }
    const uint32_t saved_primask = __get_PRIMASK();
    __disable_irq();
    const size_t result =
        os_allocator_count_fragments(&memory_allocator, requested_size);
    __set_PRIMASK(saved_primask);
    return result;
}

bool os_memory_validate(void)
{
    if (!initialized || __get_IPSR() != 0U) {
        return false;
    }
    const uint32_t saved_primask = __get_PRIMASK();
    __disable_irq();
    const bool result = os_allocator_validate(&memory_allocator);
    __set_PRIMASK(saved_primask);
    return result;
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
            schedules[current_task].state != OS_TASK_RUNNING) {
            os_kernel_panic("SVC_YIELD");
        }
        transition_task(current_task, OS_TASK_RUNNING, OS_TASK_READY,
                        "YIELD_READY");
        yield_excluded_task = current_task;
        pending_switch_cause = OS_SWITCH_YIELD;
        (void)os_trace_emit(OS_TRACE_TASK_YIELD, current_task,
                            OS_TRACE_TASK_INVALID, 0U, 0U, 0U);
        break;
    case OS_SVC_SLEEP: {
        if (exc_return != OS_EXC_RETURN_THREAD_PSP ||
            current_task == OS_TASK_ID_INVALID ||
            schedules[current_task].state != OS_TASK_RUNNING ||
            exception_frame[0] == 0U ||
            exception_frame[0] > OS_MAX_SLEEP_TICKS) {
            os_kernel_panic("SVC_SLEEP");
        }
        if (!os_sched_sleep(&schedules[current_task], monotonic_ticks,
                            exception_frame[0])) {
            os_kernel_panic("TASK_SLEEP");
        }
        (void)os_trace_emit(
            OS_TRACE_TASK_SLEEP, current_task, OS_TRACE_TASK_INVALID,
            (uint32_t)schedules[current_task].wake_order,
            (uint32_t)(schedules[current_task].wake_order >> 32U),
            exception_frame[0]);
        pending_switch_cause = OS_SWITCH_SLEEP;
        break;
    }
    case OS_SVC_EXIT: {
        if (exc_return != OS_EXC_RETURN_THREAD_PSP ||
            current_task == OS_TASK_ID_INVALID ||
            current_task == OS_TASK_ID_IDLE ||
            schedules[current_task].state != OS_TASK_RUNNING) {
            os_kernel_panic("SVC_EXIT");
        }
        const bool miss_before = schedules[current_task].deadline_miss_latched;
        if (!os_sched_complete_job(&schedules[current_task], monotonic_ticks)) {
            os_kernel_panic("EXIT_COMPLETE");
        }
        if (!miss_before && schedules[current_task].deadline_miss_latched) {
            trace_deadline(OS_TRACE_DEADLINE_MISS, current_task);
        } else if (!schedules[current_task].deadline_miss_latched) {
            trace_deadline(OS_TRACE_DEADLINE_MET, current_task);
        }
        transition_task(current_task, OS_TASK_RUNNING, OS_TASK_EXITING,
                        "TASK_EXIT");
        (void)os_trace_emit(OS_TRACE_TASK_EXIT, current_task,
                            OS_TRACE_TASK_INVALID,
                            schedules[current_task].completed_job_count,
                            schedules[current_task].deadline_miss_count, 0U);
        pending_switch_cause = OS_SWITCH_EXIT;
        break;
    }
    case OS_SVC_WAIT_NEXT_PERIOD:
        if (exc_return != OS_EXC_RETURN_THREAD_PSP ||
            current_task == OS_TASK_ID_INVALID ||
            current_task == OS_TASK_ID_IDLE ||
            schedules[current_task].state != OS_TASK_RUNNING) {
            os_kernel_panic("SVC_PERIOD");
        }
        {
            const bool miss_before =
                schedules[current_task].deadline_miss_latched;
            if (!os_sched_wait_next_period(&schedules[current_task],
                                           monotonic_ticks)) {
                os_kernel_panic("SVC_PERIOD");
            }
            if (!miss_before && schedules[current_task].deadline_miss_latched) {
                trace_deadline(OS_TRACE_DEADLINE_MISS, current_task);
            } else if (!schedules[current_task].deadline_miss_latched) {
                trace_deadline(OS_TRACE_DEADLINE_MET, current_task);
            }
            (void)os_trace_emit(
                OS_TRACE_TASK_WAIT_PERIOD, current_task,
                OS_TRACE_TASK_INVALID,
                (uint32_t)schedules[current_task].next_release_order,
                (uint32_t)(schedules[current_task].next_release_order >> 32U),
                0U);
            pending_switch_cause = OS_SWITCH_WAIT_PERIOD;
        }
        break;
    default:
        os_kernel_panic("SVC_NUMBER");
    }

    request_pendsv();
}

uint32_t *os_pendsv_switch(uint32_t *saved_psp)
{
    const os_task_id_t outgoing = current_task;
    const uint32_t preempt_sources = pending_preempt_sources;
    os_task_state_t outgoing_state = OS_TASK_DORMANT;
    if (outgoing != OS_TASK_ID_INVALID) {
        outgoing_state = schedules[outgoing].state;
    }

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
        schedules[outgoing].state == OS_TASK_RUNNING) {
        transition_task(outgoing, OS_TASK_RUNNING, OS_TASK_READY,
                        "PEND_READY");
    }

    if (outgoing != OS_TASK_ID_INVALID &&
        schedules[outgoing].state == OS_TASK_EXITING) {
        reclaim_exiting_task(outgoing);
    }

    const os_task_id_t excluded = yield_excluded_task;
    os_task_id_t next =
        os_sched_select(schedules, OS_TASK_COUNT, excluded);
    yield_excluded_task = OS_TASK_ID_INVALID;
    if (next == OS_TASK_ID_INVALID) {
        next = OS_TASK_ID_IDLE;
    }
    if (schedules[next].state != OS_TASK_READY ||
        !valid_saved_psp(next, tasks[next].saved_psp)) {
        os_kernel_panic("NEXT_TASK");
    }

    os_trace_emit_selection(schedules, OS_TASK_COUNT, next, outgoing,
                            excluded, OS_TRACE_SELECT_DISPATCH);
    transition_task(next, OS_TASK_READY, OS_TASK_RUNNING, "SELECT_RUNNING");
    current_task = next;
    os_arch_has_active_context = 1U;
    if (outgoing == OS_TASK_ID_IDLE && next != OS_TASK_ID_IDLE) {
        (void)os_trace_emit(OS_TRACE_IDLE_STOP, OS_TASK_ID_IDLE, next,
                            0U, 0U, 0U);
    }
    if (next == OS_TASK_ID_IDLE && outgoing != OS_TASK_ID_IDLE) {
        (void)os_trace_emit(OS_TRACE_IDLE_START, OS_TASK_ID_IDLE,
                            trace_task(outgoing), 0U, 0U, 0U);
    }
    if (preempt_sources != 0U) {
        if (outgoing == OS_TASK_ID_INVALID || outgoing == next) {
            os_kernel_panic("PREEMPT_COMMIT");
        }
        (void)os_trace_emit(OS_TRACE_TASK_PREEMPT, outgoing, next,
                            preempt_sources, 0U, 0U);
    }
    (void)os_trace_emit(OS_TRACE_CONTEXT_SWITCH, trace_task(outgoing), next,
                        pending_switch_cause, (uint32_t)outgoing_state, 0U);
    pending_switch_cause = 0U;
    pending_preempt_sources = 0U;
    if (next != OS_TASK_ID_IDLE && !tasks[next].started) {
        tasks[next].started = true;
        trace_deadline(OS_TRACE_TASK_START, next);
    }
    verify_state("SWITCH");
    return tasks[next].saved_psp;
}

void os_kernel_tick(void)
{
    if (!running) {
        return;
    }
    if (monotonic_ticks == UINT64_MAX) {
        os_kernel_panic("TICK_RANGE");
    }
    ++monotonic_ticks;

    bool made_ready = false;
    for (os_task_id_t id = 1U; id < OS_TASK_COUNT; ++id) {
        os_sched_account_running_tick(&schedules[id]);
        if (os_sched_record_deadline_miss(&schedules[id], monotonic_ticks)) {
            trace_deadline(OS_TRACE_DEADLINE_MISS, id);
        }
        if (os_sched_record_active_release(&schedules[id], monotonic_ticks)) {
            const uint64_t missed_release =
                schedules[id].next_release_order - schedules[id].period_ticks;
            (void)os_trace_emit(
                OS_TRACE_TASK_RELEASE_SKIPPED, id, OS_TRACE_TASK_INVALID,
                (uint32_t)missed_release,
                (uint32_t)(missed_release >> 32U),
                schedules[id].missed_release_count);
        }
        if (schedules[id].time_range_exhausted) {
            os_kernel_panic("TASK_TIME_RANGE");
        }
        if (os_sched_release_due(&schedules[id], monotonic_ticks)) {
            made_ready = true;
            trace_deadline(OS_TRACE_TASK_RELEASE, id);
        }
        if (schedules[id].time_range_exhausted) {
            os_kernel_panic("TASK_TIME_RANGE");
        }
        if (os_sched_wake_due(&schedules[id], monotonic_ticks)) {
            made_ready = true;
            trace_deadline(OS_TRACE_TASK_WAKE, id);
        }
    }

    if (made_ready && current_task != OS_TASK_ID_INVALID &&
        schedules[current_task].state == OS_TASK_RUNNING) {
        const os_task_id_t candidate =
            os_sched_select(schedules, OS_TASK_COUNT, OS_TASK_ID_INVALID);
        if (candidate != OS_TASK_ID_INVALID && candidate != OS_TASK_ID_IDLE &&
            os_sched_outranks(&schedules[candidate],
                              &schedules[current_task])) {
            os_trace_emit_selection(schedules, OS_TASK_COUNT, candidate,
                                    current_task, OS_TASK_ID_INVALID,
                                    OS_TRACE_SELECT_SYSTICK_PROBE);
            record_preemption_request(OS_PREEMPT_SOURCE_SYSTICK);
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
#if defined(AYMOS_TRACE_ENABLED)
    for (;;) {
        board_lifecycle_idle_hook();
        __WFI();
    }
#else
    board_lifecycle_idle_hook();
    for (;;) {
        __WFI();
    }
#endif
}

static void task_entry_trampoline(void *argument)
{
    if (current_task == OS_TASK_ID_INVALID ||
        schedules[current_task].state != OS_TASK_RUNNING ||
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
    tasks[id].stack_claimed = false;
    tasks[id].started = false;
    os_sched_task_reset(&schedules[id], id);
}

static void transition_task(os_task_id_t id, os_task_state_t expected,
                            os_task_state_t next, const char *reason)
{
    if (id >= OS_TASK_COUNT ||
        !os_sched_transition(&schedules[id], expected, next)) {
        os_kernel_panic(reason);
    }
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
        if (schedules[id].state == OS_TASK_RUNNING) {
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
        schedules[id].state != OS_TASK_EXITING || !tasks[id].stack_claimed) {
        os_kernel_panic("RECLAIM");
    }
    if (!os_allocator_validate(&memory_allocator)) {
        os_kernel_panic("HEAP_RECLAIM");
    }
    const size_t released = os_allocator_release_owner(
        &memory_allocator, (os_memory_owner_t)id);
    if (released != 0U) {
        (void)os_trace_emit(OS_TRACE_OWNER_RELEASE, id,
                            OS_TRACE_TASK_INVALID, (uint32_t)released,
                            (uint32_t)id, 1U);
    }
    if (!os_allocator_validate(&memory_allocator)) {
        os_kernel_panic("HEAP_RELEASE");
    }
    transition_task(id, OS_TASK_EXITING, OS_TASK_DORMANT, "RECLAIM_STATE");
    reset_task(id);
    last_reclaimed = id;
    ++reclaim_counter;
}

static bool caller_is_running_task(void)
{
    return running && current_task < OS_TASK_COUNT &&
           schedules[current_task].state == OS_TASK_RUNNING &&
           os_arch_has_active_context != 0U;
}

static void trace_deadline(os_trace_event_t event, os_task_id_t id)
{
    const uint64_t deadline = schedules[id].absolute_deadline_order;
    (void)os_trace_emit(event, id, OS_TRACE_TASK_INVALID,
                        (uint32_t)deadline, (uint32_t)(deadline >> 32U),
                        schedules[id].job_sequence);
}

static uint16_t trace_task(os_task_id_t id)
{
    return id == OS_TASK_ID_INVALID ? OS_TRACE_TASK_INVALID : id;
}

static uint32_t trace_pointer_offset(const void *pointer)
{
    extern uint8_t __aymos_heap_end__;
    extern uint8_t __aymos_heap_start__;
    if (pointer == NULL) {
        return UINT32_MAX;
    }
    const uintptr_t address = (uintptr_t)pointer;
    const uintptr_t start = (uintptr_t)&__aymos_heap_start__;
    const uintptr_t end = (uintptr_t)&__aymos_heap_end__;
    if (address < start || address >= end) {
        return UINT32_MAX;
    }
    return (uint32_t)(address - start);
}

static void record_preemption_request(uint32_t source)
{
    if (source != OS_PREEMPT_SOURCE_SYSTICK &&
        source != OS_PREEMPT_SOURCE_RUNTIME_CREATE) {
        os_kernel_panic("PREEMPT_SOURCE");
    }
    pending_preempt_sources |= source;
    switch (pending_preempt_sources) {
    case OS_PREEMPT_SOURCE_SYSTICK:
        pending_switch_cause = OS_SWITCH_SYSTICK_PREEMPT;
        break;
    case OS_PREEMPT_SOURCE_RUNTIME_CREATE:
        pending_switch_cause = OS_SWITCH_RUNTIME_CREATE;
        break;
    case OS_PREEMPT_SOURCE_SYSTICK | OS_PREEMPT_SOURCE_RUNTIME_CREATE:
        pending_switch_cause = OS_SWITCH_COALESCED_PREEMPT;
        break;
    default:
        os_kernel_panic("PREEMPT_MASK");
    }
}
