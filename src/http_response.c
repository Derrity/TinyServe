#include "http_response.h"
#include "server.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *ts_status_text(int status)
{
    switch (status) {
    case 200: return "OK";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 416: return "Range Not Satisfiable";
    case 500: return "Internal Server Error";
    default:  return "Unknown";
    }
}

/* ── Write completion callback ──
 * Frees the buffer and, if a client is tracked, decrements pending_writes.
 * When pending_writes reaches 0 and response_done is set, fires
 * ts_client_done() to handle keep-alive or close. */
static void on_write_done(uv_write_t *wr, int status)
{
    ts_write_req_t *wreq = (ts_write_req_t *)wr;
    ts_client_t *client = wreq->client;

    if (status < 0)
        LOG_ERROR("write error: %s", uv_strerror(status));

    free(wreq->buf.base);
    free(wreq);

    if (client) {
        client->pending_writes--;
        if (client->pending_writes == 0 && client->response_done) {
            ts_client_done(client);
        }
    }
}

/* ── Queue a tracked write on a client stream ── */
int ts_response_write(ts_client_t *client, const char *data, size_t len)
{
    uv_stream_t *stream = (uv_stream_t *)&client->handle;

    if (client->closing || !uv_is_writable(stream)) {
        LOG_WARN("stream not writable, dropping write");
        return -1;
    }

    ts_write_req_t *wreq = malloc(sizeof(*wreq));
    if (!wreq) {
        LOG_ERROR("out of memory for write request");
        return -1;
    }

    char *copy = malloc(len);
    if (!copy) {
        LOG_ERROR("out of memory for write buffer");
        free(wreq);
        return -1;
    }
    memcpy(copy, data, len);

    wreq->buf = uv_buf_init(copy, (unsigned int)len);
    wreq->client = client;
    client->pending_writes++;

    int r = uv_write(&wreq->req, stream, &wreq->buf, 1, on_write_done);
    if (r < 0) {
        LOG_ERROR("uv_write failed: %s", uv_strerror(r));
        client->pending_writes--;
        free(copy);
        free(wreq);
        return -1;
    }
    return 0;
}

/* ── Send a complete HTTP response ──
 * For HEAD (is_head=1), sends headers with Content-Length but no body.
 * Sets response_done = 1.  ts_client_done() fires from write callback. */
int ts_response_send(ts_client_t *client, int status,
                     const char *content_type, const char *body,
                     size_t body_len, const char *extra_headers,
                     int is_head)
{
    uv_stream_t *stream = (uv_stream_t *)&client->handle;
    int http_minor = client->req.version_minor;
    int keep_alive = client->req.keep_alive;

    if (client->closing || !uv_is_writable(stream))
        return -1;

    const char *reason = ts_status_text(status);
    const char *conn   = keep_alive ? "keep-alive" : "close";

    /* Build header */
    char hdr[4096];
    int hdr_len = snprintf(hdr, sizeof(hdr),
        "HTTP/1.%d %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: %s\r\n"
        "%s"
        "\r\n",
        http_minor, status, reason,
        content_type,
        body_len,
        conn,
        extra_headers ? extra_headers : "");

    if (hdr_len < 0 || (size_t)hdr_len >= sizeof(hdr)) {
        LOG_ERROR("response header too large");
        return -1;
    }

    /* For HEAD: send only headers */
    size_t total = is_head ? (size_t)hdr_len : ((size_t)hdr_len + body_len);

    char *buf = malloc(total);
    if (!buf) {
        LOG_ERROR("out of memory for response");
        return -1;
    }
    memcpy(buf, hdr, (size_t)hdr_len);
    if (!is_head && body && body_len > 0)
        memcpy(buf + hdr_len, body, body_len);

    ts_write_req_t *wreq = malloc(sizeof(*wreq));
    if (!wreq) {
        free(buf);
        return -1;
    }
    wreq->buf = uv_buf_init(buf, (unsigned int)total);
    wreq->client = client;
    client->pending_writes++;
    client->response_done = 1;  /* this is a complete response */

    int r = uv_write(&wreq->req, stream, &wreq->buf, 1, on_write_done);
    if (r < 0) {
        LOG_ERROR("uv_write failed: %s", uv_strerror(r));
        client->pending_writes--;
        free(buf);
        free(wreq);
        return -1;
    }
    return 0;
}

/* ── Send HTTP headers only (before streaming body) ──
 * Does NOT set response_done.  Caller must call ts_client_response_end()
 * after all body writes are queued. */
int ts_response_send_headers(ts_client_t *client, int status,
                             const char *content_type,
                             int64_t content_length,
                             const char *extra_headers)
{
    uv_stream_t *stream = (uv_stream_t *)&client->handle;
    int http_minor = client->req.version_minor;
    int keep_alive = client->req.keep_alive;

    if (client->closing || !uv_is_writable(stream))
        return -1;

    const char *reason = ts_status_text(status);
    const char *conn   = keep_alive ? "keep-alive" : "close";

    char hdr[4096];
    int hdr_len;

    if (content_length >= 0) {
        hdr_len = snprintf(hdr, sizeof(hdr),
            "HTTP/1.%d %d %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %lld\r\n"
            "Connection: %s\r\n"
            "%s"
            "\r\n",
            http_minor, status, reason,
            content_type,
            (long long)content_length,
            conn,
            extra_headers ? extra_headers : "");
    } else {
        hdr_len = snprintf(hdr, sizeof(hdr),
            "HTTP/1.%d %d %s\r\n"
            "Content-Type: %s\r\n"
            "Connection: %s\r\n"
            "%s"
            "\r\n",
            http_minor, status, reason,
            content_type,
            conn,
            extra_headers ? extra_headers : "");
    }

    if (hdr_len < 0 || (size_t)hdr_len >= sizeof(hdr)) {
        LOG_ERROR("response header too large");
        return -1;
    }

    return ts_response_write(client, hdr, (size_t)hdr_len);
}

int ts_response_send_404(ts_client_t *client, int is_head)
{
    return ts_response_send(client, 404, "text/plain",
                            TS_NOT_FOUND_BODY, strlen(TS_NOT_FOUND_BODY),
                            NULL, is_head);
}

int ts_response_send_401(ts_client_t *client, int basic_auth, int is_head)
{
    const char *extra = basic_auth
        ? "WWW-Authenticate: Basic realm=\"tinyserve\"\r\n"
        : NULL;
    return ts_response_send(client, 401, "text/plain",
                            "Unauthorized", 12,
                            extra, is_head);
}

int ts_response_send_416(ts_client_t *client, int64_t file_size, int is_head)
{
    char extra[128];
    snprintf(extra, sizeof(extra),
             "Content-Range: bytes */%lld\r\n", (long long)file_size);
    return ts_response_send(client, 416, "text/plain",
                            "Range Not Satisfiable", 21,
                            extra, is_head);
}
