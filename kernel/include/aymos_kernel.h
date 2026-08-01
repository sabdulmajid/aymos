#ifndef AYMOS_KERNEL_H
#define AYMOS_KERNEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t os_task_id_t;

enum {
    OS_TASK_ID_IDLE = 0U,
    OS_TASK_ID_INVALID = UINT8_MAX,
    OS_MAX_USER_TASKS = 4U,
    OS_MIN_STACK_SIZE = 256U,
    OS_MAX_STACK_SIZE = 1024U,
    OS_MAX_SLEEP_TICKS = INT32_MAX
};

typedef enum {
    OS_TASK_DORMANT = 0,
    OS_TASK_READY,
    OS_TASK_RUNNING,
    OS_TASK_SLEEPING,
    OS_TASK_EXITING
} os_task_state_t;

typedef void (*os_task_entry_t)(void *argument);

typedef struct {
    os_task_entry_t entry;
    void *argument;
    size_t stack_size;
    uint32_t deadline_ticks;
    uint8_t priority;
} os_task_config_t;

typedef struct {
    os_task_id_t id;
    os_task_state_t state;
    uint32_t deadline_ticks;
    uint8_t priority;
} os_task_info_t;

int os_kernel_init(void);
int os_task_create(const os_task_config_t *config, os_task_id_t *created_id);
void os_kernel_start(void) __attribute__((noreturn));
void os_yield(void);
int os_sleep(uint32_t ticks);
void os_task_exit(void) __attribute__((noreturn));

uint32_t os_tick_count(void);
os_task_id_t os_current_task(void);
os_task_id_t os_last_reclaimed_task(void);
uint32_t os_reclaim_count(void);
int os_task_info(os_task_id_t id, os_task_info_t *info);
bool os_thread_uses_psp(void);
bool os_exception_configuration_valid(void);

/* Architecture entry points. Application code must not call these directly. */
void os_svc_dispatch(uint32_t *exception_frame, uint32_t exc_return);
uint32_t *os_pendsv_switch(uint32_t *saved_psp);
void os_kernel_tick(void);
void os_kernel_panic(const char *reason) __attribute__((noreturn));

extern volatile uint32_t os_arch_has_active_context;

#endif
