#ifndef _DIRENT_H
#define _DIRENT_H

#include <stdint.h>

struct dirent {
  uint32_t inode;
  char name[256];
  uint8_t type;
};

typedef void DIR;

DIR *opendir(const char *name);
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);

#endif
