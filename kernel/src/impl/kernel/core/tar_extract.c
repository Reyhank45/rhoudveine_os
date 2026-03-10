/*
 * Simple TAR extractor for uncompressed tar files
 * Supports basic tar format (ustar format)
 */

#include <stddef.h>
#include <stdint.h>

/* String utilities */
static int my_strcmp(const char *s1, const char *s2) {
    while (*s1 && *s2 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return *s1 - *s2;
}

static int my_strncmp(const char *s1, const char *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i])
            return s1[i] - s2[i];
        if (!s1[i])
            return 0;
    }
    return 0;
}

/* TAR file structure */
struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};

/* Parse octal string */
static uint64_t octal_to_uint64(const char *str, int len) {
    uint64_t result = 0;
    for (int i = 0; i < len && str[i] >= '0' && str[i] <= '7'; i++) {
        result = (result << 3) | (str[i] - '0');
    }
    return result;
}

/* Extract a single file from tar */
typedef int (*tar_extract_fn)(const char *filename, const uint8_t *data, 
                               uint64_t size, void *userdata);

/* Iterate through tar archive and call callback for each file */
int tar_extract_all(const uint8_t *tar_data, uint64_t tar_size, 
                    tar_extract_fn callback, void *userdata) {
    if (!tar_data || tar_size < 512)
        return -1;

    uint64_t offset = 0;

    while (offset + 512 <= tar_size) {
        struct tar_header *hdr = (struct tar_header *)(tar_data + offset);

        /* Check for end of archive (two consecutive 512-byte blocks of zeros) */
        if (hdr->name[0] == '\0') {
            break;
        }

        /* Check magic number */
        if (my_strncmp(hdr->magic, "ustar", 5) != 0) {
            /* Legacy tar format, also check if all zeros */
            int all_zero = 1;
            for (int i = 0; i < 512; i++) {
                if (tar_data[offset + i] != 0) {
                    all_zero = 0;
                    break;
                }
            }
            if (all_zero)
                break;
        }

        /* Parse file size */
        uint64_t file_size = octal_to_uint64(hdr->size, 11);

        /* Move past header */
        offset += 512;

        /* Only process regular files (type flag '0' or '\0') */
        if (hdr->typeflag == '0' || hdr->typeflag == '\0') {
            /* Extract the file */
            if (callback) {
                int result = callback(hdr->name, 
                                    tar_data + offset, 
                                    file_size, 
                                    userdata);
                if (result != 0)
                    return result;
            }
        } else if (hdr->typeflag == '5') {
            /* Directory - also extract */
            if (callback) {
                int result = callback(hdr->name, NULL, 0, userdata);
                if (result != 0)
                    return result;
            }
        }

        /* Move to next file (aligned to 512 bytes) */
        uint64_t padded_size = ((file_size + 511) / 512) * 512;
        offset += padded_size;
    }

    return 0;
}
