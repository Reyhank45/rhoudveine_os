#include <libc.h>

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  char cwd[256];
  if (getcwd(cwd, sizeof(cwd)) == NULL) {
    cwd[0] = '/';
    cwd[1] = '\0';
  }

  DIR *dir = opendir(cwd);
  if (!dir) {
    printf("cdl: cannot open directory '%s'\n", cwd);
    return 1;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (entry->type == 4) {
      printf("  %s/\n", entry->name);
    } else {
      printf("  %s\n", entry->name);
    }
  }

  closedir(dir);
  return 0;
}
