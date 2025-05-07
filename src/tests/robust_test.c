#include "common.h"
#include "k_task.h"
#include "main.h"
#include <stdio.h>

int counter = 0;

void Task1(void *arg) {
    counter++;
    osYield();

    TCB task;
    task.ptask = &Task1;
    task.stack_size = 0x400;
    osCreateTask(&task);
    osTaskExit();
}

void Task2(void *arg) {
    while(1) {
        printf("Back to you %d\r\n", counter);
        osYield();
    }
}

int main(void) {
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_USART2_UART_Init();

    osKernelInit();

    TCB task;
    task.stack_size = 0x400;

    task.ptask = &Task1;
    osCreateTask(&task);

    task.ptask = &Task2;
    osCreateTask(&task);

    osKernelStart();

    while (1);
}
