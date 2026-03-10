#include <libc.h>

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  printf("GRAVITON - Rhoudveine OS Init System (PID 1)\n\n");

  // Start the shell
  printf("GRAVITON: Starting rash...\n");

  char *rash_argv[] = {"/System/Rhoudveine/Programs/rash", NULL};
  exec("/System/Rhoudveine/Programs/rash", rash_argv);

  // If exec returns, it failed
  printf("GRAVITON: Failed to start rash!\n");

  for (;;) {
    // Just hang if init fails
  }

  return 0;
}
