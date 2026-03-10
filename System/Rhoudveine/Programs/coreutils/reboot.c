#include <libc.h>

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;
  printf("Rebooting Rhoudveine OS...\n");
  reboot();
  return 0;
}
