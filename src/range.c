#include "range.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <stdint.h>

/* Safely accumulate a digit into an int64_t.
 * Returns 0 on success, -1 on overflow. */
static int safe_accum(int64_t *val, int digit)
{
    /* INT64_MAX = 9223372036854775807 */
    if (*val > (INT64_MAX - digit) / 10)
        return -1;
    *val = *val * 10 + digit;
    return 0;
}

int ts_range_parse(const char *header, int64_t file_size,
                   ts_range_t *ranges, int max_ranges, int *count) {
    *count = 0;

    /* Must start with "bytes=" (case-insensitive) */
    if (strncasecmp(header, "bytes=", 6) != 0)
        return -1;

    const char *p = header + 6;

    while (*p) {
        /* trim leading whitespace */
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        int64_t start = -1, end = -1;

        if (*p == '-') {
            /* suffix range: -N */
            p++;
            if (*p < '0' || *p > '9') return -1;
            int64_t suffix = 0;
            while (*p >= '0' && *p <= '9') {
                if (safe_accum(&suffix, *p - '0') != 0)
                    return -1;  /* overflow */
                p++;
            }
            if (suffix == 0) return -1;
            if (suffix > file_size) suffix = file_size;
            start = file_size - suffix;
            end = file_size - 1;
        } else if (*p >= '0' && *p <= '9') {
            /* start-end or start- */
            start = 0;
            while (*p >= '0' && *p <= '9') {
                if (safe_accum(&start, *p - '0') != 0)
                    return -1;  /* overflow */
                p++;
            }
            if (*p != '-') return -1;
            p++;
            /* trim whitespace after dash */
            while (*p == ' ' || *p == '\t') p++;

            if (*p >= '0' && *p <= '9') {
                end = 0;
                while (*p >= '0' && *p <= '9') {
                    if (safe_accum(&end, *p - '0') != 0)
                        return -1;  /* overflow */
                    p++;
                }
                if (end >= file_size)
                    end = file_size - 1;
            } else {
                /* open-ended: start to EOF */
                end = file_size - 1;
            }

            if (start >= file_size) {
                /* skip to next range spec */
                while (*p == ' ' || *p == '\t') p++;
                if (*p == ',') { p++; continue; }
                break;
            }
            if (start > end) {
                while (*p == ' ' || *p == '\t') p++;
                if (*p == ',') { p++; continue; }
                break;
            }
        } else {
            return -1;
        }

        /* Validate */
        if (start < 0 || start >= file_size) {
            while (*p == ' ' || *p == '\t') p++;
            if (*p == ',') { p++; continue; }
            break;
        }

        ranges[*count].start = start;
        ranges[*count].end = end;
        (*count)++;

        if (*count >= max_ranges) break;

        /* skip to next comma */
        while (*p == ' ' || *p == '\t') p++;
        if (*p == ',') {
            p++;
        } else {
            break;
        }
    }

    if (*count == 0)
        return -2;

    return 0;
}

void ts_range_boundary(char *buf, size_t buf_size) {
    static int seeded = 0;
    if (!seeded) {
        srand((unsigned int)time(NULL));
        seeded = 1;
    }

    const char *prefix = "tinyserve_";
    size_t plen = strlen(prefix);
    size_t hex_len = 16;

    if (buf_size < plen + hex_len + 1) {
        buf[0] = '\0';
        return;
    }

    memcpy(buf, prefix, plen);
    for (size_t i = 0; i < hex_len; i++) {
        int v = rand() & 0x0F;
        buf[plen + i] = "0123456789abcdef"[v];
    }
    buf[plen + hex_len] = '\0';
}

int64_t ts_range_multipart_size(const ts_range_t *ranges, int count,
                                const char *content_type,
                                int64_t file_size,
                                const char *boundary) {
    int64_t total = 0;
    char line[512];

    for (int i = 0; i < count; i++) {
        /* \r\n--BOUNDARY\r\n */
        total += 2 + 2 + (int64_t)strlen(boundary) + 2;

        /* Content-Type: TYPE\r\n */
        int n = snprintf(line, sizeof(line), "Content-Type: %s\r\n", content_type);
        total += n;

        /* Content-Range: bytes START-END/TOTAL\r\n */
        n = snprintf(line, sizeof(line), "Content-Range: bytes %lld-%lld/%lld\r\n",
                     (long long)ranges[i].start, (long long)ranges[i].end,
                     (long long)file_size);
        total += n;

        /* \r\n (blank line before body) */
        total += 2;

        /* body data */
        total += ranges[i].end - ranges[i].start + 1;
    }

    /* \r\n--BOUNDARY--\r\n */
    total += 2 + 2 + (int64_t)strlen(boundary) + 2 + 2;

    return total;
}
