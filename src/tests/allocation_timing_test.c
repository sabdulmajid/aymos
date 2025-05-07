#include "main.h"
#include <stdio.h>
#include "common.h"
#include "k_task.h"
#include "k_mem.h"

#define ARM_CM_DEMCR      (*(uint32_t *)0xE000EDFC)
#define ARM_CM_DWT_CTRL   (*(uint32_t *)0xE0001000)
#define ARM_CM_DWT_CYCCNT (*(uint32_t *)0xE0001004)

int main(void) {
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_USART2_UART_Init();

    osKernelInit();
    k_mem_init();

    if (ARM_CM_DWT_CTRL != 0) {
        printf("Using DWT\r\n\r\n");
        ARM_CM_DEMCR      |= 1 << 24;
        ARM_CM_DWT_CYCCNT  = 0;
        ARM_CM_DWT_CTRL   |= 1 << 0;
    } else {
        printf("DWT not available \r\n\r\n");
    }

    int N = 100;
    volatile U32* p_temp;
    uint32_t timestamps[N+1];

    for (int i = 0; i < N; i++) {
        timestamps[i] = ARM_CM_DWT_CYCCNT;
        p_temp = (U32*)k_mem_alloc(4);
    }
    timestamps[N] = ARM_CM_DWT_CYCCNT;
    printf("k_mem_alloc time: %lu\r\n", timestamps[N] - timestamps[0]);
    printf("Time per iteration:\r\n");
    for (int i = 0; i < N; i++) {
        printf("%u, ", timestamps[i+1] - timestamps[i]);
    }
    printf("\r\n\r\n");

    for (int i = 0; i < N; i++) {
        timestamps[i] = ARM_CM_DWT_CYCCNT;
        k_mem_dealloc(p_temp);
    }

    timestamps[N] = ARM_CM_DWT_CYCCNT;
    printf("k_mem_dealloc time: %lu\r\n", timestamps[N] - timestamps[0]);
    printf("Time per iteration (deallocation):\r\n");
    for (int i = 0; i < N; i++) {
        printf("%u, ", timestamps[i+1] - timestamps[i]);
    }
    printf("\r\n\r\n");

    printf("back to main\r\n");
    while (1);
}
