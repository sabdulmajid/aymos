#include "main.h"
#include <stdio.h>
#include "common.h"
#include "k_task.h"
#include "k_mem.h"

volatile int counter = 0;
volatile int test_counter = 0;

void Task1(void *arg) {
    while(1) {
        printf("1\r\n");
        test_counter++;
        for (counter = 0; counter < 5000; counter++);
        osYield();
    }
}

void Task2(void *arg) {
    while(1) {
        printf("2\r\n");
        for (counter = 0; counter < 5000; counter++);
        osYield();
    }
}

void Task3(void *arg) {
    while(1) {
        printf("3\r\n");
        for (counter = 0; counter < 5000; counter++);
        osYield();
    }
}

int main(void) {
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_USART2_UART_Init();

    printf("Starting RTOS...\r\n");
    osKernelInit();

    TCB task;
    task.stack_size = STACK_SIZE;

    task.ptask = &Task1;
    if (osCreateTask(&task) != RTX_OK) {
        printf("Failed to create Task1\r\n");
    }

    task.ptask = &Task2;
    if (osCreateTask(&task) != RTX_OK) {
        printf("Failed to create Task2\r\n");
    }

    task.ptask = &Task3;
    if (osCreateTask(&task) != RTX_OK) {
        printf("Failed to create Task3\r\n");
    }

    printf("Starting kernel...\r\n");
    osKernelStart();

    printf("back to main\r\n");
    while (1);
}
