#ifndef INC_K_MEM_H_
#define INC_K_MEM_H_

int k_mem_init(void);
void* k_mem_alloc(unsigned int size);
int k_mem_dealloc(void* ptr);
int k_mem_count_extfrag(unsigned int size);

#endif /* INC_K_MEM_H_ */
