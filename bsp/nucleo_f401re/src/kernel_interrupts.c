#include "aymos_kernel.h"

#include "board.h"

void NMI_Handler(void);
void HardFault_Handler(void);
void MemManage_Handler(void);
void BusFault_Handler(void);
void UsageFault_Handler(void);
void DebugMon_Handler(void);
void SysTick_Handler(void);

void NMI_Handler(void)
{
    board_kernel_panic("NMI");
}

void HardFault_Handler(void)
{
    board_kernel_panic("HARDFAULT");
}

void MemManage_Handler(void)
{
    board_kernel_panic("MEMFAULT");
}

void BusFault_Handler(void)
{
    board_kernel_panic("BUSFAULT");
}

void UsageFault_Handler(void)
{
    board_kernel_panic("USAGEFAULT");
}

void DebugMon_Handler(void)
{
}

void SysTick_Handler(void)
{
    HAL_IncTick();
    os_kernel_tick();
}
