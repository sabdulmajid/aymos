#include "aymos_allocator.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    TEST_ARENA_SIZE = 2048U,
    OWNER_A = 1U,
    OWNER_B = 2U
};

static uint32_t checks;

#define CHECK(condition)                                                       \
    do {                                                                       \
        ++checks;                                                              \
        if (!(condition)) {                                                    \
            fprintf(stderr, "allocator check failed at %s:%d: %s\n",         \
                    __FILE__, __LINE__, #condition);                           \
            exit(EXIT_FAILURE);                                                \
        }                                                                      \
    } while (0)

typedef struct {
    uint8_t *arena;
    os_allocator_t allocator;
} fixture_t;

static void init_fixture(fixture_t *fixture);
static void destroy_fixture(fixture_t *fixture);
static os_memory_stats_t stats(const os_allocator_t *allocator);
static void fill_and_check(void *pointer, size_t size, uint8_t value);
static void check_bytes(const void *pointer, size_t size, uint8_t value);
static void test_init_alignment_and_boundaries(void);
static void test_split_and_exact_fit(void);
static void test_stack_sized_alignment(void);
static void test_next_fit_and_wrap(void);
static void test_coalescing(void);
static void test_exhaustion_and_recovery(void);
static void test_invalid_free_and_ownership(void);
static void test_fragmentation_and_statistics(void);
static void test_release_owner(void);
static void test_metadata_corruption_fails_closed(void);

int main(void)
{
    test_init_alignment_and_boundaries();
    test_split_and_exact_fit();
    test_stack_sized_alignment();
    test_next_fit_and_wrap();
    test_coalescing();
    test_exhaustion_and_recovery();
    test_invalid_free_and_ownership();
    test_fragmentation_and_statistics();
    test_release_owner();
    test_metadata_corruption_fails_closed();
    printf("allocator native tests: %" PRIu32 " checks passed\n", checks);
    return EXIT_SUCCESS;
}

static void init_fixture(fixture_t *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->arena = malloc(TEST_ARENA_SIZE);
    CHECK(fixture->arena != NULL);
    memset(fixture->arena, 0xA5, TEST_ARENA_SIZE);
    CHECK(os_allocator_init(&fixture->allocator, fixture->arena,
                            TEST_ARENA_SIZE));
    CHECK(os_allocator_validate(&fixture->allocator));
}

static void destroy_fixture(fixture_t *fixture)
{
    free(fixture->arena);
    fixture->arena = NULL;
}

static os_memory_stats_t stats(const os_allocator_t *allocator)
{
    os_memory_stats_t result;
    CHECK(os_allocator_get_stats(allocator, &result));
    return result;
}

static void fill_and_check(void *pointer, size_t size, uint8_t value)
{
    memset(pointer, value, size);
    check_bytes(pointer, size, value);
}

static void check_bytes(const void *pointer, size_t size, uint8_t value)
{
    const uint8_t *const bytes = pointer;
    for (size_t index = 0U; index < size; ++index) {
        CHECK(bytes[index] == value);
    }
}

static void test_init_alignment_and_boundaries(void)
{
    uint8_t *const arena = malloc(TEST_ARENA_SIZE + 3U);
    os_allocator_t allocator;
    CHECK(arena != NULL);
    CHECK(!os_allocator_init(NULL, arena, sizeof(arena)));
    CHECK(!os_allocator_init(&allocator, NULL, sizeof(arena)));
    CHECK(!os_allocator_init(&allocator, arena, 1U));
    CHECK(os_allocator_init(&allocator, &arena[1], TEST_ARENA_SIZE));
    const os_memory_stats_t initial = stats(&allocator);
    CHECK(initial.heap_bytes < TEST_ARENA_SIZE);
    CHECK(initial.heap_bytes % OS_MEMORY_ALIGNMENT == 0U);
    CHECK(initial.free_blocks == 1U);
    CHECK(initial.allocated_blocks == 0U);
    CHECK(initial.free_bytes == initial.largest_free_block_bytes);
    CHECK(initial.payload_capacity_bytes == initial.free_bytes);
    void *const pointer = os_allocator_alloc(&allocator, 1U, OWNER_A);
    CHECK(pointer != NULL);
    CHECK((uintptr_t)pointer % OS_MEMORY_ALIGNMENT == 0U);
    CHECK(os_allocator_free(&allocator, pointer, OWNER_A));
    free(arena);
}

static void test_split_and_exact_fit(void)
{
    fixture_t fixture;
    init_fixture(&fixture);
    const os_memory_stats_t initial = stats(&fixture.allocator);

    void *const split = os_allocator_alloc(&fixture.allocator, 13U, OWNER_A);
    CHECK(split != NULL);
    fill_and_check(split, 13U, 0x5AU);
    const os_memory_stats_t after_split = stats(&fixture.allocator);
    CHECK(after_split.allocated_bytes == 16U);
    CHECK(after_split.allocated_blocks == 1U);
    CHECK(after_split.free_blocks == 1U);
    const size_t metadata_size =
        initial.heap_bytes - initial.payload_capacity_bytes;
    CHECK(after_split.payload_capacity_bytes ==
          initial.payload_capacity_bytes - metadata_size);
    CHECK(os_allocator_free(&fixture.allocator, split, OWNER_A));
    const os_memory_stats_t coalesced = stats(&fixture.allocator);
    CHECK(coalesced.free_blocks == 1U);
    CHECK(coalesced.free_bytes == initial.free_bytes);
    void *const exact = os_allocator_alloc(
        &fixture.allocator, coalesced.largest_free_block_bytes, OWNER_B);
    CHECK(exact != NULL);
    const os_memory_stats_t after_exact = stats(&fixture.allocator);
    CHECK(after_exact.free_blocks == 0U);
    CHECK(after_exact.allocated_blocks == 1U);
    destroy_fixture(&fixture);
}

static void test_next_fit_and_wrap(void)
{
    fixture_t fixture;
    init_fixture(&fixture);
    void *const first = os_allocator_alloc(&fixture.allocator, 64U, OWNER_A);
    void *const second = os_allocator_alloc(&fixture.allocator, 64U, OWNER_A);
    void *const third = os_allocator_alloc(&fixture.allocator, 64U, OWNER_A);
    CHECK(first != NULL && second != NULL && third != NULL);
    CHECK(os_allocator_free(&fixture.allocator, first, OWNER_A));

    void *const from_tail =
        os_allocator_alloc(&fixture.allocator, 32U, OWNER_A);
    CHECK(from_tail != NULL);
    CHECK(from_tail != first);

    const os_memory_stats_t before_exact = stats(&fixture.allocator);
    void *const consume_tail = os_allocator_alloc(
        &fixture.allocator, before_exact.largest_free_block_bytes, OWNER_A);
    CHECK(consume_tail != NULL);
    void *const wrapped = os_allocator_alloc(&fixture.allocator, 32U, OWNER_A);
    CHECK(wrapped == first);
    CHECK(os_allocator_validate(&fixture.allocator));
    destroy_fixture(&fixture);
}

static void test_stack_sized_alignment(void)
{
    fixture_t fixture;
    init_fixture(&fixture);
    void *const stack_sized =
        os_allocator_alloc(&fixture.allocator, 1024U, OWNER_A);
    CHECK(stack_sized != NULL);
    CHECK((uintptr_t)stack_sized % OS_MEMORY_ALIGNMENT == 0U);
    CHECK(stats(&fixture.allocator).allocated_bytes == 1024U);
    CHECK(os_allocator_free(&fixture.allocator, stack_sized, OWNER_A));
    CHECK(stats(&fixture.allocator).free_blocks == 1U);
    destroy_fixture(&fixture);
}

static void test_coalescing(void)
{
    fixture_t fixture;
    init_fixture(&fixture);
    void *const first = os_allocator_alloc(&fixture.allocator, 64U, OWNER_A);
    void *const middle = os_allocator_alloc(&fixture.allocator, 64U, OWNER_A);
    void *const last = os_allocator_alloc(&fixture.allocator, 64U, OWNER_A);
    CHECK(first != NULL && middle != NULL && last != NULL);
    fill_and_check(first, 64U, 0x11U);
    fill_and_check(middle, 64U, 0x22U);
    fill_and_check(last, 64U, 0x33U);

    CHECK(os_allocator_free(&fixture.allocator, middle, OWNER_A));
    check_bytes(first, 64U, 0x11U);
    check_bytes(last, 64U, 0x33U);
    CHECK(stats(&fixture.allocator).free_blocks == 2U);
    CHECK(os_allocator_free(&fixture.allocator, first, OWNER_A));
    check_bytes(last, 64U, 0x33U);
    CHECK(stats(&fixture.allocator).free_blocks == 2U);
    CHECK(os_allocator_free(&fixture.allocator, last, OWNER_A));
    const os_memory_stats_t final = stats(&fixture.allocator);
    CHECK(final.free_blocks == 1U);
    CHECK(final.allocated_blocks == 0U);
    CHECK(final.free_bytes == final.largest_free_block_bytes);
    destroy_fixture(&fixture);
}

static void test_exhaustion_and_recovery(void)
{
    fixture_t fixture;
    init_fixture(&fixture);
    const size_t exact_size = stats(&fixture.allocator).largest_free_block_bytes;
    void *const exact =
        os_allocator_alloc(&fixture.allocator, exact_size, OWNER_A);
    CHECK(exact != NULL);
    const os_memory_stats_t full = stats(&fixture.allocator);
    CHECK(full.free_blocks == 0U);
    CHECK(full.free_bytes == 0U);
    CHECK(full.allocated_blocks == 1U);
    CHECK(os_allocator_alloc(&fixture.allocator, 8U, OWNER_A) == NULL);
    CHECK(os_allocator_alloc(&fixture.allocator, SIZE_MAX, OWNER_A) == NULL);
    CHECK(os_allocator_alloc(&fixture.allocator, 0U, OWNER_A) == NULL);
    CHECK(stats(&fixture.allocator).failed_allocations == 3U);
    CHECK(os_allocator_free(&fixture.allocator, exact, OWNER_A));
    CHECK(stats(&fixture.allocator).free_blocks == 1U);
    CHECK(os_allocator_alloc(&fixture.allocator, exact_size, OWNER_A) == exact);
    destroy_fixture(&fixture);
}

static void test_invalid_free_and_ownership(void)
{
    fixture_t fixture;
    init_fixture(&fixture);
    void *const pointer = os_allocator_alloc(&fixture.allocator, 64U, OWNER_A);
    CHECK(pointer != NULL);
    CHECK(os_allocator_alloc(&fixture.allocator, 8U,
                             OS_MEMORY_OWNER_NONE) == NULL);
    CHECK(os_allocator_free(&fixture.allocator, NULL, OWNER_A));
    CHECK(!os_allocator_free(&fixture.allocator, pointer, OWNER_B));
    CHECK(!os_allocator_free(&fixture.allocator, pointer,
                             OS_MEMORY_OWNER_NONE));
    CHECK(!os_allocator_free(&fixture.allocator,
                             (uint8_t *)pointer + OS_MEMORY_ALIGNMENT,
                             OWNER_A));
    CHECK(!os_allocator_free(&fixture.allocator, (uint8_t *)pointer + 1U,
                             OWNER_A));
    CHECK(!os_allocator_free(&fixture.allocator, fixture.arena, OWNER_A));
    CHECK(!os_allocator_free(&fixture.allocator,
                             fixture.arena + TEST_ARENA_SIZE, OWNER_A));
    CHECK(os_allocator_free(&fixture.allocator, pointer, OWNER_A));
    CHECK(!os_allocator_free(&fixture.allocator, pointer, OWNER_A));
    const os_memory_stats_t result = stats(&fixture.allocator);
    CHECK(result.invalid_frees == 7U);
    CHECK(result.failed_allocations == 1U);
    CHECK(result.successful_frees == 1U);
    CHECK(result.allocated_blocks == 0U);
    destroy_fixture(&fixture);
}

static void test_fragmentation_and_statistics(void)
{
    fixture_t fixture;
    init_fixture(&fixture);
    void *blocks[5];
    for (size_t index = 0U; index < 5U; ++index) {
        blocks[index] =
            os_allocator_alloc(&fixture.allocator, 32U, OWNER_A);
        CHECK(blocks[index] != NULL);
    }
    CHECK(os_allocator_free(&fixture.allocator, blocks[1], OWNER_A));
    CHECK(os_allocator_free(&fixture.allocator, blocks[3], OWNER_A));
    CHECK(os_allocator_count_fragments(&fixture.allocator, 64U) == 2U);
    CHECK(os_allocator_count_fragments(&fixture.allocator, 32U) == 0U);
    CHECK(os_allocator_count_fragments(&fixture.allocator, 0U) == 0U);
    const os_memory_stats_t fragmented = stats(&fixture.allocator);
    CHECK(fragmented.allocated_blocks == 3U);
    CHECK(fragmented.free_blocks == 3U);
    CHECK(fragmented.allocated_bytes == 96U);
    CHECK(fragmented.successful_allocations == 5U);
    CHECK(fragmented.successful_frees == 2U);
    CHECK(fragmented.high_watermark_bytes == 160U);
    CHECK(fragmented.free_bytes >= fragmented.largest_free_block_bytes);
    destroy_fixture(&fixture);
}

static void test_release_owner(void)
{
    fixture_t fixture;
    init_fixture(&fixture);
    void *const a1 = os_allocator_alloc(&fixture.allocator, 24U, OWNER_A);
    void *const b1 = os_allocator_alloc(&fixture.allocator, 40U, OWNER_B);
    void *const a2 = os_allocator_alloc(&fixture.allocator, 56U, OWNER_A);
    CHECK(a1 != NULL && b1 != NULL && a2 != NULL);
    CHECK(os_allocator_release_owner(&fixture.allocator, OWNER_A) == 2U);
    os_memory_stats_t after_release = stats(&fixture.allocator);
    CHECK(after_release.allocated_blocks == 1U);
    CHECK(after_release.allocated_bytes == 40U);
    CHECK(after_release.successful_frees == 2U);
    CHECK(!os_allocator_free(&fixture.allocator, a1, OWNER_A));
    CHECK(os_allocator_free(&fixture.allocator, b1, OWNER_B));
    CHECK(os_allocator_release_owner(&fixture.allocator, OWNER_A) == 0U);
    after_release = stats(&fixture.allocator);
    CHECK(after_release.free_blocks == 1U);
    CHECK(after_release.allocated_blocks == 0U);
    destroy_fixture(&fixture);
}

static void test_metadata_corruption_fails_closed(void)
{
    fixture_t fixture;
    init_fixture(&fixture);
    const os_memory_stats_t initial = stats(&fixture.allocator);
    const size_t metadata_size =
        initial.heap_bytes - initial.payload_capacity_bytes;
    CHECK(metadata_size >= OS_MEMORY_ALIGNMENT);
    uint8_t *const pointer =
        os_allocator_alloc(&fixture.allocator, 64U, OWNER_A);
    CHECK(pointer != NULL);
    pointer[-(ptrdiff_t)metadata_size] ^= 0x01U;
    CHECK(!os_allocator_validate(&fixture.allocator));
    CHECK(os_allocator_alloc(&fixture.allocator, 8U, OWNER_A) == NULL);
    CHECK(!os_allocator_free(&fixture.allocator, pointer, OWNER_A));
    destroy_fixture(&fixture);
}
