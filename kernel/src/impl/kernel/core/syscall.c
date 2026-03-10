#include "include/syscall.h"
#include "include/vfs.h"
#include <stddef.h>
#include <stdint.h>

extern void kprintf(const char *format, uint32_t color, ...);
extern int vfs_open(const char *path, uint32_t flags);
extern int vfs_close(int fd);
extern int vfs_read(int fd, void *buffer, size_t count);
extern struct vfs_node *vfs_resolve_path(const char *path);
extern int vfs_mkdir(const char *path);

// MSR addresses for SYSCALL/SYSRET
#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_SFMASK 0xC0000084

#define EFER_SCE (1 << 0) // System Call Extensions

// Read MSR
static inline uint64_t rdmsr(uint32_t msr) {
  uint32_t lo, hi;
  __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
  return ((uint64_t)hi << 32) | lo;
}

// Write MSR
static inline void wrmsr(uint32_t msr, uint64_t value) {
  __asm__ volatile("wrmsr"
                   :
                   : "c"(msr), "a"((uint32_t)value),
                     "d"((uint32_t)(value >> 32)));
}

// External syscall entry point (defined in assembly)
extern void syscall_entry(void);

// Current process ID (simple counter for now)
static int current_pid = 1;

// Simple string length
static size_t str_len(const char *s) {
  size_t len = 0;
  while (s[len])
    len++;
  return len;
}

// Simple string copy
static void str_cpy(char *dst, const char *src) {
  while ((*dst++ = *src++) != '\0')
    ;
}

// --- Syscall Implementations ---

// Forward declaration — needed because sys_exit re-execs the shell
static int64_t sys_exec(const char *path, char *const argv[]);

static int64_t sys_exit(int status) {
  kprintf("SYSCALL: exit(%d)\n", 0x00FFFF00, status);
  // No real process management yet — re-exec the shell so we
  // get a prompt back instead of freezing.
  sys_exec("/System/Rhoudveine/Programs/rash", NULL);
  // If exec fails, halt as a last resort.
  __asm__ volatile("cli; hlt");
  return 0;
}

static int64_t sys_write(int fd, const char *buf, size_t count) {
  if (!buf || count == 0)
    return -1;

  if (fd == 1 || fd == 2) { // stdout or stderr
    // Write to console
    extern void fb_putc(char c);
    for (size_t i = 0; i < count; i++) {
      fb_putc(buf[i]);
    }
    return (int64_t)count;
  }

  // TODO: VFS file write
  return -1;
}

static int64_t sys_read(int fd, char *buf, size_t count) {
  if (!buf || count == 0)
    return -1;

  if (fd == 0) { // stdin
    extern int ps2_getchar(void);
    for (size_t i = 0; i < count; i++) {
      int c = ps2_getchar();
      if (c < 0)
        return (int64_t)i;
      buf[i] = (char)c;
      if (c == '\n')
        return (int64_t)(i + 1);
    }
    return (int64_t)count;
  }

  // TODO: VFS file read
  return -1;
}

static int64_t sys_open(const char *path, int flags) {
  if (!path)
    return -1;
  kprintf("SYSCALL: open(\"%s\", %d)\n", 0x00FFFF00, path, flags);

#ifdef CONFIG_VFS
  // TODO: Implement proper file descriptor table
  // For now, just verify the file exists
  struct vfs_node *node = vfs_resolve_path(path);
  if (!node)
    return -1;
  return 3; // Return a dummy fd (0,1,2 are std streams)
#else
  return -1;
#endif
}

static int64_t sys_close(int fd) {
  if (fd < 3)
    return -1; // Can't close std streams
  kprintf("SYSCALL: close(%d)\n", 0x00FFFF00, fd);
  // TODO: Implement proper file descriptor cleanup
  return 0;
}

static int64_t sys_exec(const char *path, char *const argv[]) {
  if (!path)
    return -1;
  kprintf("SYSCALL: exec(\"%s\")\n", 0x00FFFF00, path);

  int fd = vfs_open(path, 0);
  if (fd < 0)
    return -1;

#define EXEC_MAX_SIZE (1024 * 1024)
  static uint8_t exec_buf[EXEC_MAX_SIZE];
  int bytes_read = vfs_read(fd, exec_buf, EXEC_MAX_SIZE);
  vfs_close(fd);

  if (bytes_read <= 0)
    return -2;

  extern int elf64_load_and_run(void *image, uint32_t size,
                                void (*print_fn)(const char *));
  extern void fb_puts(const char *s);

  // We don't have a multi-process state to "replace" yet.
  // We just jump. This will work for graviton -> rash.
  int ret = elf64_load_and_run(exec_buf, (uint32_t)bytes_read, fb_puts);

  return (int64_t)ret;
}

static int64_t sys_fork(void) {
  kprintf("SYSCALL: fork() -> 0 (single-process stub)\n", 0x00FFFF00);
  return 0;
}

static int64_t sys_getpid(void) { return current_pid; }

static int64_t sys_waitpid(int pid, int *status, int options) {
  (void)pid;
  (void)status;
  (void)options;
  kprintf("SYSCALL: waitpid(%d) -> 0 (stub)\n", 0x00FFFF00, pid);
  if (status)
    *status = 0;
  return 0;
}

static int64_t sys_sbrk(int64_t increment) {
  (void)increment;
  kprintf("SYSCALL: sbrk(%ld)\n", 0x00FFFF00, increment);
  // TODO: Implement heap expansion
  return -1;
}

static char cwd[256] = "/";

static int64_t sys_getcwd(char *buf, size_t size) {
  if (!buf || size == 0)
    return -1;
  size_t len = str_len(cwd);
  if (len >= size)
    return -1;
  str_cpy(buf, cwd);
  return (int64_t)len;
}

static int64_t sys_chdir(const char *path) {
  if (!path)
    return -1;
  kprintf("SYSCALL: chdir(\"%s\")\n", 0x00FFFF00, path);

#ifdef CONFIG_VFS
  struct vfs_node *node = vfs_resolve_path(path);
  if (!node || !(node->flags & VFS_DIRECTORY))
    return -1;
  str_cpy(cwd, path);
  return 0;
#else
  return -1;
#endif
}

static int64_t sys_mkdir(const char *path) {
  if (!path)
    return -1;
  kprintf("SYSCALL: mkdir(\"%s\")\n", 0x00FFFF00, path);

#ifdef CONFIG_VFS
  return vfs_mkdir(path);
#else
  return -1;
#endif
}

static int64_t sys_stat(const char *path, void *statbuf) {
  (void)path;
  (void)statbuf;
  // TODO: Implement stat
  return -1;
}

static int64_t sys_readdir(int fd, void *dirp) {
  if (!dirp)
    return -1;
  // STAC to allow kernel write to user-space buffer
  __asm__ volatile("stac" ::: "memory");
  int ret = vfs_readdir(fd, (struct dirent *)dirp);
  __asm__ volatile("clac" ::: "memory");
  return (int64_t)ret;
}

static int64_t sys_reboot(void) {
  kprintf("SYSCALL: rebooting...\n", 0x00FFFF00);
  // Reset via 8042 keyboard controller
  uint8_t good = 0x02;
  while (good & 0x02)
    __asm__ volatile("inb $0x64, %0" : "=a"(good));
  __asm__ volatile("outb %0, $0x64" : : "a"((uint8_t)0xFE));
  return 0;
}

static int64_t sys_shutdown(void) {
  kprintf("SYSCALL: shutting down...\n", 0x00FFFF00);
  // QEMU shutdown
  __asm__ volatile("outw %0, %1"
                   :
                   : "a"((uint16_t)0x2000), "d"((uint16_t)0x604));
  // Alternative QEMU/VirtualBox
  __asm__ volatile("outw %0, %1"
                   :
                   : "a"((uint16_t)0x31BE), "d"((uint16_t)0x4004));
  return 0;
}

// Main syscall dispatcher
int64_t syscall_handler(uint64_t num, uint64_t arg1, uint64_t arg2,
                        uint64_t arg3, uint64_t arg4, uint64_t arg5) {
  (void)arg4;
  (void)arg5; // Currently unused

  switch (num) {
  case SYS_EXIT:
    return sys_exit((int)arg1);
  case SYS_WRITE:
    return sys_write((int)arg1, (const char *)arg2, (size_t)arg3);
  case SYS_READ:
    return sys_read((int)arg1, (char *)arg2, (size_t)arg3);
  case SYS_OPEN:
    return sys_open((const char *)arg1, (int)arg2);
  case SYS_CLOSE:
    return sys_close((int)arg1);
  case SYS_EXEC:
    return sys_exec((const char *)arg1, (char *const *)arg2);
  case SYS_FORK:
    return sys_fork();
  case SYS_GETPID:
    return sys_getpid();
  case SYS_WAITPID:
    return sys_waitpid((int)arg1, (int *)arg2, (int)arg3);
  case SYS_SBRK:
    return sys_sbrk((int64_t)arg1);
  case SYS_GETCWD:
    return sys_getcwd((char *)arg1, (size_t)arg2);
  case SYS_CHDIR:
    return sys_chdir((const char *)arg1);
  case SYS_MKDIR:
    return sys_mkdir((const char *)arg1);
  case SYS_STAT:
    return sys_stat((const char *)arg1, (void *)arg2);
  case SYS_READDIR:
    return sys_readdir((int)arg1, (void *)arg2);
  case SYS_REBOOT:
    return sys_reboot();
  case SYS_SHUTDOWN:
    return sys_shutdown();
  default:
    kprintf("SYSCALL: Unknown syscall %d\n", 0xFFFF0000, (int)num);
    return -1;
  }
}

void syscall_init(void) {
  kprintf("SYSCALL: Initializing syscall handler...\n", 0x00FF0000);

  // Enable SYSCALL/SYSRET instructions
  uint64_t efer = rdmsr(MSR_EFER);
  wrmsr(MSR_EFER, efer | EFER_SCE);

  // Set up STAR MSR:
  // Bits 32-47: Kernel CS selector for SYSCALL (0x08)
  // Bits 48-63: Base selector for SYSRET. CPU computes:
  //   CS = base+16 | 3 = 0x20|3 = 0x23 (User Code)
  //   SS = base+8  | 3 = 0x18|3 = 0x1B (User Data)
  uint64_t star = ((uint64_t)0x08 << 32) | ((uint64_t)0x10 << 48);
  wrmsr(MSR_STAR, star);

  // Set LSTAR to syscall entry point
  wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

  // Set SFMASK to clear IF (interrupts) during syscall
  wrmsr(MSR_SFMASK, 0x200); // Clear IF flag

  kprintf("SYSCALL: Handler installed at 0x%lx\n", 0x00FF0000,
          (uint64_t)syscall_entry);
}
