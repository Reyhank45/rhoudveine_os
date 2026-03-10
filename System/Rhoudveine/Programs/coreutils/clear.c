#include <libc.h>

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;
  printf("\033[2J\033[H");
  return 0;
}
