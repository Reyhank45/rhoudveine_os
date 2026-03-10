#include "include/vfs.h"
#include "include/vfs.h"
#include "include/mm.h"
#include <stdint.h>
#include <stddef.h>

extern void kprintf(const char *format, uint32_t color, ...);

// Simple RAMFS implementation using 4KB pages from PFA
// Limitations: 
// - Fixed number of nodes per page? No, we'll just alloc pages as needed.
// - Inefficient, uses full 4KB for even small data if we don't block.
// - For now: 1 file = 1 metadata page + N data pages.

typedef struct ramfs_file_data {
    uint8_t *pages[128];  // Array of page pointers (supports up to 512KB per file)
    uint32_t num_pages;   // Number of allocated pages
} ramfs_file_data_t;

typedef struct ramfs_node_data {
    // We can store children pointers here for directories
    // Simplicity: fixed array of children
    #define RAMFS_MAX_CHILDREN 64
    char child_names[RAMFS_MAX_CHILDREN][32];
    struct vfs_node *children[RAMFS_MAX_CHILDREN];
} ramfs_node_data_t;

// Static memory pool for ramfs (no dynamic allocation)
#define MAX_RAMFS_NODES 64
#define MAX_RAMFS_FILES 64
static struct vfs_node ramfs_node_pool[MAX_RAMFS_NODES];
static ramfs_node_data_t ramfs_data_pool[MAX_RAMFS_NODES]; // One data block per node (wasteful but safe)
static ramfs_file_data_t file_data_pool[MAX_RAMFS_FILES];  // File metadata pool
static int ramfs_node_count = 0;
static int file_data_count = 0;

// Forward declaration
int ramfs_mkdir_op(struct vfs_node *parent, const char *name);

// Helper to create a new vfs_node from static pool
static struct vfs_node* ramfs_alloc_node(const char *name, int flags) {
    if (ramfs_node_count >= MAX_RAMFS_NODES) {
        return NULL;  // Pool exhausted
    }
    
    int index = ramfs_node_count++;
    struct vfs_node *node = &ramfs_node_pool[index];
    ramfs_node_data_t *data = &ramfs_data_pool[index];
    
    // Clear node
    for (int i = 0; i < (int)sizeof(struct vfs_node); i++) ((char*)node)[i] = 0;
    
    // Clear data
    for (int i = 0; i < RAMFS_MAX_CHILDREN; i++) {
        data->children[i] = NULL;
        data->child_names[i][0] = '\0';
    }
    
    // Copy name
    int i = 0;
    while(name[i] && i < 255) {
        node->name[i] = name[i];
        i++;
    }
    node->name[i] = '\0';
    
    node->flags = flags;
    node->size = 0;
    
    // Assign data block if directory
    if (flags & VFS_DIRECTORY) {
        node->fs_data = (void*)data;
    } else {
        node->fs_data = NULL; // Will be allocated on first write
    }
    
    return node;
}

/* Write data to a ramfs file */
static int ramfs_write(struct vfs_node *node, uint64_t offset, uint32_t size, const uint8_t *data) {
    if (!node || (node->flags & VFS_DIRECTORY)) {
        return -1;
    }
    
    ramfs_file_data_t *file_data = (ramfs_file_data_t*)node->fs_data;
    
    /* Allocate from file_data pool on first write */
    if (!file_data) {
        if (file_data_count >= MAX_RAMFS_FILES) {
            kprintf("ramfs: File data pool exhausted\n", 0xFFFF0000);
            return -1;
        }
        
        file_data = &file_data_pool[file_data_count++];
        file_data->num_pages = 0;
        for (int i = 0; i < 128; i++) {
            file_data->pages[i] = NULL;
        }
        node->fs_data = (void*)file_data;
    }
    
    /* Calculate which pages we need */
    uint32_t start_page = offset / 4096;
    uint32_t end_page = (offset + size - 1) / 4096;
    
    /* Allocate necessary pages */
    for (uint32_t page_idx = file_data->num_pages; page_idx <= end_page && page_idx < 128; page_idx++) {
        uint64_t page_addr = pfa_alloc_low();
        if (!page_addr) {
            kprintf("ramfs: Failed to allocate file page %u\n", 0xFFFF0000, page_idx);
            return -1;
        }
        file_data->pages[page_idx] = (uint8_t*)phys_to_virt(page_addr);
    }
    
    /* Update page count */
    if (end_page >= file_data->num_pages) {
        file_data->num_pages = end_page + 1;
    }
    
    /* Write data across pages */
    uint32_t written = 0;
    uint32_t remaining = size;
    uint64_t current_offset = offset;
    
    while (remaining > 0) {
        uint32_t page_idx = current_offset / 4096;
        uint32_t page_offset = current_offset % 4096;
        uint32_t bytes_to_write = (4096 - page_offset < remaining) ? (4096 - page_offset) : remaining;
        
        if (page_idx >= file_data->num_pages || !file_data->pages[page_idx]) {
            kprintf("ramfs: Invalid page index %u\n", 0xFFFF0000, page_idx);
            break;
        }
        
        /* Write to this page */
        for (uint32_t i = 0; i < bytes_to_write; i++) {
            file_data->pages[page_idx][page_offset + i] = data[written + i];
        }
        
        written += bytes_to_write;
        remaining -= bytes_to_write;
        current_offset += bytes_to_write;
    }
    
    /* Update size if necessary */
    if (offset + written > node->size) {
        node->size = offset + written;
    }
    
    return written;
}

/* Read data from a ramfs file */
static int ramfs_read(struct vfs_node *node, uint64_t offset, uint32_t size, uint8_t *buffer) {
    if (!node || (node->flags & VFS_DIRECTORY)) {
        return -1;
    }
    
    ramfs_file_data_t *file_data = (ramfs_file_data_t*)node->fs_data;
    if (!file_data) {
        return 0; /* Empty file */
    }
    
    /* Don't read past end of file */
    if (offset >= node->size) {
        return 0;
    }
    
    if (offset + size > node->size) {
        size = node->size - offset;
    }
    
    /* Read data across pages */
    uint32_t read_count = 0;
    uint32_t remaining = size;
    uint64_t current_offset = offset;
    
    while (remaining > 0 && current_offset < node->size) {
        uint32_t page_idx = current_offset / 4096;
        uint32_t page_offset = current_offset % 4096;
        uint32_t bytes_to_read = (4096 - page_offset < remaining) ? (4096 - page_offset) : remaining;
        
        if (page_idx >= file_data->num_pages || !file_data->pages[page_idx]) {
            break;
        }
        
        /* Read from this page */
        for (uint32_t i = 0; i < bytes_to_read; i++) {
            buffer[read_count + i] = file_data->pages[page_idx][page_offset + i];
        }
        
        read_count += bytes_to_read;
        remaining -= bytes_to_read;
        current_offset += bytes_to_read;
    }
    
    return read_count;
}

static struct vfs_node* ramfs_finddir(struct vfs_node *node, const char *name) {
    if (!node || !(node->flags & VFS_DIRECTORY)) return NULL;
    ramfs_node_data_t *data = (ramfs_node_data_t*)node->fs_data;
    if (!data) return NULL;

    for (int i = 0; i < RAMFS_MAX_CHILDREN; i++) {
        if (data->children[i]) {
            // Compare names
            const char *n1 = name;
            const char *n2 = data->child_names[i];
            int match = 1;
            while (*n1 && *n2) {
                if (*n1 != *n2) { match = 0; break; }
                n1++; n2++;
            }
            if (match && *n1 == '\0' && *n2 == '\0') {
                return data->children[i];
            }
        }
    }
    return NULL;
}

static struct vfs_node* ramfs_readdir(struct vfs_node *node, uint32_t index) {
    if (!node || !(node->flags & VFS_DIRECTORY)) return NULL;
    ramfs_node_data_t *data = (ramfs_node_data_t*)node->fs_data;
    if (!data) return NULL;
    
    int count = 0;
    for (int i = 0; i < RAMFS_MAX_CHILDREN; i++) {
        if (data->children[i]) {
            if (count == (int)index) {
                return data->children[i];
            }
            count++;
        }
    }
    return NULL;
}

static int ramfs_create(struct vfs_node *parent, const char *name, uint32_t flags);
int ramfs_mkdir_op(struct vfs_node *parent, const char *name);

static int ramfs_create(struct vfs_node *parent, const char *name, uint32_t flags) {
    if (!parent || !(parent->flags & VFS_DIRECTORY) || !parent->fs_data) return -1;
    
    ramfs_node_data_t *data = (ramfs_node_data_t*)parent->fs_data;
    
    // Find free slot
    int slot = -1;
    for (int i = 0; i < RAMFS_MAX_CHILDREN; i++) {
        if (!data->children[i]) {
            slot = i;
            break;
        }
    }
    
    if (slot == -1) return -1; // Directory full
    
    struct vfs_node *new_node = ramfs_alloc_node(name, flags);
    if (!new_node) return -1;
    
    // Link operations
    new_node->finddir = ramfs_finddir;
    new_node->readdir = ramfs_readdir;
    new_node->create = ramfs_create;
    new_node->mkdir = ramfs_mkdir_op;
    new_node->read = ramfs_read;
    new_node->write = ramfs_write;

    data->children[slot] = new_node;
    
    // Copy name to parent list too
    int k = 0;
    while(name[k] && k < 31) {
        data->child_names[slot][k] = name[k];
        k++;
    }
    data->child_names[slot][k] = '\0';
    
    return 0;
}

// wrapper for mkdir op
int ramfs_mkdir_op(struct vfs_node *parent, const char *name) {
    return ramfs_create(parent, name, VFS_DIRECTORY);
}

static int ramfs_mount_op(const char *device, struct mount_point *mp) {
    (void)device; // Unused for ramfs
    
    struct vfs_node *root = ramfs_alloc_node("/", VFS_DIRECTORY);
    
    if (!root) return -1;
    
    root->inode = 0;
    
    // Populate function pointers
    root->readdir = ramfs_readdir;
    root->finddir = ramfs_finddir;
    root->create = ramfs_create;
    root->mkdir = ramfs_mkdir_op;
    root->read = ramfs_read;
    root->write = ramfs_write;
    
    mp->root = root;
    mp->fs_private = NULL;
    
    return 0;
}

static int ramfs_unmount_op(struct mount_point *mp) {
    return 0;
}

void ramfs_register(void) {
    vfs_register_filesystem("ramfs", ramfs_mount_op, ramfs_unmount_op);
}
