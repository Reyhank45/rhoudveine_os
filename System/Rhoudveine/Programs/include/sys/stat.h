#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <stdint.h>
#include <sys/types.h>

struct stat {
  uint32_t st_dev;
  uint32_t st_ino;
  uint32_t st_mode;
  uint32_t st_nlink;
  uint32_t st_uid;
  uint32_t st_gid;
  uint32_t st_rdev;
  uint64_t st_size;
};

int mkdir(const char *pathname);

#endif
