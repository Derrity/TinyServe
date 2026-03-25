#include "http_parser.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

void ts_request_init(ts_request_t *req)
{
    memset(req, 0, sizeof(*req));
    req->version_major = 1;
    req->version_minor = 1;
    req->buf       = NULL;
    req->buf_len   = 0;
    req->buf_cap   = 0;
    req->parse_pos = 0;
}

/* Ensure the internal buffer can hold at least `need` more bytes. */
static int buf_grow(ts_request_t *req, size_t need)
{
    size_t required = req->buf_len + need;
    if (required <= req->buf_cap)
        return 0;

    size_t new_cap = req->buf_cap ? req->buf_cap : 1024;
    while (new_cap < required)
        new_cap *= 2;

    char *tmp = realloc(req->buf, new_cap);
    if (!tmp) {
        LOG_ERROR("http_parser: out of memory");
        return -1;
    }
    req->buf     = tmp;
    req->buf_cap = new_cap;
    return 0;
}

/* Free all header strings in the request. */
static void free_headers(ts_request_t *req)
{
    for (int i = 0; i < req->header_count; i++) {
        free(req->headers[i].name);
        free(req->headers[i].value);
        req->headers[i].name  = NULL;
        req->headers[i].value = NULL;
    }
    req->header_count = 0;
}

/* Find end of line (\r\n) in buffer starting from `off`. Returns offset of \r,
 * or -1 if not found. */
static int find_crlf(const char *buf, size_t len, size_t off)
{
    for (size_t i = off; i + 1 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n')
            return (int)i;
    }
    return -1;
}

/* Strictly parse a non-negative integer from a string.
 * Rejects: leading/trailing whitespace (except leading spaces already trimmed
 * by header parsing), negative signs, overflow, empty, non-digit chars.
 * Returns 0 on success with value in *out, -1 on error. */
static int strict_parse_size(const char *s, size_t *out)
{
    if (!s || *s == '\0') return -1;

    /* skip leading whitespace (OWS per RFC 7230) */
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '\0') return -1;

    /* reject leading sign */
    if (*s == '-' || *s == '+') return -1;

    /* must start with digit */
    if (*s < '0' || *s > '9') return -1;

    errno = 0;
    char *end = NULL;
    unsigned long long val = strtoull(s, &end, 10);

    /* must consume all remaining non-whitespace */
    while (*end == ' ' || *end == '\t') end++;
    if (*end != '\0') return -1;

    if (errno == ERANGE) return -1;

    /* apply maximum body size limit */
    if (val > (unsigned long long)TS_MAX_BODY_SIZE) return -1;

    *out = (size_t)val;
    return 0;
}

int ts_request_parse(ts_request_t *req, const char *data, size_t len)
{
    /* No new data and no buffered data — nothing to parse */
    if (len == 0 && req->buf_len == 0)
        return 0;

    /* Append incoming data (if any) */
    if (data && len > 0) {
        if (buf_grow(req, len) < 0)
            return -1;
        memcpy(req->buf + req->buf_len, data, len);
        req->buf_len += len;
    }

    /* Enforce total header size limit (before headers are complete) */
    if (!req->headers_complete && req->buf_len > TS_MAX_HEADER_SIZE) {
        LOG_ERROR("http_parser: headers too large (%zu bytes)", req->buf_len);
        return -1;
    }

    size_t pos = req->parse_pos; /* resume from where we left off */

    /* ── 1. Parse request line ── */
    if (req->method[0] == '\0') {
        int eol = find_crlf(req->buf, req->buf_len, 0);
        if (eol < 0)
            return 0; /* need more data */

        /* "METHOD SP PATH SP HTTP/x.y" */
        char *line = req->buf;
        int line_len = eol;

        /* Method */
        char *sp1 = memchr(line, ' ', line_len);
        if (!sp1) {
            LOG_ERROR("http_parser: malformed request line (no method)");
            return -1;
        }
        size_t method_len = sp1 - line;
        if (method_len == 0 || method_len >= TS_MAX_METHOD) {
            LOG_ERROR("http_parser: method too long or empty");
            return -1;
        }
        memcpy(req->method, line, method_len);
        req->method[method_len] = '\0';
        /* Uppercase method */
        for (size_t i = 0; i < method_len; i++)
            req->method[i] = (char)toupper((unsigned char)req->method[i]);

        /* Path */
        char *path_start = sp1 + 1;
        char *sp2 = memchr(path_start, ' ', line_len - (path_start - line));
        if (!sp2) {
            LOG_ERROR("http_parser: malformed request line (no path)");
            return -1;
        }
        size_t raw_path_len = sp2 - path_start;
        if (raw_path_len == 0 || raw_path_len >= TS_MAX_PATH) {
            LOG_ERROR("http_parser: path too long or empty");
            return -1;
        }
        memcpy(req->raw_path, path_start, raw_path_len);
        req->raw_path[raw_path_len] = '\0';

        /* Split path and query at '?' */
        char *qmark = memchr(req->raw_path, '?', raw_path_len);
        if (qmark) {
            size_t path_only_len = qmark - req->raw_path;
            memcpy(req->path, req->raw_path, path_only_len);
            req->path[path_only_len] = '\0';
            strncpy(req->query, qmark + 1, TS_MAX_PATH - 1);
            req->query[TS_MAX_PATH - 1] = '\0';
        } else {
            memcpy(req->path, req->raw_path, raw_path_len);
            req->path[raw_path_len] = '\0';
            req->query[0] = '\0';
        }

        /* HTTP version */
        char *ver_start = sp2 + 1;
        size_t ver_len = line_len - (ver_start - line);
        if (ver_len < 8 || strncmp(ver_start, "HTTP/", 5) != 0) {
            LOG_ERROR("http_parser: bad HTTP version");
            return -1;
        }
        if (sscanf(ver_start + 5, "%d.%d", &req->version_major, &req->version_minor) != 2) {
            LOG_ERROR("http_parser: cannot parse HTTP version");
            return -1;
        }

        pos = (size_t)eol + 2; /* skip past \r\n */
    }

    /* ── 2. Parse headers ── */
    if (!req->headers_complete) {
        for (;;) {
            int eol = find_crlf(req->buf, req->buf_len, pos);
            if (eol < 0)
                return 0; /* need more data */

            /* Empty line = end of headers */
            if ((size_t)eol == pos) {
                req->headers_complete = 1;
                pos = (size_t)eol + 2;
                break;
            }

            /* Parse "Name: Value" */
            char *hline = req->buf + pos;
            size_t hlen = (size_t)eol - pos;

            char *colon = memchr(hline, ':', hlen);
            if (!colon) {
                LOG_ERROR("http_parser: malformed header (no colon)");
                return -1;
            }

            if (req->header_count >= TS_MAX_HEADERS) {
                LOG_ERROR("http_parser: too many headers (max %d)", TS_MAX_HEADERS);
                return -1;
            }

            size_t name_len = colon - hline;
            char *val_start = colon + 1;
            /* Trim leading whitespace from value */
            while (val_start < hline + hlen && *val_start == ' ')
                val_start++;
            size_t val_len = hlen - (val_start - hline);
            /* Trim trailing whitespace from value */
            while (val_len > 0 && (val_start[val_len - 1] == ' ' || val_start[val_len - 1] == '\t'))
                val_len--;

            /* Store as strdup'd copies to avoid dangling pointers on realloc */
            req->headers[req->header_count].name = strndup(hline, name_len);
            req->headers[req->header_count].value = strndup(val_start, val_len);
            if (!req->headers[req->header_count].name || !req->headers[req->header_count].value) {
                LOG_ERROR("http_parser: out of memory for header");
                return -1;
            }
            req->header_count++;

            pos = (size_t)eol + 2;
        }

        /* Determine content_length after headers are parsed.
         * Use strict parsing: reject negatives, overflow, non-digits. */
        const char *cl = ts_request_header(req, "Content-Length");
        if (cl) {
            /* Check for duplicate/conflicting Content-Length headers */
            const char *first_cl = NULL;
            for (int i = 0; i < req->header_count; i++) {
                if (strcasecmp(req->headers[i].name, "Content-Length") == 0) {
                    if (!first_cl)
                        first_cl = req->headers[i].value;
                    else if (strcmp(first_cl, req->headers[i].value) != 0) {
                        LOG_ERROR("http_parser: conflicting Content-Length headers");
                        return -1;
                    }
                }
            }

            size_t parsed_cl;
            if (strict_parse_size(cl, &parsed_cl) != 0) {
                LOG_ERROR("http_parser: invalid Content-Length: '%s'", cl);
                return -1;
            }
            req->content_length = parsed_cl;
        } else {
            req->content_length = 0;
        }
    }

    /* ── 3. Body handling ── */
    if (req->headers_complete && !req->complete) {
        size_t remaining = req->buf_len - pos;

        if (req->content_length > 0) {
            /* Wait for full body */
            if (remaining < req->content_length)
                return 0; /* need more data */

            req->body     = req->buf + pos;
            req->body_len = req->content_length;
            pos += req->content_length;
        } else {
            /* No Content-Length: GET/HEAD have no body */
            req->body     = NULL;
            req->body_len = 0;
        }

        req->complete = 1;

        /* Determine keep-alive */
        const char *conn = ts_request_header(req, "Connection");
        if (conn) {
            if (strcasecmp(conn, "keep-alive") == 0)
                req->keep_alive = 1;
            else if (strcasecmp(conn, "close") == 0)
                req->keep_alive = 0;
            else
                req->keep_alive = (req->version_minor >= 1) ? 1 : 0;
        } else {
            /* HTTP/1.1 defaults to keep-alive, 1.0 defaults to close */
            req->keep_alive = (req->version_minor >= 1) ? 1 : 0;
        }
    }

    req->parse_pos = pos;
    return req->complete ? (int)pos : 0;
}

const char *ts_request_header(const ts_request_t *req, const char *name)
{
    for (int i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->headers[i].name, name) == 0)
            return req->headers[i].value;
    }
    return NULL;
}

void ts_request_reset(ts_request_t *req)
{
    /* Preserve any leftover data after the parsed request (pipelining) */
    size_t leftover = 0;
    if (req->buf_len > req->parse_pos) {
        leftover = req->buf_len - req->parse_pos;
        if (leftover > 0 && req->parse_pos > 0)
            memmove(req->buf, req->buf + req->parse_pos, leftover);
    }

    free_headers(req);

    req->method[0]   = '\0';
    req->path[0]     = '\0';
    req->raw_path[0] = '\0';
    req->query[0]    = '\0';
    req->version_major = 1;
    req->version_minor = 1;
    req->header_count  = 0;
    req->body          = NULL;
    req->body_len      = 0;
    req->content_length = 0;
    req->keep_alive     = 0;
    req->headers_complete = 0;
    req->complete       = 0;

    /* Keep the buffer allocated, set length to leftover bytes */
    req->buf_len   = leftover;
    req->parse_pos = 0;
}

void ts_request_free(ts_request_t *req)
{
    free_headers(req);
    free(req->buf);
    req->buf     = NULL;
    req->buf_len = 0;
    req->buf_cap = 0;
}
