#include "mime.h"

#include <string.h>
#include <ctype.h>

typedef struct {
    const char *ext;
    const char *type;
} mime_entry_t;

static const mime_entry_t mime_table[] = {
    { ".html",  "text/html" },
    { ".htm",   "text/html" },
    { ".css",   "text/css" },
    { ".js",    "application/javascript" },
    { ".json",  "application/json" },
    { ".xml",   "application/xml" },
    { ".txt",   "text/plain" },
    { ".md",    "text/markdown" },
    { ".png",   "image/png" },
    { ".jpg",   "image/jpeg" },
    { ".jpeg",  "image/jpeg" },
    { ".gif",   "image/gif" },
    { ".svg",   "image/svg+xml" },
    { ".ico",   "image/x-icon" },
    { ".webp",  "image/webp" },
    { ".mp4",   "video/mp4" },
    { ".webm",  "video/webm" },
    { ".mp3",   "audio/mpeg" },
    { ".ogg",   "audio/ogg" },
    { ".wav",   "audio/wav" },
    { ".pdf",   "application/pdf" },
    { ".zip",   "application/zip" },
    { ".gz",    "application/gzip" },
    { ".tar",   "application/x-tar" },
    { ".woff",  "font/woff" },
    { ".woff2", "font/woff2" },
    { ".ttf",   "font/ttf" },
    { ".otf",   "font/otf" },
    { ".wasm",  "application/wasm" },
};

static const size_t mime_table_len = sizeof(mime_table) / sizeof(mime_table[0]);

const char *ts_mime_type(const char *path) {
    if (!path) return "application/octet-stream";

    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";

    /* Case-insensitive comparison against known extensions */
    for (size_t i = 0; i < mime_table_len; i++) {
        if (strcasecmp(dot, mime_table[i].ext) == 0)
            return mime_table[i].type;
    }

    return "application/octet-stream";
}
