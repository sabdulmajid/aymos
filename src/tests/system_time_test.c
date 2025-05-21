#include "main.h"
#include <stdio.h>
#include "common.h"
#include "k_task.h"
#include "k_mem.h"

void TickPrinter(void *arg) {
    while (1) {
        printf("time=%lu, mem=%u\r\n", osGetSystemTime(), k_mem_get_usage());
        osSleep(1);
    }
}

int main(void) {
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_USART2_UART_Init();

    osKernelInit();

    TCB task;
    task.stack_size = STACK_SIZE;
    task.ptask = &TickPrinter;
    osCreateTask(&task);

    osKernelStart();

    while (1);
}
