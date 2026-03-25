#ifndef TS_PATH_UTILS_H
#define TS_PATH_UTILS_H

#include <stddef.h>

/* URL-decode a string in place. Returns 0 on success, -1 on invalid encoding. */
int ts_url_decode(const char *src, char *dst, size_t dst_size);

/* Normalize and resolve a request path against a root directory.
 * Writes the full filesystem path to `out`.
 * Returns 0 on success, -1 if path traversal is detected or path is invalid. */
int ts_path_resolve(const char *root, const char *request_path, char *out, size_t out_size);

#endif /* TS_PATH_UTILS_H */
