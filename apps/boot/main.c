#include "board.h"

#include "stm32f4xx.h"

int main(void)
{
    static const char banner[] = "AYMOS READY\r\n";
    static const char smoke[] = "AYMOS SMOKE SYSTICK=1 SVC=1\r\n";

    if (board_init() != HAL_OK) {
        board_panic();
    }

    if (board_uart_write(banner, sizeof(banner) - 1U) != HAL_OK) {
        board_panic();
    }

    while (!board_systick_observed()) {
        __WFI();
    }

    board_invoke_svc_smoke();
    if (!board_svc_observed()) {
        board_panic();
    }

    if (board_uart_write(smoke, sizeof(smoke) - 1U) != HAL_OK) {
        board_panic();
    }

    for (;;) {
        __WFI();
    }
}
