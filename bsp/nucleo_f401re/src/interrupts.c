#include "board.h"

#include <stdbool.h>

void NMI_Handler(void);
void HardFault_Handler(void);
void MemManage_Handler(void);
void BusFault_Handler(void);
void UsageFault_Handler(void);
void DebugMon_Handler(void);
void SVC_Handler(void);
void SysTick_Handler(void);

static void fault_stop(void) __attribute__((noreturn));

static volatile bool systick_observed;
static volatile bool svc_observed;

void NMI_Handler(void)
{
    fault_stop();
}

void HardFault_Handler(void)
{
    fault_stop();
}

void MemManage_Handler(void)
{
    fault_stop();
}

void BusFault_Handler(void)
{
    fault_stop();
}

void UsageFault_Handler(void)
{
    fault_stop();
}

void DebugMon_Handler(void)
{
}

void SVC_Handler(void)
{
    svc_observed = true;
}

void SysTick_Handler(void)
{
    HAL_IncTick();
    systick_observed = true;
}

bool board_systick_observed(void)
{
    return systick_observed;
}

bool board_svc_observed(void)
{
    return svc_observed;
}

void board_invoke_svc_smoke(void)
{
    __asm volatile("svc #0" ::: "memory");
}

static void fault_stop(void)
{
    __disable_irq();
    for (;;) {
        __NOP();
    }
}
