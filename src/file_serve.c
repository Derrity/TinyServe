#include "file_serve.h"
#include "http_parser.h"
#include "http_response.h"
#include "server.h"
#include "mime.h"
#include "path_utils.h"
#include "range.h"
#include "auth.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

/* Write request that tracks its owning file context for chained reads. */
typedef struct {
    uv_write_t req;
    uv_buf_t   buf;
    ts_file_ctx_t *fctx;
} ts_file_write_t;

/* Forward declarations */
static void on_file_open(uv_fs_t *req);
static void on_file_read(uv_fs_t *req);
static void on_file_chunk_written(uv_write_t *req, int status);
static void on_file_close(uv_fs_t *req);
static void file_read_next(ts_file_ctx_t *fctx);
static void file_start_close(ts_file_ctx_t *fctx);
static void file_ctx_free(ts_file_ctx_t *fctx);

/* ── Free file context resources (does NOT signal response end) ── */
static void file_ctx_free(ts_file_ctx_t *fctx) {
    ts_client_t *client = fctx->client;
    free(fctx->read_buf);
    free(fctx);
    client->file_ctx = NULL;
}

/* ── Async close callback ── */
static void on_file_close(uv_fs_t *req) {
    ts_file_ctx_t *fctx = req->data;
    uv_fs_req_cleanup(req);
    fctx->fd = -1;
    ts_client_t *client = fctx->client;
    file_ctx_free(fctx);
    ts_client_response_end(client);
}

/* ── Begin async file close ── */
static void file_start_close(ts_file_ctx_t *fctx) {
    if (fctx->fd >= 0) {
        fctx->fs_req.data = fctx;
        uv_fs_close(fctx->client->loop, &fctx->fs_req, fctx->fd,
                     on_file_close);
    } else {
        ts_client_t *client = fctx->client;
        file_ctx_free(fctx);
        ts_client_response_end(client);
    }
}

/* ── Write completion callback — triggers next read ── */
static void on_file_chunk_written(uv_write_t *req, int status) {
    ts_file_write_t *fwr = (ts_file_write_t *)req;
    ts_file_ctx_t *fctx = fwr->fctx;
    ts_client_t *client = fctx->client;

    free(fwr->buf.base);
    free(fwr);

    /* Decrement pending write counter */
    client->pending_writes--;

    if (status < 0) {
        LOG_ERROR("file write error: %s", uv_strerror(status));
        client->req.keep_alive = 0;  /* response is corrupt */
        file_start_close(fctx);
        return;
    }

    if (client->closing) {
        file_start_close(fctx);
        return;
    }

    file_read_next(fctx);
}

/* ── Async read callback ── */
static void on_file_read(uv_fs_t *req) {
    ts_file_ctx_t *fctx = req->data;
    ssize_t nread = req->result;
    uv_fs_req_cleanup(req);

    if (nread <= 0) {
        if (nread < 0)
            LOG_ERROR("file read error: %s", uv_strerror((int)nread));
        fctx->client->req.keep_alive = 0;  /* response may be truncated */
        file_start_close(fctx);
        return;
    }

    if (fctx->client->closing) {
        file_start_close(fctx);
        return;
    }

    ts_file_write_t *fwr = malloc(sizeof(*fwr));
    if (!fwr) {
        fctx->client->req.keep_alive = 0;
        file_start_close(fctx);
        return;
    }
    fwr->buf.base = malloc((size_t)nread);
    if (!fwr->buf.base) {
        free(fwr);
        fctx->client->req.keep_alive = 0;
        file_start_close(fctx);
        return;
    }
    memcpy(fwr->buf.base, fctx->read_buf, (size_t)nread);
    fwr->buf.len = (size_t)nread;
    fwr->fctx = fctx;

    /* Track this write */
    fctx->client->pending_writes++;

    uv_write(&fwr->req, (uv_stream_t *)&fctx->client->handle,
             &fwr->buf, 1, on_file_chunk_written);

    fctx->offset += nread;
    fctx->remaining -= nread;
}

/* ── Issue the next async read (or advance to next range / finish) ── */
static void file_read_next(ts_file_ctx_t *fctx) {
    if (fctx->remaining <= 0) {
        /* Current range exhausted */
        if (fctx->is_multipart &&
            fctx->current_range + 1 < fctx->range_count) {
            fctx->current_range++;
            int i = fctx->current_range;

            char part_hdr[512];
            int n = snprintf(part_hdr, sizeof(part_hdr),
                "\r\n--%s\r\n"
                "Content-Type: %s\r\n"
                "Content-Range: bytes %lld-%lld/%lld\r\n"
                "\r\n",
                fctx->boundary, fctx->mime,
                (long long)fctx->ranges[i].start,
                (long long)fctx->ranges[i].end,
                (long long)fctx->file_size);
            ts_response_write(fctx->client, part_hdr, (size_t)n);

            fctx->offset    = fctx->ranges[i].start;
            fctx->remaining = fctx->ranges[i].end - fctx->ranges[i].start + 1;

            file_read_next(fctx);
            return;
        }

        /* All ranges done */
        if (fctx->is_multipart) {
            char final[128];
            int n = snprintf(final, sizeof(final),
                             "\r\n--%s--\r\n", fctx->boundary);
            ts_response_write(fctx->client, final, (size_t)n);
        }

        file_start_close(fctx);
        return;
    }

    int64_t to_read = fctx->remaining < TS_SEND_BUF_SIZE
                          ? fctx->remaining : TS_SEND_BUF_SIZE;
    uv_buf_t iov = uv_buf_init(fctx->read_buf, (unsigned int)to_read);
    fctx->fs_req.data = fctx;
    uv_fs_read(fctx->client->loop, &fctx->fs_req, fctx->fd,
               &iov, 1, fctx->offset, on_file_read);
}

/* ── Async open callback — send headers then start reading ── */
static void on_file_open(uv_fs_t *req) {
    ts_file_ctx_t *fctx = req->data;
    ts_client_t  *client = fctx->client;

    if (req->result < 0) {
        uv_fs_req_cleanup(req);
        ts_response_send_404(client, fctx->is_head);
        /* ts_response_send_404 sets response_done via ts_response_send.
         * Just free the file context. */
        fctx->fd = -1;
        file_ctx_free(fctx);
        return;
    }

    fctx->fd = (uv_file)req->result;
    uv_fs_req_cleanup(req);

    if (client->closing) {
        file_start_close(fctx);
        return;
    }

    /* Parse Range header */
    const char *range_hdr = ts_request_header(&client->req, "Range");

    if (!range_hdr) {
        /* Full file — 200 */
        ts_response_send_headers(client, 200, fctx->mime,
                                 fctx->file_size,
                                 "Accept-Ranges: bytes\r\n");
        fctx->offset    = 0;
        fctx->remaining = fctx->file_size;
        fctx->range_count = 0;
    } else {
        int rc = ts_range_parse(range_hdr, fctx->file_size,
                                fctx->ranges, TS_MAX_RANGES,
                                &fctx->range_count);

        if (rc == -1) {
            /* Malformed range — serve full file */
            ts_response_send_headers(client, 200, fctx->mime,
                                     fctx->file_size,
                                     "Accept-Ranges: bytes\r\n");
            fctx->offset    = 0;
            fctx->remaining = fctx->file_size;
            fctx->range_count = 0;
        } else if (rc == -2) {
            /* Not satisfiable */
            ts_response_send_416(client, fctx->file_size, fctx->is_head);
            file_ctx_free(fctx);
            return;
        } else if (fctx->range_count == 1) {
            /* Single range — 206 */
            char extra[256];
            snprintf(extra, sizeof(extra),
                     "Accept-Ranges: bytes\r\n"
                     "Content-Range: bytes %lld-%lld/%lld\r\n",
                     (long long)fctx->ranges[0].start,
                     (long long)fctx->ranges[0].end,
                     (long long)fctx->file_size);
            int64_t part_len =
                fctx->ranges[0].end - fctx->ranges[0].start + 1;

            ts_response_send_headers(client, 206, fctx->mime,
                                     part_len, extra);
            fctx->offset    = fctx->ranges[0].start;
            fctx->remaining = part_len;
        } else {
            /* Multiple ranges — multipart/byteranges */
            ts_range_boundary(fctx->boundary, sizeof(fctx->boundary));
            int64_t total_len = ts_range_multipart_size(
                fctx->ranges, fctx->range_count, fctx->mime,
                fctx->file_size, fctx->boundary);

            char ct[128];
            snprintf(ct, sizeof(ct),
                     "multipart/byteranges; boundary=%s", fctx->boundary);

            ts_response_send_headers(client, 206, ct, total_len,
                                     "Accept-Ranges: bytes\r\n");

            fctx->is_multipart  = 1;
            fctx->current_range = 0;

            /* First part header — only write if not HEAD */
            if (!fctx->is_head) {
                char part_hdr[512];
                int n = snprintf(part_hdr, sizeof(part_hdr),
                    "\r\n--%s\r\n"
                    "Content-Type: %s\r\n"
                    "Content-Range: bytes %lld-%lld/%lld\r\n"
                    "\r\n",
                    fctx->boundary, fctx->mime,
                    (long long)fctx->ranges[0].start,
                    (long long)fctx->ranges[0].end,
                    (long long)fctx->file_size);
                ts_response_write(client, part_hdr, (size_t)n);
            }

            fctx->offset    = fctx->ranges[0].start;
            fctx->remaining =
                fctx->ranges[0].end - fctx->ranges[0].start + 1;
        }
    }

    /* HEAD — no body, just close the file */
    if (fctx->is_head) {
        file_start_close(fctx);
        return;
    }

    file_read_next(fctx);
}

/* ── HTML escape a string for safe insertion into HTML text content ── */
static size_t html_escape(const char *src, char *dst, size_t dst_size)
{
    size_t di = 0;
    for (size_t i = 0; src[i] && di + 6 < dst_size; i++) {
        switch (src[i]) {
        case '&':  memcpy(dst + di, "&amp;", 5);  di += 5; break;
        case '<':  memcpy(dst + di, "&lt;", 4);   di += 4; break;
        case '>':  memcpy(dst + di, "&gt;", 4);   di += 4; break;
        case '"':  memcpy(dst + di, "&quot;", 6);  di += 6; break;
        case '\'': memcpy(dst + di, "&#39;", 5);   di += 5; break;
        default:   dst[di++] = src[i]; break;
        }
    }
    dst[di] = '\0';
    return di;
}

/* ── URL-encode a path component for use in href attributes ── */
static size_t url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t di = 0;
    for (size_t i = 0; src[i] && di + 3 < dst_size; i++) {
        unsigned char c = (unsigned char)src[i];
        /* RFC 3986 unreserved characters + '/' for paths */
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~' || c == '/') {
            dst[di++] = (char)c;
        } else {
            dst[di++] = '%';
            dst[di++] = hex[c >> 4];
            dst[di++] = hex[c & 0x0F];
        }
    }
    dst[di] = '\0';
    return di;
}

/* ── Generate and send directory listing (synchronous HTML build) ── */
static void send_directory_listing(ts_client_t *client, const char *fs_path,
                                   const char *url_path) {
    int is_head = (strcmp(client->req.method, "HEAD") == 0);

    DIR *dir = opendir(fs_path);
    if (!dir) {
        ts_response_send_404(client, is_head);
        return;
    }

    /* Build HTML in a dynamic buffer */
    size_t cap = 4096;
    size_t len = 0;
    char *html = malloc(cap);
    if (!html) {
        closedir(dir);
        /* Send a simple error and signal done */
        ts_response_send(client, 500, "text/plain",
                         "Internal Server Error", 21, NULL, is_head);
        return;
    }

#define APPEND(s) do { \
    size_t _sl = strlen(s); \
    while (len + _sl + 1 > cap) { cap *= 2; html = realloc(html, cap); } \
    memcpy(html + len, s, _sl); len += _sl; \
} while (0)

#define APPENDF(...) do { \
    char _tmp[2048]; \
    int _n = snprintf(_tmp, sizeof(_tmp), __VA_ARGS__); \
    if (_n > 0) { \
        while (len + (size_t)_n + 1 > cap) { cap *= 2; html = realloc(html, cap); } \
        memcpy(html + len, _tmp, (size_t)_n); len += (size_t)_n; \
    } \
} while (0)

    /* HTML-escape the URL path for display in title and heading */
    char escaped_path[TS_MAX_PATH * 6];
    html_escape(url_path, escaped_path, sizeof(escaped_path));

    APPENDF("<!DOCTYPE html>\n<html><head><title>Index of %s</title>\n",
            escaped_path);
    APPEND("<style>body{font-family:monospace;margin:2em}"
           "a{text-decoration:none}a:hover{text-decoration:underline}"
           "</style>\n");
    APPENDF("</head><body>\n<h1>Index of %s</h1>\n<hr><pre>\n", escaped_path);

    /* Parent directory link (unless at root) */
    if (strcmp(url_path, "/") != 0)
        APPEND("<a href=\"../\">../</a>\n");

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;

        char entry_path[TS_MAX_PATH];
        snprintf(entry_path, sizeof(entry_path), "%s/%s",
                 fs_path, entry->d_name);
        struct stat st;
        if (stat(entry_path, &st) != 0)
            continue;

        /* HTML-escape the display name, URL-encode the href */
        char escaped_name[1024];
        char encoded_href[1024];
        html_escape(entry->d_name, escaped_name, sizeof(escaped_name));
        url_encode(entry->d_name, encoded_href, sizeof(encoded_href));

        if (S_ISDIR(st.st_mode)) {
            APPENDF("<a href=\"%s/\">%s/</a>\n",
                    encoded_href, escaped_name);
        } else {
            APPENDF("<a href=\"%s\">%s</a>  (%lld bytes)\n",
                     encoded_href, escaped_name, (long long)st.st_size);
        }
    }

    APPEND("</pre><hr>\n<p>tinyserve</p>\n</body></html>\n");

#undef APPEND
#undef APPENDF

    closedir(dir);

    if (is_head) {
        ts_response_send_headers(client, 200,
                                 "text/html; charset=utf-8", (int64_t)len,
                                 NULL);
        free(html);
        ts_client_response_end(client);
    } else {
        ts_response_send(client, 200,
                         "text/html; charset=utf-8", html, len,
                         NULL, 0);
        free(html);
        /* ts_response_send sets response_done */
    }
}

/* ── Begin async file serving (allocate context, open file) ── */
static void serve_file_async(ts_client_t *client, const char *fs_path,
                             int64_t file_size) {
    int is_head = (strcmp(client->req.method, "HEAD") == 0);

    ts_file_ctx_t *fctx = calloc(1, sizeof(*fctx));
    if (!fctx) {
        ts_response_send_404(client, is_head);
        return;
    }

    fctx->client    = client;
    fctx->fd        = -1;
    fctx->file_size = file_size;
    fctx->is_head   = is_head;
    snprintf(fctx->mime, sizeof(fctx->mime), "%s", ts_mime_type(fs_path));

    fctx->read_buf = malloc(TS_SEND_BUF_SIZE);
    if (!fctx->read_buf) {
        free(fctx);
        ts_response_send_404(client, is_head);
        return;
    }

    client->file_ctx   = fctx;
    fctx->fs_req.data  = fctx;

    int r = uv_fs_open(client->loop, &fctx->fs_req, fs_path,
                       O_RDONLY, 0, on_file_open);
    if (r < 0) {
        LOG_ERROR("uv_fs_open submit failed: %s", uv_strerror(r));
        client->file_ctx = NULL;
        free(fctx->read_buf);
        free(fctx);
        ts_response_send_404(client, is_head);
    }
}

/* ── Main entry point ── */
void ts_file_serve(ts_client_t *client) {
    ts_request_t *req = &client->req;
    ts_config_t  *cfg = client->config;
    int is_head = (strcmp(req->method, "HEAD") == 0);

    /* Method check: only GET and HEAD */
    if (strcmp(req->method, "GET") != 0 && !is_head) {
        ts_response_send(client, 405,
                         "text/plain", "Method Not Allowed", 18,
                         "Allow: GET, HEAD\r\n", 0);
        return;
    }

    /* Auth check */
    if (!ts_auth_check(req, cfg)) {
        ts_response_send_401(client, ts_auth_basic_enabled(cfg), is_head);
        return;
    }

    /* Path resolution */
    char fs_path[TS_MAX_PATH];
    if (ts_path_resolve(cfg->root_dir, req->path,
                        fs_path, sizeof(fs_path)) != 0) {
        ts_response_send_404(client, is_head);
        return;
    }

    /* Stat the path */
    struct stat st;
    if (stat(fs_path, &st) != 0) {
        ts_response_send_404(client, is_head);
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        /* Redirect to trailing slash if missing */
        size_t plen = strlen(req->path);
        if (plen == 0 || req->path[plen - 1] != '/') {
            /* Build Location header preserving query string.
             * Use the path portion of raw_path (before '?') + '/' + query. */
            char loc[TS_MAX_PATH * 2 + 64];
            if (req->query[0]) {
                snprintf(loc, sizeof(loc), "Location: %s/?%s\r\n",
                         req->path, req->query);
            } else {
                snprintf(loc, sizeof(loc), "Location: %s/\r\n", req->path);
            }
            ts_response_send(client, 301,
                             "text/html", "Redirecting", 11,
                             loc, is_head);
            return;
        }

        /* Check for index.html */
        char index_path[TS_MAX_PATH];
        snprintf(index_path, sizeof(index_path), "%s/index.html", fs_path);

        struct stat idx_st;
        if (stat(index_path, &idx_st) == 0 && S_ISREG(idx_st.st_mode)) {
            serve_file_async(client, index_path, idx_st.st_size);
        } else {
            send_directory_listing(client, fs_path, req->path);
        }
    } else if (S_ISREG(st.st_mode)) {
        serve_file_async(client, fs_path, st.st_size);
    } else {
        ts_response_send_404(client, is_head);
    }
}
