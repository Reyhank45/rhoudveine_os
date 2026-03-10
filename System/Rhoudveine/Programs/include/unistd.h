#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>
#include <stdint.h>
#include <syscall.h>

// Use types from syscall.h
// int exec(const char *pathname, char *const argv[]); (already in syscall.h)
// pid_t fork(void); (already in syscall.h)

unsigned int sleep(unsigned int seconds);

#endif
