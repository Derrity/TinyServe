#ifndef TS_HTTP_RESPONSE_H
#define TS_HTTP_RESPONSE_H

#include "tinyserve.h"
#include <uv.h>

/* Get the standard reason phrase for an HTTP status code. */
const char *ts_status_text(int status);

/* Send a complete HTTP response (headers + body).
 * For HEAD requests (is_head=1), sends headers with correct Content-Length
 * but omits the body.  Sets client->response_done = 1. */
int ts_response_send(ts_client_t *client, int status,
                     const char *content_type, const char *body,
                     size_t body_len, const char *extra_headers,
                     int is_head);

/* Send only HTTP headers (before streaming body).
 * Does NOT set response_done — caller must call ts_client_response_end()
 * after all body writes are queued.
 * content_length: value for Content-Length header, -1 to omit. */
int ts_response_send_headers(ts_client_t *client, int status,
                             const char *content_type,
                             int64_t content_length,
                             const char *extra_headers);

/* Send a 404 response with standard body (or headers-only for HEAD). */
int ts_response_send_404(ts_client_t *client, int is_head);

/* Send a 401 response. If basic_auth is true, includes WWW-Authenticate header. */
int ts_response_send_401(ts_client_t *client, int basic_auth, int is_head);

/* Send a 416 Range Not Satisfiable response. */
int ts_response_send_416(ts_client_t *client, int64_t file_size, int is_head);

/* Queue a write on the client's stream.  Increments client->pending_writes.
 * The buffer is copied internally; caller may free data after return. */
int ts_response_write(ts_client_t *client, const char *data, size_t len);

#endif /* TS_HTTP_RESPONSE_H */
