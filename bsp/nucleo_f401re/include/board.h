#ifndef AYMOS_NUCLEO_F401RE_BOARD_H
#define AYMOS_NUCLEO_F401RE_BOARD_H

#include "stm32f4xx_hal.h"

#include <stddef.h>

HAL_StatusTypeDef board_init(void);
HAL_StatusTypeDef board_uart_write(const char *data, size_t length);
void board_panic(void) __attribute__((noreturn));

#endif
