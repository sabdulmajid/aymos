#include "stm32f4xx_hal.h"

void NMI_Handler(void);
void HardFault_Handler(void);
void MemManage_Handler(void);
void BusFault_Handler(void);
void UsageFault_Handler(void);
void DebugMon_Handler(void);
void SysTick_Handler(void);

static void fault_stop(void) __attribute__((noreturn));

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

void SysTick_Handler(void)
{
    HAL_IncTick();
}

static void fault_stop(void)
{
    __disable_irq();
    for (;;) {
        __NOP();
    }
}
