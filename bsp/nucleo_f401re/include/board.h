#ifndef AYMOS_NUCLEO_F401RE_BOARD_H
#define AYMOS_NUCLEO_F401RE_BOARD_H

#include "stm32f4xx_hal.h"

#include <stdbool.h>
#include <stddef.h>

HAL_StatusTypeDef board_init(void);
HAL_StatusTypeDef board_uart_write(const char *data, size_t length);
bool board_systick_observed(void);
bool board_svc_observed(void);
void board_invoke_svc_smoke(void);
void board_panic(void) __attribute__((noreturn));

#endif
