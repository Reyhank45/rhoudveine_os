#pragma once

#include <stdint.h>

/* Callback for tar extraction - called for each file/directory in archive
 * filename: path in tar (null-terminated)
 * data: file data pointer (NULL for directories)
 * size: file size
 * userdata: user-provided context
 * Returns 0 on success, non-zero to stop extraction
 */
typedef int (*tar_extract_fn)(const char *filename, const uint8_t *data, 
                               uint64_t size, void *userdata);

/* Extract all files from tar archive
 * tar_data: pointer to tar archive in memory
 * tar_size: size of tar archive
 * callback: function called for each file/directory
 * userdata: user context passed to callback
 * Returns 0 on success, non-zero on error
 */
int tar_extract_all(const uint8_t *tar_data, uint64_t tar_size, 
                    tar_extract_fn callback, void *userdata);
