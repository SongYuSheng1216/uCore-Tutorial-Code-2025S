#ifndef KALLOC_H
#define KALLOC_H
#include "types.h"

void *kalloc();
void kfree(void *);
void kinit();
int bool_enough_mem(uint64 max_num);

#endif // KALLOC_H