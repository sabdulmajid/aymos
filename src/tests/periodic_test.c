#include "main.h"
#include <stdio.h>
#include "common.h"
#include "k_task.h"
#include "k_mem.h"

int counter1 = 0;
int counter2 = 0;

void TaskA(void *arg) {
    while(1) {
        printf("%d, %d\r\n", counter1, counter2);
        osPeriodYield();
    }
}

void TaskB(void *arg) {
    while(1) {
        counter1 = counter1 + 1;
        osPeriodYield();
    }
}

void TaskC(void *arg) {
    while(1) {
        counter2 = counter2 + 1;
        osPeriodYield();
    }
}

int main(void) {
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_USART2_UART_Init();
    printf("STARTING RTOS \r\n");
    osKernelInit();

    TCB task;
    task.stack_size = STACK_SIZE;
    task.ptask = &TaskA;
    osCreateDeadlineTask(4, &task);

    task.ptask = &TaskB;
    osCreateDeadlineTask(4, &task);

    task.ptask = &TaskC;
    osCreateDeadlineTask(12, &task);

    osKernelStart();

    printf("back to main\r\n");
    while (1);
}
