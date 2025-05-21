#ifndef INC_K_MEM_H_
#define INC_K_MEM_H_

/** Initialise the heap allocator. */
int k_mem_init(void);

/** Allocate @p size bytes of memory. */
void* k_mem_alloc(unsigned int size);

/** Deallocate a previously allocated block. */
int k_mem_dealloc(void* ptr);

/** Count the number of fragments smaller than @p size. */
int k_mem_count_extfrag(unsigned int size);

#endif /* INC_K_MEM_H_ */
