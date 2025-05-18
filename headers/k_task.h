#ifndef INC_K_TASK_H_
#define INC_K_TASK_H_
#include "stm32f4xx_hal.h"
#define EnablePrivilegedMode() __asm("SVC #0")
#define StartThread() __asm("SVC #1")
extern uint32_t* stackptr;

#define SHPR2 *(uint32_t*)0xE000ED1C
#define SHPR3 *(uint32_t*)0xE000ED20

typedef unsigned int U32;
typedef unsigned short U16;
typedef char U8;
typedef unsigned int task_t;

typedef struct task_control_block{
    void (*ptask)(void* args);
    uint32_t* stack_high;
    task_t tid;
    U8 state;
    U16 stack_size;
    U8 priority;
    uint32_t* thread_psp_ptr;
    uint32_t deadline;
    uint32_t time_remaining;
}TCB;

// Kernel Functions - Lab 1
void osKernelInit(void);
int osCreateTask(TCB* task);
int osKernelStart(void);
void osYield(void);
int osTaskInfo(task_t TID, TCB* task_copy);
int osTaskExit(void);

void osSleep(int timeInMs);
void osPeriodYield(void);
int osSetDeadline(int deadline, task_t TID);
int osCreateDeadlineTask(int deadline, TCB* task);
int osSetPriority(U8 priority, task_t TID);

#endif /* INC_K_TASK_H_ */
