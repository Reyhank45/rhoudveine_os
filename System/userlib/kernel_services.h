#ifndef KERNEL_SERVICES_H
#define KERNEL_SERVICES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Matches kernel's struct dirent
typedef struct {
    uint32_t inode;
    char name[256];
    uint8_t type;
} vfs_dirent_t;

typedef struct {
    void (*puts)(const char*);
    void (*putchar)(char);
    int (*getchar)(void);
    int (*try_getchar)(void);
    
    void (*fb_backspace)(void);
    void (*fb_cursor_show)(void);
    void (*fb_cursor_hide)(void);
    
    void (*beep)(double, double, bool);
    uint64_t (*timer_get_uptime_ms)(void);
    
    void (*kernel_panic_shell)(const char*);

    // VFS operations
    int (*vfs_open)(const char *path, uint32_t flags);
    int (*vfs_close)(int fd);
    int (*vfs_read)(int fd, void *buffer, size_t count);
    int (*vfs_write)(int fd, const void *buffer, size_t count);
    int (*vfs_readdir)(int fd, vfs_dirent_t *entry);
    int (*vfs_mkdir)(const char *path);
    int (*vfs_mount)(const char *path, const char *fstype, const char *device);

    // ACPI/AHCI operations
    void (*acpi_shutdown)(void);
    void (*acpi_reboot)(void);
    int (*ahci_read_sectors)(uint64_t lba, uint32_t count, uint8_t *buffer);
    int (*ahci_is_initialized)(void);
} kernel_services_t;

#endif
