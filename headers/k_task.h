#ifndef INC_K_TASK_H_
#define INC_K_TASK_H_
#include "stm32f4xx_hal.h"
/** Request privileged execution mode. */
#define EnablePrivilegedMode() __asm("SVC #0")
/** Start the first user thread. */
#define StartThread() __asm("SVC #1")
extern uint32_t* stackptr;

#define SHPR2 *(uint32_t*)0xE000ED1C
#define SHPR3 *(uint32_t*)0xE000ED20

typedef unsigned int U32;
typedef unsigned short U16;
typedef char U8;
typedef unsigned int task_t;

/**
 * @brief Task Control Block used by the scheduler.
 */
typedef struct task_control_block{
    void (*ptask)(void* args);       /**< Entry point of the task             */
    uint32_t* stack_high;            /**< Pointer to the top of the stack      */
    task_t tid;                      /**< Unique task identifier               */
    U8 state;                        /**< Current task state                   */
    U16 stack_size;                  /**< Size of the allocated stack          */
    U8 priority;                     /**< Static priority used as tie breaker  */
    uint32_t* thread_psp_ptr;        /**< Saved process stack pointer          */
    uint32_t deadline;               /**< Deadline in milliseconds             */
    uint32_t time_remaining;         /**< Remaining time before the deadline   */
} TCB;

/** Initialise internal data structures and the idle task. */
void osKernelInit(void);

/** Create a task using a default deadline. */
int osCreateTask(TCB* task);

/** Start task scheduling. */
int osKernelStart(void);

/** Yield control to the scheduler. */
void osYield(void);

/** Retrieve a copy of the TCB for the task with identifier @p TID. */
int osTaskInfo(task_t TID, TCB* task_copy);

/** Terminate the calling task. */
int osTaskExit(void);

/** Put the current task to sleep for @p timeInMs milliseconds. */
void osSleep(int timeInMs);

/** Yield the processor until the current task's next period. */
void osPeriodYield(void);

/** Set a new deadline for the task identified by @p TID. */
int osSetDeadline(int deadline, task_t TID);

/** Create a task with an explicit @p deadline. */
int osCreateDeadlineTask(int deadline, TCB* task);

/** Change the priority of the task identified by @p TID. */
int osSetPriority(U8 priority, task_t TID);

#endif /* INC_K_TASK_H_ */
