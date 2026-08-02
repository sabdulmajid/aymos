#ifndef AYMOS_ALLOCATOR_H
#define AYMOS_ALLOCATOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    OS_MEMORY_ALIGNMENT = 8U,
    OS_MEMORY_OWNER_NONE = UINT16_MAX
};

typedef uint16_t os_memory_owner_t;

typedef struct {
    size_t heap_bytes;
    size_t payload_capacity_bytes;
    size_t allocated_bytes;
    size_t free_bytes;
    size_t largest_free_block_bytes;
    size_t high_watermark_bytes;
    size_t allocated_blocks;
    size_t free_blocks;
    uint32_t successful_allocations;
    uint32_t successful_frees;
    uint32_t failed_allocations;
    uint32_t invalid_frees;
} os_memory_stats_t;

/* Byte counters report aligned block capacity, including an unsplittable tail
 * assigned to an allocation. Operation counters saturate at UINT32_MAX. */

/* The allocator object contains no storage for user allocations. The caller
 * supplies one contiguous arena to os_allocator_init(). Its fields are public
 * only so native tests can instantiate the same implementation as firmware;
 * applications should use the os_memory_* kernel API instead. The portable
 * core is deliberately not internally synchronized; firmware wrappers hold
 * PRIMASK for every operation. */
typedef struct {
    uint8_t *heap_begin;
    uint8_t *heap_end;
    void *first_block;
    void *next_fit;
    size_t allocated_bytes;
    size_t high_watermark_bytes;
    uint32_t successful_allocations;
    uint32_t successful_frees;
    uint32_t failed_allocations;
    uint32_t invalid_frees;
    uintptr_t cookie;
    bool initialized;
} os_allocator_t;

bool os_allocator_init(os_allocator_t *allocator, void *arena,
                       size_t arena_size);
bool os_allocator_validate(const os_allocator_t *allocator);
void *os_allocator_alloc(os_allocator_t *allocator, size_t size,
                         os_memory_owner_t owner);
bool os_allocator_free(os_allocator_t *allocator, void *pointer,
                       os_memory_owner_t owner);
size_t os_allocator_release_owner(os_allocator_t *allocator,
                                  os_memory_owner_t owner);
bool os_allocator_get_stats(const os_allocator_t *allocator,
                            os_memory_stats_t *stats);
size_t os_allocator_count_fragments(const os_allocator_t *allocator,
                                    size_t requested_size);

#endif
