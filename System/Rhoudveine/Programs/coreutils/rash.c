#include <libc.h>

#define MAX_CMD_LEN 256
#define MAX_ARGS 16

void print_prompt() {
  char cwd[256];
  if (getcwd(cwd, sizeof(cwd))) {
    printf("rash:%s# ", cwd);
  } else {
    printf("rash:# ");
  }
}

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  printf("Rhoudveine Shell (rash) v0.1\n");

  char cmd[MAX_CMD_LEN];
  while (1) {
    print_prompt();

    int i = 0;
    while (i < MAX_CMD_LEN - 1) {
      int c = getchar();
      if (c < 0 || c == '\n')
        break;
      if (c == '\b' || c == 0x7F) {
        if (i > 0) {
          i--;
          printf("\b \b");
        }
        continue;
      }
      cmd[i++] = (char)c;
      putchar(c);
    }
    cmd[i] = '\0';
    putchar('\n');

    if (i == 0)
      continue;

    // Simple tokenization
    char *args[MAX_ARGS];
    int arg_count = 0;
    char *p = cmd;

    while (*p && arg_count < MAX_ARGS - 1) {
      while (*p == ' ')
        p++;
      if (!*p)
        break;
      args[arg_count++] = p;
      while (*p && *p != ' ')
        p++;
      if (*p)
        *p++ = '\0';
    }
    args[arg_count] = NULL;

    if (arg_count == 0)
      continue;

    if (strcmp(args[0], "exit") == 0) {
      break;
    } else if (strcmp(args[0], "cd") == 0) {
      if (arg_count > 1) {
        if (chdir(args[1]) != 0) {
          printf("cd: failed to change directory to %s\n", args[1]);
        }
      }
    } else {
      // Try to execute
      char path[512];
      if (args[0][0] == '/') {
        strcpy(path, args[0]);
      } else {
        strcpy(path, "/System/Rhoudveine/Programs/");
        char *tmp = path + strlen(path);
        strcpy(tmp, args[0]);
      }

      if (exec(path, args) < 0) {
        printf("rash: command not found: %s\n", args[0]);
      }
    }
  }

  return 0;
}
