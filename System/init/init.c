/* Simple init shell
 * This file is compiled and embedded into the kernel as the fallback `init`.
 * It implements a tiny line-based shell using kernel-provided input/output
 * symbols when available; when built as a standalone ELF (loaded by the
 * kernel) it falls back to using the provided print function for output and
 * disables input-dependent features.
 */

#include <stdint.h>
#include <stddef.h>
#include "kernel_services.h"

static kernel_services_t *g_services = NULL;

/* Weak kernel helpers (kept for compatibility during transition). */
extern void beep(uint32_t freq, uint64_t duration) __attribute__((weak));
extern int getchar(void) __attribute__((weak));
extern int putchar(int c) __attribute__((weak));
extern void puts(const char* s) __attribute__((weak));
/* Current working directory (for display purposes) */
static char g_cwd[128] = "/";

/* ACPI power management */
extern void acpi_shutdown(void) __attribute__((weak));
extern void acpi_reboot(void) __attribute__((weak));

/* Timer functions */
extern uint64_t timer_get_uptime_ms(void) __attribute__((weak));
extern uint64_t timer_get_ticks(void) __attribute__((weak));

/* AHCI disk functions */
extern int ahci_read_sectors(uint64_t lba, uint32_t count, uint8_t *buffer) __attribute__((weak));
extern int ahci_is_initialized(void) __attribute__((weak));

// VFS directory entry
extern void fb_backspace(void) __attribute__((weak));
extern void fb_cursor_show(void) __attribute__((weak));
extern void fb_cursor_hide(void) __attribute__((weak));
extern int try_getchar(void) __attribute__((weak));
extern void kernel_panic_shell(const char *reason) __attribute__((weak));

extern void fb_backspace(void) __attribute__((weak));
extern void fb_cursor_show(void) __attribute__((weak));
extern void fb_cursor_hide(void) __attribute__((weak));
extern int try_getchar(void) __attribute__((weak));
extern void kernel_panic_shell(const char *reason) __attribute__((weak));

static void (*g_print_fn)(const char*) = 0;

void out_puts(const char *s) {
    if (g_services && g_services->fb_cursor_hide) g_services->fb_cursor_hide();
    
    if (g_services && g_services->puts) g_services->puts(s);
    else if (puts) puts(s);
    
    if (g_services && g_services->fb_cursor_show) g_services->fb_cursor_show();
}

void out_putchar(char c) {
    if (g_services && g_services->fb_cursor_hide) g_services->fb_cursor_hide();
    
    if (g_services && g_services->putchar) g_services->putchar((int)c);
    else if (putchar) putchar((int)c);
    
    if (g_services && g_services->fb_cursor_show) g_services->fb_cursor_show();
}

int in_getchar(void) {
    if (getchar) return getchar();
    return -1;
}

static int my_strlen(const char *s) { int i = 0; while (s && s[i]) i++; return i; }
static int my_strcmp(const char *a, const char *b) { if (!a||!b) return (a==b)?0:(a?1:-1); int i=0; while(a[i]&&b[i]){ if(a[i]!=b[i]) return (int)(a[i]-b[i]); i++; } return (int)(a[i]-b[i]); }
static int my_strncmp(const char *a, const char *b, int n) { if(!a||!b) return (a==b)?0:(a?1:-1); for(int i=0;i<n;i++){ if(a[i]=='\0'&&b[i]=='\0') return 0; if(a[i]!=b[i]) return (int)(a[i]-b[i]); if(a[i]=='\0'||b[i]=='\0') return (int)(a[i]-b[i]); } return 0; }

// Helper for simple path normalization
// dest must be large enough (128 bytes)
static void resolve_path(char *dest, const char *cwd, const char *input) {
    char temp[128];
    int t_len = 0;
    
    // 1. Base path
    if (input[0] == '/') {
        temp[0] = '/';
        temp[1] = '\0';
        t_len = 1;
        input++; // Skip leading slash
    } else {
        // Copy cwd
        int i = 0;
        while(cwd[i]) { temp[t_len++] = cwd[i]; i++; }
        if (t_len == 0 || temp[t_len-1] != '/') {
            temp[t_len++] = '/';
        }
        temp[t_len] = '\0';
    }
    
    // 2. Process input segments
    const char *p = input;
    while (*p) {
        // skip extra slashes
        while (*p == '/') p++;
        if (!*p) break;
        
        // find end of next segment
        const char *end = p;
        while (*end && *end != '/') end++;
        
        int seg_len = (int)(end - p);
        
        // Handle "." and ".."
        if (seg_len == 1 && p[0] == '.') {
            // ignore
        } else if (seg_len == 2 && p[0] == '.' && p[1] == '.') {
            // pop last component
            if (t_len > 1) {
                // find last slash
                t_len--; // skip trailing slash
                while (t_len > 0 && temp[t_len-1] != '/') t_len--;
                temp[t_len] = '\0';
            }
        } else {
            // Append component
            if (t_len > 1 && temp[t_len-1] != '/') {
                temp[t_len++] = '/';
            }
            for (int k=0; k<seg_len; k++) {
                if (t_len < 127) temp[t_len++] = p[k];
            }
            temp[t_len] = '\0';
        }
        
        p = end;
    }
    
    // 3. Cleanup: remove trailing slash if not root
    if (t_len > 1 && temp[t_len-1] == '/') {
        temp[t_len-1] = '\0';
    } else if (t_len == 0) {
        temp[0] = '/';
        temp[1] = '\0';
    } else {
        temp[t_len] = '\0';
    }
    
    // output
    int i = 0;
    while(temp[i]) { dest[i] = temp[i]; i++; }
    dest[i] = '\0';
}

static void execute_command(char *buf) {
    if (buf[0] == '\0' || buf[0] == '#') return;

    if (my_strcmp(buf, "help") == 0) {
        out_puts("Available commands:\n");
        out_puts("  help      - show this message\n");
        out_puts("  echo ...  - echo text\n");
        out_puts("  cdl [path]- list directory contents\n");
        out_puts("  dump <f>  - display file contents\n");
        out_puts("  write <file> <text> - write text to file\n");
        out_puts("  mkdir <dir> - create directory\n");
        out_puts("  mount <path> <type> <dev> - mount filesystem\n");
        out_puts("  cd <path> - change directory\n");
        out_puts("  uptime    - show system uptime\n");
        out_puts("  diskread <lba> - read sector from disk\n");
        out_puts("  shutdown  - ACPI shutdown\n");
        out_puts("  reboot    - ACPI reboot\n");
        return;
    }

    if (my_strcmp(buf, "uptime") == 0) {
        if (g_services && g_services->timer_get_uptime_ms) {
            uint64_t ms = g_services->timer_get_uptime_ms();
            uint64_t seconds = ms / 1000;
            uint64_t minutes = seconds / 60;
            uint64_t hours = minutes / 60;
            
            seconds %= 60;
            minutes %= 60;
            
            char hour_str[20], min_str[20], sec_str[20];
            int h_len = 0, m_len = 0, s_len = 0;
            uint64_t h = hours, m = minutes, s = seconds;
            
            if (h == 0) { hour_str[h_len++] = '0'; }
            else { while (h > 0) { hour_str[h_len++] = '0' + (h % 10); h /= 10; } }
            hour_str[h_len] = '\0';
            for (int i = 0; i < h_len/2; i++) {
                char tmp = hour_str[i];
                hour_str[i] = hour_str[h_len-1-i];
                hour_str[h_len-1-i] = tmp;
            }
            
            if (m == 0) { min_str[m_len++] = '0'; }
            else { while (m > 0) { min_str[m_len++] = '0' + (m % 10); m /= 10; } }
            min_str[m_len] = '\0';
            for (int i = 0; i < m_len/2; i++) {
                char tmp = min_str[i];
                min_str[i] = min_str[m_len-1-i];
                min_str[m_len-1-i] = tmp;
            }
            
            if (s == 0) { sec_str[s_len++] = '0'; }
            else { while (s > 0) { sec_str[s_len++] = '0' + (s % 10); s /= 10; } }
            sec_str[s_len] = '\0';
            for (int i = 0; i < s_len/2; i++) {
                char tmp = sec_str[i];
                sec_str[i] = sec_str[s_len-1-i];
                sec_str[s_len-1-i] = tmp;
            }
            
            out_puts("Uptime: ");
            out_puts(hour_str); out_puts("h ");
            out_puts(min_str); out_puts("m ");
            out_puts(sec_str); out_puts("s\n");
        } else {
            out_puts("Timer not available\n");
        }
        return;
    }

    if (my_strcmp(buf, "shutdown") == 0) {
        if (g_services && g_services->acpi_shutdown) {
            out_puts("Initiating ACPI shutdown...\n");
            g_services->acpi_shutdown();
        } else {
            out_puts("ACPI shutdown not available, halting\n");
            for (;;) { __asm__("cli; hlt"); }
        }
        return;
    }

    if (my_strncmp(buf, "write ", 6) == 0) {
        if (g_services && g_services->vfs_open && g_services->vfs_write && g_services->vfs_close) {
            const char *p = buf + 6;
            char filename[128];
            int i = 0;
            while (*p && *p != ' ' && i < 127) {
                filename[i++] = *p++;
            }
            filename[i] = '\0';
            if (*p == ' ') p++; 

            char abs_path[128];
            resolve_path(abs_path, g_cwd, filename);
            
            int fd = g_services->vfs_open(abs_path, 0x0101); // O_CREAT | O_WRONLY
            if (fd < 0) {
                out_puts("Failed to open file for writing\n");
                return;
            }
            
            int len = my_strlen(p);
            g_services->vfs_write(fd, p, len);
            g_services->vfs_write(fd, "\n", 1);
            g_services->vfs_close(fd);
        } else {
            out_puts("VFS not available\n");
        }
        return;
    }

    if (my_strncmp(buf, "mkdir ", 6) == 0) {
        if (g_services && g_services->vfs_mkdir) {
            const char *path = buf + 6;
            char abs_path[128];
            resolve_path(abs_path, g_cwd, path);
            if (g_services->vfs_mkdir(abs_path) != 0) {
                out_puts("Failed to create directory\n");
            }
        } else {
            out_puts("VFS not available\n");
        }
        return;
    }

    if (my_strncmp(buf, "mount ", 6) == 0) {
        if (g_services && g_services->vfs_mount) {
            // mount <path> <type> <dev>
            const char *p = buf + 6;
            char path[64], type[32], dev[64];
            int i = 0;
            while (*p && *p != ' ' && i < 63) path[i++] = *p++;
            path[i] = '\0'; if (*p == ' ') p++;
            i = 0; while (*p && *p != ' ' && i < 31) type[i++] = *p++;
            type[i] = '\0'; if (*p == ' ') p++;
            i = 0; while (*p && *p != ' ' && i < 63) dev[i++] = *p++;
            dev[i] = '\0';

            if (g_services->vfs_mount(path, type, dev) == 0) {
                out_puts("Mounted "); out_puts(type); out_puts(" at "); out_puts(path); out_puts("\n");
            } else {
                out_puts("Failed to mount\n");
            }
        } else {
            out_puts("VFS mount not available\n");
        }
        return;
    }

    if (my_strncmp(buf, "diskread ", 9) == 0) {
        if (g_services && g_services->ahci_is_initialized && g_services->ahci_read_sectors) {
            if (!g_services->ahci_is_initialized()) {
                out_puts("AHCI not initialized\n");
                return;
            }
            const char *p = buf + 9;
            uint64_t lba = 0;
            while (*p >= '0' && *p <= '9') {
                lba = lba * 10 + (*p - '0');
                p++;
            }
            static uint8_t sector_buf[512];
            int result = g_services->ahci_read_sectors(lba, 1, sector_buf);
            if (result == 0) {
                out_puts("Read successful!\n");
            } else {
                out_puts("Read failed!\n");
            }
        } else {
            out_puts("AHCI driver not available\n");
        }
        return;
    }

    if (my_strcmp(buf, "reboot") == 0) {
        if (g_services && g_services->acpi_reboot) {
            out_puts("Initiating ACPI reboot...\n");
            g_services->acpi_reboot();
        } else {
            out_puts("ACPI reboot not available, halting\n");
            for (;;) { __asm__("cli; hlt"); }
        }
        return;
    }

    if (my_strcmp(buf, "panic") == 0) {
        if (g_services && g_services->kernel_panic_shell) {
            g_services->kernel_panic_shell("manual panic from init shell");
        }
        return;
    }

    if (my_strncmp(buf, "panic ", 6) == 0) {
        if (g_services && g_services->kernel_panic_shell) {
            g_services->kernel_panic_shell(buf + 6);
        }
        return;
    }

    if (my_strncmp(buf, "echo ", 5) == 0) {
        out_puts(buf + 5);
        out_putchar('\n');
        return;
    }

    if (my_strcmp(buf, "cdl") == 0 || my_strncmp(buf, "cdl ", 4) == 0) {
        if (g_services && g_services->vfs_open && g_services->vfs_readdir && g_services->vfs_close) {
            const char *input_path = (buf[3] == ' ') ? buf + 4 : "";
            char abs_path[128];
            resolve_path(abs_path, g_cwd, input_path);

            int fd = g_services->vfs_open(abs_path, 0);
            if (fd < 0) {
                out_puts("Failed to open directory\n");
                return;
            }
            
            vfs_dirent_t entry;
            while (g_services->vfs_readdir(fd, &entry) == 0) {
                out_puts(entry.name);
                if (entry.type & 0x02) out_puts("/");
                out_puts("\n");
            }
            g_services->vfs_close(fd);
        } else {
            out_puts("VFS not available\n");
        }
        return;
    }

    if (my_strncmp(buf, "dump ", 5) == 0) {
        if (g_services && g_services->vfs_open && g_services->vfs_read && g_services->vfs_close) {
            const char *path = buf + 5;
            char abs_path[128];
            resolve_path(abs_path, g_cwd, path);
            
            int fd = g_services->vfs_open(abs_path, 0);
            if (fd < 0) {
                out_puts("Failed to open file\n");
                return;
            }
            
            static uint8_t read_buf[512];
            int bytes;
            while ((bytes = g_services->vfs_read(fd, read_buf, sizeof(read_buf))) > 0) {
                for (int i = 0; i < bytes; i++) {
                    out_putchar(read_buf[i]);
                }
            }
            out_putchar('\n');
            g_services->vfs_close(fd);
        } else {
            out_puts("VFS not available\n");
        }
        return;
    }

    // Unified CD logic
    const char *target_arg = NULL;
    int is_cd = 0;
    if (my_strncmp(buf, "cd ", 3) == 0) { target_arg = buf + 3; is_cd = 1; }
    else if (buf[0] == '/') { target_arg = buf; is_cd = 1; }
    else {
        int l = my_strlen(buf);
        if (l > 0 && buf[l-1] == '/') { target_arg = buf; is_cd = 1; }
        else if (my_strcmp(buf, "..") == 0) { target_arg = ".."; is_cd = 1; }
    }
    
    if (is_cd && target_arg) {
        char new_path[128];
        resolve_path(new_path, g_cwd, target_arg);
        if (g_services && g_services->vfs_open && g_services->vfs_close) {
            int fd = g_services->vfs_open(new_path, 0);
            if (fd >= 0) {
                g_services->vfs_close(fd);
                int n_len = my_strlen(new_path);
                for(int i=0; i<=n_len; i++) g_cwd[i] = new_path[i];
            } else {
                out_puts("Path not found\n");
            }
        }
        return;
    }

    out_puts("Unknown command: "); out_puts(buf); out_puts("\n");
}

static void run_initrc(const char *path) {
    if (!g_services || !g_services->vfs_open || !g_services->vfs_read || !g_services->vfs_close) return;

    int fd = g_services->vfs_open(path, 0);
    if (fd < 0) return;

    char line[128];
    int line_pos = 0;
    uint8_t byte;

    while (g_services->vfs_read(fd, &byte, 1) == 1) {
        if (byte == '\n' || byte == '\r') {
            if (line_pos > 0) {
                line[line_pos] = '\0';
                execute_command(line);
                line_pos = 0;
            }
        } else if (line_pos < 127) {
            line[line_pos++] = (char)byte;
        }
    }
    if (line_pos > 0) {
        line[line_pos] = '\0';
        execute_command(line);
    }

    g_services->vfs_close(fd);
}

void main(kernel_services_t *services) {
    g_services = services;

    // Diagnostic: Enable interrupts now that we are in init
    // __asm__ volatile ("sti");

    if (g_services && g_services->beep) {
        g_services->beep(50000000, 1000, true);
    }

    out_puts("--- INIT GRAVITON ---\n");
    out_puts("Rhoudveine init shell. Type 'help' for commands.\n");

    const int BUF_SIZE = 128;
    char buf[BUF_SIZE];
    int pos = 0;

    // 1. Execute startup script
    run_initrc("/System/Rhoudveine/Booter/initrc");

    // 2. Main interactive loop
    for (;;) {
        if (g_services && g_services->fb_cursor_hide) g_services->fb_cursor_hide();
        out_puts("init> ");
        pos = 0;
        int blink_counter = 0;
        int cursor_state = 0;
        if (g_services && g_services->fb_cursor_show) g_services->fb_cursor_show();
        
        while (1) {
            int c = -1;
            if (g_services && g_services->try_getchar) c = g_services->try_getchar();
            
            if (c <= 0) {
                blink_counter++;
                if (blink_counter >= 50) {
                    blink_counter = 0;
                    cursor_state = !cursor_state;
                    if (cursor_state) { if (g_services && g_services->fb_cursor_show) g_services->fb_cursor_show(); }
                    else { if (g_services && g_services->fb_cursor_hide) g_services->fb_cursor_hide(); }
                }
                for (volatile int z = 0; z < 20000; z++);
                continue;
            }
            if (c == '\r' || c == '\n') {
                if (g_services && g_services->fb_cursor_hide) g_services->fb_cursor_hide();
                out_putchar('\n');
                if (g_services && g_services->fb_cursor_show) g_services->fb_cursor_show();
                buf[pos] = '\0';
                break;
            }
            if (c == '\b' || c == 127) {
                if (pos > 0) {
                    pos--;
                    if (g_services && g_services->fb_cursor_hide) g_services->fb_cursor_hide();
                    if (g_services && g_services->fb_backspace) g_services->fb_backspace();
                    else out_putchar('\b');
                    if (g_services && g_services->fb_cursor_show) g_services->fb_cursor_show();
                }
                continue;
            }
            if (pos < BUF_SIZE - 1) {
                buf[pos++] = (char)c;
                if (g_services && g_services->fb_cursor_hide) g_services->fb_cursor_hide();
                out_putchar((char)c);
                if (g_services && g_services->fb_cursor_show) g_services->fb_cursor_show();
            }
        }

        if (pos > 0) {
            execute_command(buf);
        }
    }
}
