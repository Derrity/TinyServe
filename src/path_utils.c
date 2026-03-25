#include "path_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <sys/stat.h>

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int ts_url_decode(const char *src, char *dst, size_t dst_size) {
    if (!src || !dst || dst_size == 0) return -1;

    size_t di = 0;
    for (size_t si = 0; src[si] != '\0'; si++) {
        if (di >= dst_size - 1) return -1;

        if (src[si] == '%') {
            /* Check that two hex chars follow (bounds safety) */
            if (src[si + 1] == '\0' || src[si + 2] == '\0')
                return -1;
            int h = hex_val(src[si + 1]);
            int l = hex_val(src[si + 2]);
            if (h < 0 || l < 0) return -1;
            char decoded = (char)((h << 4) | l);
            /* Reject null byte injection */
            if (decoded == '\0') return -1;
            dst[di++] = decoded;
            si += 2;
        } else {
            /* '+' is NOT decoded as space in URL paths.
             * ('+' → space only applies to application/x-www-form-urlencoded) */
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
    return 0;
}

/*
 * Normalize a decoded path in-place within `buf`.
 * - Replaces '\\' with '/'
 * - Collapses multiple '/' into one
 * - Resolves '.' and '..' components (never ascending above root)
 * Returns 0 on success, -1 if the path is invalid.
 */
static int normalize_path(char *buf, size_t buf_size) {
    /* Replace backslashes */
    for (size_t i = 0; buf[i]; i++) {
        if (buf[i] == '\\') buf[i] = '/';
    }

    /* Build the normalized path by processing components.
     * Track depth to reject any attempt to go above root. */
    char out[PATH_MAX];
    size_t out_len = 0;
    int depth = 0;

    /* Ensure leading slash */
    if (buf[0] != '/') {
        out[out_len++] = '/';
    }

    const char *p = buf;
    while (*p) {
        /* Skip slashes */
        while (*p == '/') p++;
        if (*p == '\0') break;

        /* Find end of component */
        const char *comp = p;
        while (*p && *p != '/') p++;
        size_t comp_len = (size_t)(p - comp);

        if (comp_len == 1 && comp[0] == '.') {
            /* Current dir — skip */
            continue;
        }

        if (comp_len == 2 && comp[0] == '.' && comp[1] == '.') {
            depth--;
            if (depth < 0) return -1; /* Traversal above root — reject */
            /* Go up one level */
            if (out_len > 1) {
                if (out[out_len - 1] == '/')
                    out_len--;
                while (out_len > 0 && out[out_len - 1] != '/')
                    out_len--;
                if (out_len == 0) out_len = 1;
            }
            continue;
        }

        /* Regular component — append "/component" */
        depth++;
        if (out_len == 0 || out[out_len - 1] != '/') {
            if (out_len >= buf_size - 1) return -1;
            out[out_len++] = '/';
        }
        if (out_len + comp_len >= buf_size - 1) return -1;
        memcpy(out + out_len, comp, comp_len);
        out_len += comp_len;
    }

    /* Ensure at least "/" */
    if (out_len == 0) {
        out[out_len++] = '/';
    }

    out[out_len] = '\0';
    memcpy(buf, out, out_len + 1);
    return 0;
}

int ts_path_resolve(const char *root, const char *request_path,
                    char *out, size_t out_size) {
    if (!root || !out || out_size == 0) return -1;

    /* Resolve canonical root path */
    char real_root[PATH_MAX];
    if (!realpath(root, real_root)) return -1;
    size_t root_len = strlen(real_root);

    /* Remove trailing slash from root (unless root is "/") */
    while (root_len > 1 && real_root[root_len - 1] == '/') {
        real_root[--root_len] = '\0';
    }

    /* Handle NULL or empty request path */
    if (!request_path || request_path[0] == '\0') {
        request_path = "/";
    }

    /* URL-decode the request path */
    char decoded[PATH_MAX];
    if (ts_url_decode(request_path, decoded, sizeof(decoded)) != 0)
        return -1;

    /* Strip query string */
    char *q = strchr(decoded, '?');
    if (q) *q = '\0';

    /* Normalize */
    if (normalize_path(decoded, sizeof(decoded)) != 0)
        return -1;

    /* Build full path: root + normalized */
    char joined[PATH_MAX];
    int n;
    if (decoded[0] == '/' && root_len > 0 && real_root[root_len - 1] == '/') {
        n = snprintf(joined, sizeof(joined), "%s%s", real_root, decoded + 1);
    } else {
        n = snprintf(joined, sizeof(joined), "%s%s", real_root, decoded);
    }
    if (n < 0 || (size_t)n >= sizeof(joined)) return -1;

    /* Resolve the final path with realpath.
     * If the target doesn't exist, we still need to verify the parent exists
     * and the path doesn't escape root. */
    char resolved[PATH_MAX];
    if (realpath(joined, resolved)) {
        /* Path exists — verify it starts with root */
        size_t res_len = strlen(resolved);
        if (res_len < root_len) return -1;
        if (strncmp(resolved, real_root, root_len) != 0) return -1;
        /* Must be exactly root or followed by '/' */
        if (res_len > root_len && resolved[root_len] != '/') return -1;

        if (strlen(resolved) >= out_size) return -1;
        strcpy(out, resolved);
        return 0;
    }

    /* Path doesn't exist — resolve the longest existing prefix to check
     * it's still within root. Walk backwards to find an existing ancestor. */
    char check[PATH_MAX];
    strcpy(check, joined);

    /* Try resolving parent directories until one exists */
    for (;;) {
        char *slash = strrchr(check, '/');
        if (!slash) return -1;
        if (slash == check) {
            /* We're at root "/" */
            check[1] = '\0';
        } else {
            *slash = '\0';
        }

        if (realpath(check, resolved)) {
            size_t res_len = strlen(resolved);
            if (res_len < root_len && strcmp(resolved, real_root) != 0)
                return -1;
            if (strncmp(resolved, real_root, root_len) != 0) return -1;
            if (res_len > root_len && resolved[root_len] != '/') return -1;
            break;
        }

        if (check[0] == '/' && check[1] == '\0') return -1;
    }

    /* The ancestor is within root, assemble the full (non-existent) path */
    if (strlen(joined) >= out_size) return -1;
    strcpy(out, joined);
    return 0;
}
