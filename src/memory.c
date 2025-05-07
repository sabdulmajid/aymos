#include "k_mem.h"
#include "k_task.h"
#include "common.h"
#include <stdio.h>

extern uint32_t _estack;
extern uint32_t _Min_Stack_Size;
extern uint32_t _img_end;

extern int init_flag;
extern task_t new_task_id;
extern task_t current_task_id;

int mem_init_flag = 0;

const uint32_t BLOCK_SIZE = 24;

typedef struct MEM_BLOCK {
    task_t owner;
    uint8_t allocated;

    struct MEM_BLOCK* next;
    struct MEM_BLOCK* prev;

    uint32_t* start_location;
    uint32_t size;
} MEM_BLOCK;

static MEM_BLOCK* free_list = NULL;

static uint32_t* heap_start = NULL;
static uint32_t* heap_end = NULL;

static uint32_t align_to_word(uint32_t size);

int k_mem_init(void) {
    if (mem_init_flag || !init_flag) {
        return RTX_ERR;
    }

    heap_start = (uint32_t*)&_img_end;
    heap_end = (uint32_t*)((uint32_t)&_estack - (uint32_t)&_Min_Stack_Size);

    free_list = (MEM_BLOCK*)heap_start;
    free_list->size = (uint32_t)heap_end - (uint32_t)heap_start - BLOCK_SIZE;
    free_list->owner = TID_NULL;
    free_list->allocated = 0;
    free_list->next = NULL;
    free_list->prev = NULL;
    free_list->start_location = free_list;

    mem_init_flag = 1;
    return RTX_OK;
}

static MEM_BLOCK* last_alloc = NULL;
static uint32_t min_alloc_size = 0;

void* k_mem_alloc(unsigned int size) {
    if (!mem_init_flag || size == 0) {
        return NULL;
    }

    uint32_t aligned_size = align_to_word(size);
    MEM_BLOCK* start = last_alloc && (min_alloc_size <= aligned_size || last_alloc->size <= aligned_size) ? last_alloc->next : free_list;
    MEM_BLOCK* curr = start;
    MEM_BLOCK* prev = NULL;

    do {
        if (!curr->allocated && curr->size >= aligned_size) {
            if (curr->size >= aligned_size + BLOCK_SIZE + 4) {
                MEM_BLOCK* new_block = (MEM_BLOCK*)((uint32_t)curr + BLOCK_SIZE + aligned_size);
                new_block->size = curr->size - aligned_size - BLOCK_SIZE;
                new_block->owner = TID_NULL;
                new_block->allocated = 0;
                new_block->next = curr->next;
                new_block->prev = curr;
                new_block->start_location = new_block;

                if (curr->next) {
                    curr->next->prev = new_block;
                }

                curr->size = aligned_size;
                curr->next = new_block;
            }

            curr->allocated = 1;
            curr->owner = new_task_id;
            last_alloc = curr;
            min_alloc_size = (last_alloc) ? ((min_alloc_size < aligned_size) ? min_alloc_size : aligned_size) : aligned_size;

            return (void*)((uint32_t)curr + BLOCK_SIZE);
        }

        prev = curr;
        curr = curr->next ? curr->next : free_list;
    } while (curr != start);

    return NULL;
}

int k_mem_dealloc(void* ptr) {
    if (!mem_init_flag) {
        return RTX_ERR;
    }

    if (ptr == NULL) {
        return RTX_OK;
    }

    MEM_BLOCK* block = (MEM_BLOCK*)((uint32_t)ptr - BLOCK_SIZE);
    if ((uint32_t)block < (uint32_t)heap_start || (uint32_t)block >= (uint32_t)heap_end) {
        return RTX_ERR;
    }

    if (block->start_location != block || (uint32_t*)((uint32_t)block + BLOCK_SIZE) != ptr) {
        return RTX_ERR;
    }

    if (!block->allocated) {
        return RTX_ERR;
    }

    if (block->owner != current_task_id && current_task_id != TID_NULL) {
        return RTX_ERR;
    }

    block->allocated = 0;
    block->owner = TID_NULL;
    last_alloc = NULL;

    MEM_BLOCK* coalesce_start = block;
    if (coalesce_start->prev != NULL && !coalesce_start->prev->allocated) {
        coalesce_start = free_list;
    }
    coalesce(coalesce_start);

    return RTX_OK;
}

void coalesce(MEM_BLOCK* curr) {
    while (curr != NULL && curr->next != NULL) {
        if (!curr->allocated && !curr->next->allocated) {
            MEM_BLOCK* to_merge = curr->next;
            curr->size += BLOCK_SIZE + to_merge->size;
            curr->next = to_merge->next;
            if (to_merge->next != NULL) {
                to_merge->next->prev = curr;
            }
        } else {
            curr = curr->next;
        }
    }
}

int k_mem_count_extfrag(unsigned int size) {
    if (!mem_init_flag) {
        return 0;
    }

    uint32_t aligned_size = align_to_word(size);
    int count = 0;
    MEM_BLOCK* curr = free_list;

    while (curr != NULL) {
        if (!curr->allocated && curr->size < aligned_size) {
            count++;
        }
        curr = curr->next;
    }

    return count;
}

static uint32_t align_to_word(uint32_t size) {
    return (size + 3) & ~0x03;
}
