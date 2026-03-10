#include <libc.h>

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;
  printf("Entering Safe Mode...\n");
  printf("Disabling non-essential drivers...\n");
  return 0;
}
