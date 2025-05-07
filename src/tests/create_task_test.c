#include "common.h"
#include "k_task.h"
#include "main.h"
#include <stdio.h>

#define EXPECTED_MAX_TASKS 16
#define TEST_STACK_SIZE 0x400

void f_test_task(void* arg) {
  printf("PASS: kernel kept its own copy of TCB\r\n");
  while (1); //does not yield
}

void f_print_fail(void* arg) {
  printf("FAIL: first task was clobbered\r\n");
  while (1);
}

int main(void) {
 /* MCU Configuration: Don't change this or the whole chip won't work!*/

 /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
 HAL_Init();
 /* Configure the system clock */
 SystemClock_Config();

 /* Initialize all configured peripherals */
 MX_GPIO_Init();
 MX_USART2_UART_Init();
 /* MCU Configuration is now complete. Start writing your code below this line */

 osKernelInit();

 TCB mytask = (TCB){
   .ptask = &f_test_task, //expect this to go into kernel's copy
   .stack_size = TEST_STACK_SIZE, //expect this to go into kernel's copy
   .tid = 0xff, //kernel to assign a TID between 1-15
   .stack_high = 0, //expect kernel's copy to get an allocated stack address
 };

 if (osCreateTask(&mytask) != RTX_OK) { //if successful this should have updated the user's copy with the TID assigned
   printf("FAIL: osCreateTask failed\r\n");
   return 0;
 }

 TCB task_readback = (TCB){  //create a fresh object to ensure osTaskInfo did something
    .ptask = 0,
    .stack_size = 0,
    .tid = 0xff,
    .stack_high = 0
 };

 if ((mytask.tid >= EXPECTED_MAX_TASKS) || (mytask.tid == 0 )) { //expect between 1-15
   printf("FAIL: osCreateTask did not update the input TCB with a valid TID \r\n\r\n");
   return 0;
 } else {
   printf("PASS: osCreateTask updated the input TCB with a valid TID %u\r\n\r\n", mytask.tid);
 	if (osTaskInfo(mytask.tid, &task_readback) != RTX_OK) {
           printf("FAIL: osTaskInfo failed\r\n");
           return 0;
	}
 }

 printf("Values retrieved by osTaskInfo:\r\n");
 printf("ptask=%p\r\n", task_readback.ptask);
 printf("stack_high=0x%x\r\n", task_readback.stack_high);
 printf("tid=%u\r\n", task_readback.tid);
 printf("state=0x%x\r\n", task_readback.state);
 printf("stack_size=0x%x\r\n", task_readback.stack_size);

 // check population of TCB
 if (task_readback.tid == mytask.tid && task_readback.stack_high != 0 && task_readback.ptask == &f_test_task && task_readback.stack_size == TEST_STACK_SIZE)
   printf("PASS: TCB populated correctly\r\n\r\n");
 else
   printf("FAIL: TCB not populated correctly\r\n\r\n");


 //create another task, everything the same except task function.
 //This function should never start if the first task is working (because it doesn't yield)
 mytask.ptask = &f_print_fail;
 if (osCreateTask(&mytask) != RTX_OK) {
   printf("FAIL: osCreateTask failed\r\n");
   return 0;
 }

 osKernelStart();

 while (1);
}
