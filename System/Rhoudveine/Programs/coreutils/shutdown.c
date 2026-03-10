#include <libc.h>

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;
  printf("Shutting down Rhoudveine OS...\n");
  shutdown();
  return 0;
}
