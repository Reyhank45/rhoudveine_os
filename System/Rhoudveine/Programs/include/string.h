#ifndef _STRING_H
#define _STRING_H

#include <stddef.h>

size_t strlen(const char *s);
int strcmp(const char *s1, const char *s2);
char *strcpy(char *dest, const char *src);
void *memset(void *s, int c, size_t n);

#endif
