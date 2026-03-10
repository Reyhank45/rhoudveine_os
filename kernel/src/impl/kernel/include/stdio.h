#pragma once

#include <stdint.h>

int getchar(void);
int putchar(int c);
void puts(const char *s);
void kprintf(const char *format, uint32_t color, ...);
