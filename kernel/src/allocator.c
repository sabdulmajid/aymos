#include "aymos_allocator.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

typedef struct os_memory_block os_memory_block_t;

struct os_memory_block {
    uintptr_t marker;
    size_t capacity;
    os_memory_block_t *next;
    os_memory_block_t *previous;
    os_memory_owner_t owner;
    uint8_t allocated;
    uint8_t reserved[5];
};

enum {
    OS_MEMORY_BLOCK_MARKER = 0xA94D4F53U
};

_Static_assert((sizeof(os_memory_block_t) % OS_MEMORY_ALIGNMENT) == 0U,
               "allocator metadata must preserve payload alignment");
_Static_assert(_Alignof(os_memory_block_t) <= OS_MEMORY_ALIGNMENT,
               "allocator metadata requires unsupported alignment");

static bool align_up_size(size_t value, size_t *aligned);
static uintptr_t align_up_address(uintptr_t value);
static uintptr_t align_down_address(uintptr_t value);
static uintptr_t block_marker(const os_allocator_t *allocator,
                              const os_memory_block_t *block);
static bool address_can_hold_block(const os_allocator_t *allocator,
                                   uintptr_t address);
static bool block_chain_valid(const os_allocator_t *allocator);
static os_memory_block_t *first_block(const os_allocator_t *allocator);
static os_memory_block_t *next_fit_block(const os_allocator_t *allocator);
static uint8_t *payload_address(os_memory_block_t *block);
static const uint8_t *const_payload_address(const os_memory_block_t *block);
static os_memory_block_t *find_payload_block(const os_allocator_t *allocator,
                                             const void *pointer);
static void set_free(os_memory_block_t *block);
static void merge_with_next(os_allocator_t *allocator,
                            os_memory_block_t *block);
static void coalesce_all(os_allocator_t *allocator);
static void increment_saturating(uint32_t *value);
static bool reserved_bytes_clear(const os_memory_block_t *block);

bool os_allocator_init(os_allocator_t *allocator, void *arena,
                       size_t arena_size)
{
    if (allocator == NULL) {
        return false;
    }
    memset(allocator, 0, sizeof(*allocator));
    if (arena == NULL || arena_size < OS_MEMORY_ALIGNMENT) {
        return false;
    }

    const uintptr_t raw_begin = (uintptr_t)arena;
    if (arena_size > UINTPTR_MAX - raw_begin) {
        return false;
    }
    const uintptr_t raw_end = raw_begin + arena_size;
    const uintptr_t begin = align_up_address(raw_begin);
    const uintptr_t end = align_down_address(raw_end);
    if (begin < raw_begin || end > raw_end || end <= begin ||
        end - begin < sizeof(os_memory_block_t) + OS_MEMORY_ALIGNMENT) {
        return false;
    }

    allocator->heap_begin = (uint8_t *)begin;
    allocator->heap_end = (uint8_t *)end;
    allocator->cookie = begin ^ end ^ (uintptr_t)OS_MEMORY_BLOCK_MARKER;

    os_memory_block_t *const block = (os_memory_block_t *)begin;
    block->marker = block_marker(allocator, block);
    block->capacity = (size_t)(end - begin) - sizeof(*block);
    block->next = NULL;
    block->previous = NULL;
    set_free(block);

    allocator->first_block = block;
    allocator->next_fit = block;
    allocator->initialized = true;
    if (!block_chain_valid(allocator)) {
        memset(allocator, 0, sizeof(*allocator));
        return false;
    }
    return true;
}

bool os_allocator_validate(const os_allocator_t *allocator)
{
    return block_chain_valid(allocator);
}

void *os_allocator_alloc(os_allocator_t *allocator, size_t size,
                         os_memory_owner_t owner)
{
    if (allocator == NULL || !allocator->initialized) {
        return NULL;
    }
    if (!block_chain_valid(allocator) || size == 0U ||
        owner == OS_MEMORY_OWNER_NONE) {
        increment_saturating(&allocator->failed_allocations);
        return NULL;
    }

    size_t aligned_size = 0U;
    if (!align_up_size(size, &aligned_size)) {
        increment_saturating(&allocator->failed_allocations);
        return NULL;
    }

    /* next_fit names the first block examined by the next search. A split
       advances it to the remainder; exact fit advances with wrap. */
    os_memory_block_t *const start = next_fit_block(allocator);
    if (start == NULL) {
        increment_saturating(&allocator->failed_allocations);
        return NULL;
    }

    os_memory_block_t *block = start;
    do {
        if (block->allocated == 0U && block->capacity >= aligned_size) {
            const size_t remainder = block->capacity - aligned_size;
            if (remainder >= sizeof(*block) + OS_MEMORY_ALIGNMENT) {
                uint8_t *const split_address = (uint8_t *)(
                    (uintptr_t)payload_address(block) + aligned_size);
                os_memory_block_t *const split =
                    (os_memory_block_t *)split_address;
                split->marker = block_marker(allocator, split);
                split->capacity = remainder - sizeof(*split);
                split->next = block->next;
                split->previous = block;
                set_free(split);
                if (block->next != NULL) {
                    block->next->previous = split;
                }
                block->capacity = aligned_size;
                block->next = split;
                allocator->next_fit = split;
            } else {
                allocator->next_fit =
                    (block->next != NULL) ? block->next : allocator->first_block;
            }

            block->allocated = 1U;
            block->owner = owner;
            allocator->allocated_bytes += block->capacity;
            if (allocator->allocated_bytes > allocator->high_watermark_bytes) {
                allocator->high_watermark_bytes = allocator->allocated_bytes;
            }
            increment_saturating(&allocator->successful_allocations);
            if (!block_chain_valid(allocator)) {
                return NULL;
            }
            return payload_address(block);
        }
        block = (block->next != NULL) ? block->next : first_block(allocator);
    } while (block != start);

    increment_saturating(&allocator->failed_allocations);
    return NULL;
}

bool os_allocator_free(os_allocator_t *allocator, void *pointer,
                       os_memory_owner_t owner)
{
    if (allocator == NULL || !allocator->initialized) {
        return false;
    }
    if (pointer == NULL) {
        return true;
    }
    if (!block_chain_valid(allocator) || owner == OS_MEMORY_OWNER_NONE) {
        increment_saturating(&allocator->invalid_frees);
        return false;
    }

    os_memory_block_t *const block = find_payload_block(allocator, pointer);
    if (block == NULL || block->allocated == 0U || block->owner != owner) {
        increment_saturating(&allocator->invalid_frees);
        return false;
    }

    allocator->allocated_bytes -= block->capacity;
    set_free(block);
    coalesce_all(allocator);
    increment_saturating(&allocator->successful_frees);
    return block_chain_valid(allocator);
}

size_t os_allocator_release_owner(os_allocator_t *allocator,
                                  os_memory_owner_t owner)
{
    if (allocator == NULL || !allocator->initialized ||
        owner == OS_MEMORY_OWNER_NONE || !block_chain_valid(allocator)) {
        return 0U;
    }

    size_t released = 0U;
    for (os_memory_block_t *block = first_block(allocator); block != NULL;
         block = block->next) {
        if (block->allocated != 0U && block->owner == owner) {
            allocator->allocated_bytes -= block->capacity;
            set_free(block);
            increment_saturating(&allocator->successful_frees);
            ++released;
        }
    }
    if (released != 0U) {
        coalesce_all(allocator);
        allocator->next_fit = allocator->first_block;
    }
    if (!block_chain_valid(allocator)) {
        return 0U;
    }
    return released;
}

bool os_allocator_get_stats(const os_allocator_t *allocator,
                            os_memory_stats_t *stats)
{
    if (allocator == NULL || stats == NULL || !allocator->initialized ||
        !block_chain_valid(allocator)) {
        return false;
    }

    memset(stats, 0, sizeof(*stats));
    stats->heap_bytes = (size_t)((uintptr_t)allocator->heap_end -
                                 (uintptr_t)allocator->heap_begin);
    stats->allocated_bytes = allocator->allocated_bytes;
    stats->high_watermark_bytes = allocator->high_watermark_bytes;
    stats->successful_allocations = allocator->successful_allocations;
    stats->successful_frees = allocator->successful_frees;
    stats->failed_allocations = allocator->failed_allocations;
    stats->invalid_frees = allocator->invalid_frees;

    for (const os_memory_block_t *block = first_block(allocator);
         block != NULL; block = block->next) {
        stats->payload_capacity_bytes += block->capacity;
        if (block->allocated != 0U) {
            ++stats->allocated_blocks;
        } else {
            ++stats->free_blocks;
            stats->free_bytes += block->capacity;
            if (block->capacity > stats->largest_free_block_bytes) {
                stats->largest_free_block_bytes = block->capacity;
            }
        }
    }
    return true;
}

size_t os_allocator_count_fragments(const os_allocator_t *allocator,
                                    size_t requested_size)
{
    size_t aligned_size = 0U;
    if (allocator == NULL || !allocator->initialized || requested_size == 0U ||
        !align_up_size(requested_size, &aligned_size) ||
        !block_chain_valid(allocator)) {
        return 0U;
    }

    size_t count = 0U;
    for (const os_memory_block_t *block = first_block(allocator);
         block != NULL; block = block->next) {
        if (block->allocated == 0U && block->capacity < aligned_size) {
            ++count;
        }
    }
    return count;
}

static bool align_up_size(size_t value, size_t *aligned)
{
    if (aligned == NULL || value > SIZE_MAX - (OS_MEMORY_ALIGNMENT - 1U)) {
        return false;
    }
    *aligned =
        (value + (OS_MEMORY_ALIGNMENT - 1U)) & ~(OS_MEMORY_ALIGNMENT - 1U);
    return *aligned != 0U;
}

static uintptr_t align_up_address(uintptr_t value)
{
    if (value > UINTPTR_MAX - (OS_MEMORY_ALIGNMENT - 1U)) {
        return UINTPTR_MAX;
    }
    return (value + (OS_MEMORY_ALIGNMENT - 1U)) &
           ~((uintptr_t)OS_MEMORY_ALIGNMENT - 1U);
}

static uintptr_t align_down_address(uintptr_t value)
{
    return value & ~((uintptr_t)OS_MEMORY_ALIGNMENT - 1U);
}

static uintptr_t block_marker(const os_allocator_t *allocator,
                              const os_memory_block_t *block)
{
    return allocator->cookie ^ (uintptr_t)block ^
           (uintptr_t)OS_MEMORY_BLOCK_MARKER;
}

static bool address_can_hold_block(const os_allocator_t *allocator,
                                   uintptr_t address)
{
    const uintptr_t begin = (uintptr_t)allocator->heap_begin;
    const uintptr_t end = (uintptr_t)allocator->heap_end;
    return address >= begin && (address & (OS_MEMORY_ALIGNMENT - 1U)) == 0U &&
           address <= end && end - address >= sizeof(os_memory_block_t);
}

static bool block_chain_valid(const os_allocator_t *allocator)
{
    if (allocator == NULL || !allocator->initialized ||
        allocator->heap_begin == NULL || allocator->heap_end == NULL ||
        allocator->first_block == NULL || allocator->next_fit == NULL) {
        return false;
    }
    const uintptr_t begin = (uintptr_t)allocator->heap_begin;
    const uintptr_t end = (uintptr_t)allocator->heap_end;
    if (begin >= end || (begin & (OS_MEMORY_ALIGNMENT - 1U)) != 0U ||
        (end & (OS_MEMORY_ALIGNMENT - 1U)) != 0U ||
        (uintptr_t)allocator->first_block != begin ||
        !address_can_hold_block(allocator, (uintptr_t)allocator->next_fit)) {
        return false;
    }

    const os_memory_block_t *previous = NULL;
    const os_memory_block_t *block = first_block(allocator);
    bool found_next_fit = false;
    size_t computed_allocated = 0U;
    size_t visited = 0U;
    const size_t maximum_blocks =
        (size_t)(end - begin) /
        (sizeof(os_memory_block_t) + OS_MEMORY_ALIGNMENT);

    while (block != NULL) {
        const uintptr_t address = (uintptr_t)block;
        if (++visited > maximum_blocks ||
            !address_can_hold_block(allocator, address) ||
            block->marker != block_marker(allocator, block) ||
            block->previous != previous ||
            (block->allocated != 0U && block->allocated != 1U) ||
            !reserved_bytes_clear(block) ||
            (block->capacity & (OS_MEMORY_ALIGNMENT - 1U)) != 0U ||
            block->capacity > end - address - sizeof(*block) ||
            (block->allocated == 0U && block->owner != OS_MEMORY_OWNER_NONE) ||
            (block->allocated != 0U && block->owner == OS_MEMORY_OWNER_NONE)) {
            return false;
        }

        const uintptr_t expected_next =
            address + sizeof(*block) + block->capacity;
        if (expected_next == end) {
            if (block->next != NULL) {
                return false;
            }
        } else if (!address_can_hold_block(allocator, expected_next) ||
                   (uintptr_t)block->next != expected_next) {
            return false;
        }

        if (block->allocated != 0U) {
            if (computed_allocated > SIZE_MAX - block->capacity) {
                return false;
            }
            computed_allocated += block->capacity;
        }
        if ((const void *)block == allocator->next_fit) {
            found_next_fit = true;
        }
        previous = block;
        block = block->next;
    }
    return found_next_fit && computed_allocated == allocator->allocated_bytes &&
           allocator->high_watermark_bytes >= allocator->allocated_bytes;
}

static os_memory_block_t *first_block(const os_allocator_t *allocator)
{
    return (os_memory_block_t *)allocator->first_block;
}

static os_memory_block_t *next_fit_block(const os_allocator_t *allocator)
{
    return (os_memory_block_t *)allocator->next_fit;
}

static uint8_t *payload_address(os_memory_block_t *block)
{
    return (uint8_t *)((uintptr_t)block + sizeof(*block));
}

static const uint8_t *const_payload_address(const os_memory_block_t *block)
{
    return (const uint8_t *)((uintptr_t)block + sizeof(*block));
}

static os_memory_block_t *find_payload_block(const os_allocator_t *allocator,
                                             const void *pointer)
{
    const uintptr_t address = (uintptr_t)pointer;
    if ((address & (OS_MEMORY_ALIGNMENT - 1U)) != 0U ||
        address < (uintptr_t)allocator->heap_begin + sizeof(os_memory_block_t) ||
        address >= (uintptr_t)allocator->heap_end) {
        return NULL;
    }
    for (os_memory_block_t *block = first_block(allocator); block != NULL;
         block = block->next) {
        if ((uintptr_t)const_payload_address(block) == address) {
            return block;
        }
    }
    return NULL;
}

static void set_free(os_memory_block_t *block)
{
    block->owner = OS_MEMORY_OWNER_NONE;
    block->allocated = 0U;
    memset(block->reserved, 0, sizeof(block->reserved));
}

static void merge_with_next(os_allocator_t *allocator,
                            os_memory_block_t *block)
{
    os_memory_block_t *const removed = block->next;
    if (removed == NULL || block->allocated != 0U || removed->allocated != 0U) {
        return;
    }
    if (allocator->next_fit == removed) {
        allocator->next_fit = block;
    }
    block->capacity += sizeof(*removed) + removed->capacity;
    block->next = removed->next;
    if (block->next != NULL) {
        block->next->previous = block;
    }
}

static void coalesce_all(os_allocator_t *allocator)
{
    os_memory_block_t *block = first_block(allocator);
    while (block != NULL && block->next != NULL) {
        if (block->allocated == 0U && block->next->allocated == 0U) {
            merge_with_next(allocator, block);
        } else {
            block = block->next;
        }
    }
}

static void increment_saturating(uint32_t *value)
{
    if (*value != UINT32_MAX) {
        ++*value;
    }
}

static bool reserved_bytes_clear(const os_memory_block_t *block)
{
    for (size_t index = 0U; index < sizeof(block->reserved); ++index) {
        if (block->reserved[index] != 0U) {
            return false;
        }
    }
    return true;
}
