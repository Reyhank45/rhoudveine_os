#include <libc.h>

int main(int argc, char *argv[]) {
  if (argc < 2) {
    printf("Usage: cdir <directory>\n");
    return 1;
  }

  if (mkdir(argv[1]) == -1) {
    printf("cdir: failed to create directory '%s'\n", argv[1]);
    return 1;
  }

  return 0;
}
