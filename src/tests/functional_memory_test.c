#include "main.h"
#include <stdio.h>
#include "common.h"
#include "k_task.h"
#include "k_mem.h"
#include <stdlib.h>
#include "string.h"

#define TEST_ITERATIONS 100
uint8_t test_data[1000] = "Lorem ipsum dolor sit amet, consectetuer adipiscing elit. Aenean commodo ligula eget dolor. Aenean massa. Cum sociis natoque penatibus et magnis dis parturient montes, nascetur ridiculus mus. Donec quam felis, ultricies nec, pellentesque eu, pretium quis, sem. Nulla consequat massa quis enim. Donec pede justo, fringilla vel, aliquet nec, vulputate eget, arcu. In enim justo, rhoncus ut, imperdiet a, venenatis vitae, justo. Nullam dictum felis eu pede mollis pretium. Integer tincidunt. Cras dapibus. Vivamus elementum semper nisi. Aenean vulputate eleifend tellus. Aenean leo ligula, porttitor eu, consequat vitae, eleifend ac, enim. Aliquam lorem ante, dapibus in, viverra quis, feugiat a, tellus. Phasellus viverra nulla ut metus varius laoreet. Quisque rutrum. Aenean imperdiet. Etiam ultricies nisi vel augue. Curabitur ullamcorper ultricies nisi. Nam eget dui. Etiam rhoncus. Maecenas tempus, tellus eget condimentum rhoncus, sem quam semper libero, sit amet adipiscing sem neque sed ipsum. N";
uint8_t* buffers[TEST_ITERATIONS];
uint8_t checksums[TEST_ITERATIONS];
uint32_t buffer_sizes[TEST_ITERATIONS];
uint32_t total_allocated = 0;

uint8_t calculate_checksum(uint8_t* buffer, uint32_t size) {
    uint8_t checksum = 0;
    for (int i = 0; i < size; i++) {
        checksum = checksum ^ buffer[i];
    }
    return checksum;
}

int main(void) {
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_USART2_UART_Init();

    osKernelInit();
    k_mem_init();

    uint32_t size, idx;
    uint32_t alloc_fails = 0;
    uint32_t dealloc_fails = 0;
    uint32_t checksum_fails = 0;

    printf("\r\n\r\n");
    printf("Starting testing ================================\r\n");

    for (int i = 0; i < TEST_ITERATIONS; i++) {
        if (total_allocated < 0x8000) {
            size = rand() % 1000;
            buffers[i] = k_mem_alloc(size);
            printf("itr=%d, alloc %lu bytes, ptr=%p\r\n", i, size, buffers[i]);

            if (buffers[i] != NULL) {
                buffer_sizes[i] = size;
                checksums[i] = calculate_checksum(test_data, size);
                memcpy(buffers[i], test_data, size);
                total_allocated = total_allocated + size;
            } else {
                printf("NULL POINTER (allocation failed)\r\n\r\n");
                alloc_fails = alloc_fails + 1;
                buffer_sizes[i] = 0;
            }
        } else {
            buffer_sizes[i] = 0;
            buffers[i] = NULL;
        }

        if (i > 0 && i % 2 == 0) {
            do {
                idx = rand() % i;
            } while(buffer_sizes[idx] == 0);

            printf("dealloc mem from itr %lu, ptr=%p, ~%lu bytes\r\n", idx, buffers[idx], buffer_sizes[idx]);
            if (k_mem_dealloc(buffers[idx]) != RTX_ERR) {
                total_allocated = total_allocated - buffer_sizes[idx];
                buffer_sizes[idx] = 0;
                buffers[idx] = NULL;
            } else {
                printf("RTX_ERR (deallocation failed)\r\n\r\n");
                dealloc_fails += 1;
            }
        }
    }

    printf("Validating buffer contents... \r\n");

    uint8_t checksum;
    for (int i = 0; i < TEST_ITERATIONS; i++) {
        if (buffer_sizes[i] > 0) {
            checksum = calculate_checksum(buffers[i], buffer_sizes[i]);
            if (checksum != checksums[i]) {
                checksum_fails += 1;
                printf("buffer from itr %d corrupted, checksum=%u, expected=%u \r\n\r\n", i, checksum, checksums[i]);
            }
        }
    }

    printf("Total corrupted buffers = %lu \r\n", checksum_fails);
    printf("Total failed allocs = %lu \r\n", alloc_fails);
    printf("Total failed deallocs = %lu \r\n", dealloc_fails);

    printf("back to main\r\n");
    while (1);
}
