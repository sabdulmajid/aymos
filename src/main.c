#include "main.h"
#include "k_task.h"
#include <stdio.h>

void print_continuously(void) {
	while(1) {
		printf("Thread!\r\n");
	}
}

extern uint32_t _Min_Stack_Size;
uint32_t* stackptr;

int main(void) {
	HAL_Init();
	SystemClock_Config();
	MX_GPIO_Init();
	MX_USART2_UART_Init();

	uint32_t* msp_init = *(uint32_t**)0x0;
	printf("MSP Init is: %p\r\n", msp_init);

	stackptr = (uint32_t*)((uint32_t)msp_init - (uint32_t)&(_Min_Stack_Size));

	*(--stackptr) = 1<<24;
	*(--stackptr) = (uint32_t) print_continuously;
	for (int i = 0; i < 14; i++) {
		*(--stackptr) = 0xA;
	}

	StartThread();

	printf("_____END MAIN_____\r\n");

	while (1) {
	}
}
