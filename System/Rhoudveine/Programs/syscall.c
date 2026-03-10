#include <dirent.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define exit __kernel_exit
#include <syscall.h>
#undef exit

// Global exit for crt0.s
void exit(int status) { __kernel_exit(status); }

// String functions
size_t strlen(const char *s) {
  size_t len = 0;
  while (s[len])
    len++;
  return len;
}

int strcmp(const char *s1, const char *s2) {
  while (*s1 && (*s1 == *s2)) {
    s1++;
    s2++;
  }
  return *(unsigned char *)s1 - *(unsigned char *)s2;
}

void *memset(void *s, int c, size_t n) {
  unsigned char *p = s;
  while (n--)
    *p++ = (unsigned char)c;
  return s;
}

// itoa and utoa are still needed by vsnprintf
static void utoa_internal(uint64_t n, char *s, int base) {
  int i = 0;
  if (n == 0) {
    s[i++] = '0';
  } else {
    while (n > 0) {
      int d = n % base;
      s[i++] = (d > 9) ? (d - 10) + 'a' : d + '0';
      n /= base;
    }
  }
  s[i] = '\0';
  for (int j = 0; j < i / 2; j++) {
    char t = s[j];
    s[j] = s[i - 1 - j];
    s[i - 1 - j] = t;
  }
}

static void itoa_internal(int64_t n, char *s, int base) {
  if (base == 10 && n < 0) {
    *s++ = '-';
    utoa_internal((uint64_t)-n, s, base);
  } else {
    utoa_internal((uint64_t)n, s, base);
  }
}

int vsnprintf(char *str, size_t size, const char *format, va_list args) {
  size_t i = 0, j = 0;
  while (format[i] && j < size - 1) {
    if (format[i] == '%') {
      i++;
      if (format[i] == 's') {
        char *s = va_arg(args, char *);
        while (*s && j < size - 1)
          str[j++] = *s++;
      } else if (format[i] == 'd') {
        char buf[32];
        itoa_internal(va_arg(args, int), buf, 10);
        char *p = buf;
        while (*p && j < size - 1)
          str[j++] = *p++;
      } else if (format[i] == 'u') {
        char buf[32];
        utoa_internal(va_arg(args, unsigned int), buf, 10);
        char *p = buf;
        while (*p && j < size - 1)
          str[j++] = *p++;
      } else if (format[i] == 'x') {
        char buf[32];
        utoa_internal(va_arg(args, unsigned int), buf, 16);
        char *p = buf;
        while (*p && j < size - 1)
          str[j++] = *p++;
      } else {
        str[j++] = format[i];
      }
    } else {
      str[j++] = format[i];
    }
    i++;
  }
  str[j] = '\0';
  return (int)j;
}

int printf(const char *fmt, ...) {
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  return (int)write(1, buf, (size_t)n);
}

int getchar(void) {
  char c;
  if (read(0, &c, 1) <= 0)
    return -1;
  return (int)c;
}

static uint8_t heap[64 * 1024];
static size_t heap_off = 0;

void *malloc(size_t size) {
  if (heap_off + size > sizeof(heap))
    return NULL;
  void *ptr = &heap[heap_off];
  heap_off += (size + 7) & ~7; // 8-byte align
  return ptr;
}

void free(void *ptr) { (void)ptr; }

DIR *opendir(const char *name) {
  int fd = open(name, 0);
  if (fd < 0)
    return NULL;
  return (DIR *)(intptr_t)fd;
}

struct dirent *readdir(DIR *dirp) {
  static struct dirent entry;
  int fd = (int)(intptr_t)dirp;
  if (syscall2(SYS_READDIR, fd, (long)&entry) == 0) {
    return &entry;
  }
  return NULL;
}

int closedir(DIR *dirp) {
  int fd = (int)(intptr_t)dirp;
  return close(fd);
}

unsigned int sleep(unsigned int seconds) {
  (void)seconds;
  return 0;
}
