#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>
#include <syscall.h>

void *malloc(size_t size);
void free(void *ptr);

#endif
