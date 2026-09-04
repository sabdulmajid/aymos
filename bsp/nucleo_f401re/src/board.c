#include "board.h"

#include "stm32f4xx.h"

#include <stdint.h>

enum {
    AYMOS_UART_TIMEOUT_MS = 1000U,
    AYMOS_UART_CHUNK_MAX = UINT16_MAX
};

static UART_HandleTypeDef uart2;

static HAL_StatusTypeDef configure_clock(void);
static HAL_StatusTypeDef configure_gpio(void);
static HAL_StatusTypeDef configure_uart(void);
static void panic_uart_write(const char *data, size_t length);

HAL_StatusTypeDef board_init(void)
{
    if (HAL_Init() != HAL_OK) {
        return HAL_ERROR;
    }

    if (configure_clock() != HAL_OK) {
        return HAL_ERROR;
    }
    if (configure_gpio() != HAL_OK) {
        return HAL_ERROR;
    }
    return configure_uart();
}

HAL_StatusTypeDef board_uart_write(const char *data, size_t length)
{
    size_t offset = 0U;

    if ((data == NULL) && (length != 0U)) {
        return HAL_ERROR;
    }

    while (offset < length) {
        const size_t remaining = length - offset;
        const uint16_t chunk = (remaining > AYMOS_UART_CHUNK_MAX)
                                   ? UINT16_MAX
                                   : (uint16_t)remaining;
        HAL_StatusTypeDef status = HAL_UART_Transmit(
            &uart2, (uint8_t *)(data + offset), chunk, AYMOS_UART_TIMEOUT_MS);

        if (status != HAL_OK) {
            return status;
        }
        offset += chunk;
    }

    return HAL_OK;
}

void board_panic(void)
{
    __disable_irq();
    for (;;) {
        __NOP();
    }
}

void board_kernel_panic(const char *reason)
{
    static const char prefix[] = "AYMOS PANIC ";
    static const char newline[] = "\r\n";

    __disable_irq();
    panic_uart_write(prefix, sizeof(prefix) - 1U);
    if (reason != NULL) {
        size_t length = 0U;
        while (reason[length] != '\0') {
            ++length;
        }
        panic_uart_write(reason, length);
    }
    panic_uart_write(newline, sizeof(newline) - 1U);
    for (;;) {
        __NOP();
    }
}

__attribute__((weak)) void board_lifecycle_idle_hook(void)
{
}

static void panic_uart_write(const char *data, size_t length)
{
    enum { PANIC_UART_SPIN_LIMIT = 1000000U };

    if ((__HAL_RCC_USART2_IS_CLK_ENABLED() == 0U) || data == NULL) {
        return;
    }
    for (size_t index = 0U; index < length; ++index) {
        uint32_t remaining = PANIC_UART_SPIN_LIMIT;
        while ((USART2->SR & USART_SR_TXE) == 0U && remaining > 0U) {
            --remaining;
        }
        if (remaining == 0U) {
            return;
        }
        USART2->DR = (uint8_t)data[index];
    }
    uint32_t remaining = PANIC_UART_SPIN_LIMIT;
    while ((USART2->SR & USART_SR_TC) == 0U && remaining > 0U) {
        --remaining;
    }
}

void HAL_MspInit(void)
{
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);
}

void HAL_UART_MspInit(UART_HandleTypeDef *handle)
{
    GPIO_InitTypeDef gpio = {0};

    if (handle->Instance != USART2) {
        return;
    }

    __HAL_RCC_USART2_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    gpio.Pin = GPIO_PIN_2 | GPIO_PIN_3;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF7_USART2;
    HAL_GPIO_Init(GPIOA, &gpio);
}

static HAL_StatusTypeDef configure_clock(void)
{
    RCC_OscInitTypeDef oscillator = {0};
    RCC_ClkInitTypeDef clocks = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

    oscillator.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    oscillator.HSIState = RCC_HSI_ON;
    oscillator.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    oscillator.PLL.PLLState = RCC_PLL_ON;
    oscillator.PLL.PLLSource = RCC_PLLSOURCE_HSI;
    oscillator.PLL.PLLM = 16U;
    oscillator.PLL.PLLN = 336U;
    oscillator.PLL.PLLP = RCC_PLLP_DIV4;
    oscillator.PLL.PLLQ = 7U;
    if (HAL_RCC_OscConfig(&oscillator) != HAL_OK) {
        return HAL_ERROR;
    }

    clocks.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                       RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clocks.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clocks.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clocks.APB1CLKDivider = RCC_HCLK_DIV2;
    clocks.APB2CLKDivider = RCC_HCLK_DIV1;

    return HAL_RCC_ClockConfig(&clocks, FLASH_LATENCY_2);
}

static HAL_StatusTypeDef configure_gpio(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();

    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_RESET);
    gpio.Pin = GPIO_PIN_5;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &gpio);

    return HAL_OK;
}

static HAL_StatusTypeDef configure_uart(void)
{
    uart2.Instance = USART2;
    uart2.Init.BaudRate = 115200U;
    uart2.Init.WordLength = UART_WORDLENGTH_8B;
    uart2.Init.StopBits = UART_STOPBITS_1;
    uart2.Init.Parity = UART_PARITY_NONE;
    uart2.Init.Mode = UART_MODE_TX_RX;
    uart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    uart2.Init.OverSampling = UART_OVERSAMPLING_16;
    return HAL_UART_Init(&uart2);
}
