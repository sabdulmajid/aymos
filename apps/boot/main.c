#include "board.h"

#include "stm32f4xx.h"

int main(void)
{
    static const char banner[] = "AYMOS BOOT F401RE\r\n";

    if (board_init() != HAL_OK) {
        board_panic();
    }

    if (board_uart_write(banner, sizeof(banner) - 1U) != HAL_OK) {
        board_panic();
    }

    for (;;) {
        __WFI();
    }
}
