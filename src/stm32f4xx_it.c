/* Includes ------------------------------------------------------------------*/

#include "common.h"
#include "k_mem.h"
#include "k_task.h"
#include <stdio.h>
#include "main.h"
#include "stm32f4xx_it.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */
extern int kernel_running;

extern task_t current_TID;
extern task_t next_TID;

extern uint32_t* curr_thread_ptr;
extern uint32_t* next_thread_ptr;

extern uint32_t g_system_time;
extern unsigned int svc_number;

extern TCB task_list[];
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/

/* USER CODE BEGIN EV */

/* USER CODE END EV */

/******************************************************************************/
/*           Cortex-M4 Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void) {
    while (1) {
    }
}

/**
  * @brief This function handles Hard fault interrupt.
  */
void HardFault_Handler(void) {
    while (1) {
    }
}

/**
  * @brief This function handles Memory management fault.
  */
void MemManage_Handler(void) {
    while (1) {
    }
}

/**
  * @brief This function handles Pre-fetch fault, memory access fault.
  */
void BusFault_Handler(void) {
    while (1) {
    }
}

/**
  * @brief This function handles Undefined instruction or illegal state.
  */
void UsageFault_Handler(void) {
    while (1) {
    }
}

/**
  * @brief This function handles System service call via SWI instruction.
  */
//void SVC_Handler(void)
//{
  /* USER CODE BEGIN SVCall_IRQn 0 */

  /* USER CODE END SVCall_IRQn 0 */
  /* USER CODE BEGIN SVCall_IRQn 1 */

  /* USER CODE END SVCall_IRQn 1 */
//}

/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void) {
}

/**
  * @brief This function handles Pendable request for system service.
  */
/**
void PendSV_Handler(void)
{
	static int i = 0;
	++i;
  /* USER CODE BEGIN PendSV_IRQn 0 */

  /* USER CODE END PendSV_IRQn 0 */
  /* USER CODE BEGIN PendSV_IRQn 1 */

  /* USER CODE END PendSV_IRQn 1 */
//}



int context_switch_required = 0;

// Update time remaining for all tasks
void updateTaskTimes(void) {
    for (int i = 1; i < MAX_TASKS; i++) {
        if (task_list[i].time_remaining > 0 && task_list[i].state != DORMANT) {
            task_list[i].time_remaining--;
        }
        if (task_list[i].time_remaining == 0 && task_list[i].state != DORMANT) {
            task_list[i].state = READY;
            task_list[i].time_remaining = task_list[i].deadline;
            context_switch_required = 1;
        }
    }
}

/**
  * @brief This function handles System tick timer.
  */
void SysTick_Handler(void) {
    HAL_IncTick();

    if (!kernel_running) {
        return;
    }

    g_system_time++;
    context_switch_required = 0;

    updateTaskTimes();

    if (context_switch_required) {
        next_TID = getEarliestDeadlineTask();

        if (current_TID == next_TID) {
            return;
        }

        for(volatile int i = 0; i < 2500; i++) {
        }

        svc_number = 2;
        SCB->ICSR |= (0x1 << 28);
        __asm("isb");
    }
}

/******************************************************************************/
/* STM32F4xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32f4xx.s).                    */
/******************************************************************************/

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
