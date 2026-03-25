#include "proxy.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>

/* ── Forward declarations ── */
static void proxy_close_all(ts_proxy_ctx_t *ctx);
static void maybe_close_proxy(ts_proxy_ctx_t *ctx);
static void on_proxy_close(uv_handle_t *handle);
static void on_client_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf);
static void on_upstream_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf);

static void alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
    (void)handle;
    (void)suggested_size;
    buf->base = malloc(TS_READ_BUF_SIZE);
    buf->len = buf->base ? TS_READ_BUF_SIZE : 0;
}

static void write_done_cb(uv_write_t *req, int status)
{
    ts_write_req_t *wr = (ts_write_req_t *)req;
    ts_proxy_ctx_t *ctx = req->data;

    free(wr->buf.base);
    free(wr);

    if (status < 0) {
        LOG_ERROR("Proxy write failed: %s", uv_strerror(status));
        proxy_close_all(ctx);
        return;
    }

    if (ctx->closing) return;

    /* Backpressure: resume reading on the opposite side if it was paused. */
    uv_stream_t *target = req->handle;
    if (target == (uv_stream_t *)&ctx->upstream && !ctx->client_reading &&
        !ctx->client_eof) {
        uv_read_start((uv_stream_t *)&ctx->client, alloc_cb, on_client_read);
        ctx->client_reading = 1;
    } else if (target == (uv_stream_t *)&ctx->client && !ctx->upstream_reading &&
               !ctx->upstream_eof) {
        uv_read_start((uv_stream_t *)&ctx->upstream, alloc_cb, on_upstream_read);
        ctx->upstream_reading = 1;
    }
}

static void proxy_write(uv_stream_t *dest, ts_proxy_ctx_t *ctx,
                         const char *data, ssize_t len)
{
    if (ctx->closing) return;

    ts_write_req_t *wr = malloc(sizeof(*wr));
    if (!wr) {
        proxy_close_all(ctx);
        return;
    }
    wr->buf.base = malloc((size_t)len);
    if (!wr->buf.base) {
        free(wr);
        proxy_close_all(ctx);
        return;
    }
    memcpy(wr->buf.base, data, (size_t)len);
    wr->buf.len = (size_t)len;
    wr->req.data = ctx;
    wr->client = NULL;  /* not used for proxy tracking */

    int r = uv_write(&wr->req, dest, &wr->buf, 1, write_done_cb);
    if (r < 0) {
        LOG_ERROR("uv_write failed: %s", uv_strerror(r));
        free(wr->buf.base);
        free(wr);
        proxy_close_all(ctx);
        return;
    }

    /* Backpressure: if write queue is large, pause reading from the source side. */
    size_t qsize = dest->write_queue_size;
    if (qsize > TS_READ_BUF_SIZE * 4) {
        if (dest == (uv_stream_t *)&ctx->upstream && ctx->client_reading) {
            uv_read_stop((uv_stream_t *)&ctx->client);
            ctx->client_reading = 0;
        } else if (dest == (uv_stream_t *)&ctx->client && ctx->upstream_reading) {
            uv_read_stop((uv_stream_t *)&ctx->upstream);
            ctx->upstream_reading = 0;
        }
    }
}

/* ── Shutdown completion callbacks ── */
static void on_upstream_shutdown(uv_shutdown_t *req, int status)
{
    ts_proxy_ctx_t *ctx = req->data;
    (void)status;
    ctx->upstream_write_shut = 1;
    LOG_INFO("Proxy: upstream write shutdown complete");
    maybe_close_proxy(ctx);
}

static void on_client_shutdown(uv_shutdown_t *req, int status)
{
    ts_proxy_ctx_t *ctx = req->data;
    (void)status;
    ctx->client_write_shut = 1;
    LOG_INFO("Proxy: client write shutdown complete");
    maybe_close_proxy(ctx);
}

/* ── Check if we should close both handles ── */
static void maybe_close_proxy(ts_proxy_ctx_t *ctx)
{
    if (ctx->closing) return;

    /* Close when both sides have received EOF */
    if (ctx->client_eof && ctx->upstream_eof) {
        proxy_close_all(ctx);
    }
}

/* ── Client → Upstream ── */
static void on_client_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    ts_proxy_ctx_t *ctx = stream->data;

    if (nread > 0) {
        proxy_write((uv_stream_t *)&ctx->upstream, ctx, buf->base, nread);
    } else if (nread < 0) {
        if (nread != UV_EOF)
            LOG_WARN("Client read error: %s", uv_strerror((int)nread));
        else
            LOG_INFO("Proxy: client EOF");

        ctx->client_eof = 1;
        if (ctx->client_reading) {
            uv_read_stop((uv_stream_t *)&ctx->client);
            ctx->client_reading = 0;
        }

        /* Half-close: shutdown upstream write side so upstream gets FIN */
        if (ctx->upstream_connected && !ctx->upstream_write_shut && !ctx->closing) {
            ctx->upstream_shutdown_req.data = ctx;
            int r = uv_shutdown(&ctx->upstream_shutdown_req,
                                (uv_stream_t *)&ctx->upstream,
                                on_upstream_shutdown);
            if (r < 0) {
                ctx->upstream_write_shut = 1;
                maybe_close_proxy(ctx);
            }
        } else {
            maybe_close_proxy(ctx);
        }
    }

    if (buf->base) free(buf->base);
}

/* ── Upstream → Client ── */
static void on_upstream_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    ts_proxy_ctx_t *ctx = stream->data;

    if (nread > 0) {
        proxy_write((uv_stream_t *)&ctx->client, ctx, buf->base, nread);
    } else if (nread < 0) {
        if (nread != UV_EOF)
            LOG_WARN("Upstream read error: %s", uv_strerror((int)nread));
        else
            LOG_INFO("Proxy: upstream EOF");

        ctx->upstream_eof = 1;
        if (ctx->upstream_reading) {
            uv_read_stop((uv_stream_t *)&ctx->upstream);
            ctx->upstream_reading = 0;
        }

        /* Half-close: shutdown client write side so client gets FIN */
        if (!ctx->client_write_shut && !ctx->closing) {
            ctx->client_shutdown_req.data = ctx;
            int r = uv_shutdown(&ctx->client_shutdown_req,
                                (uv_stream_t *)&ctx->client,
                                on_client_shutdown);
            if (r < 0) {
                ctx->client_write_shut = 1;
                maybe_close_proxy(ctx);
            }
        } else {
            maybe_close_proxy(ctx);
        }
    }

    if (buf->base) free(buf->base);
}

/* ── uv_close callback — free ctx when both handles are closed ── */
static void on_proxy_close(uv_handle_t *handle)
{
    ts_proxy_ctx_t *ctx = handle->data;
    if (!ctx) return;

    ctx->close_count++;
    if (ctx->close_count >= 2) {
        LOG_INFO("Proxy connection fully closed");
        free(ctx);
    }
}

/* ── Close both handles.  ctx is freed in on_proxy_close after both
 *    close callbacks have fired. ── */
static void proxy_close_all(ts_proxy_ctx_t *ctx)
{
    if (ctx->closing) return;
    ctx->closing = 1;

    if (ctx->client_reading) {
        uv_read_stop((uv_stream_t *)&ctx->client);
        ctx->client_reading = 0;
    }
    if (ctx->upstream_reading) {
        uv_read_stop((uv_stream_t *)&ctx->upstream);
        ctx->upstream_reading = 0;
    }

    if (!uv_is_closing((uv_handle_t *)&ctx->client)) {
        uv_close((uv_handle_t *)&ctx->client, on_proxy_close);
    } else {
        ctx->close_count++;  /* already closing, count it */
    }

    if (!uv_is_closing((uv_handle_t *)&ctx->upstream)) {
        uv_close((uv_handle_t *)&ctx->upstream, on_proxy_close);
    } else {
        ctx->close_count++;
    }

    /* If both were already closing/closed, free immediately */
    if (ctx->close_count >= 2) {
        LOG_INFO("Proxy connection fully closed (immediate)");
        free(ctx);
    }
}

static void on_upstream_connect(uv_connect_t *req, int status)
{
    ts_proxy_ctx_t *ctx = req->data;

    if (status < 0) {
        LOG_ERROR("Failed to connect to upstream: %s", uv_strerror(status));
        proxy_close_all(ctx);
        return;
    }

    ctx->upstream_connected = 1;
    LOG_INFO("Connected to upstream %s:%d", ctx->config->target_host,
             ctx->config->target_port);

    /* Start reading from both sides. */
    uv_read_start((uv_stream_t *)&ctx->client, alloc_cb, on_client_read);
    ctx->client_reading = 1;

    uv_read_start((uv_stream_t *)&ctx->upstream, alloc_cb, on_upstream_read);
    ctx->upstream_reading = 1;
}

static void on_resolved(uv_getaddrinfo_t *resolver, int status,
                         struct addrinfo *res)
{
    ts_proxy_ctx_t *ctx = resolver->data;
    free(resolver);

    if (status < 0) {
        LOG_ERROR("DNS resolution failed for %s: %s",
                  ctx->config->target_host, uv_strerror(status));
        if (res) uv_freeaddrinfo(res);
        proxy_close_all(ctx);
        return;
    }

    ctx->connect_req.data = ctx;
    int r = uv_tcp_connect(&ctx->connect_req, &ctx->upstream,
                           res->ai_addr, on_upstream_connect);
    uv_freeaddrinfo(res);

    if (r < 0) {
        LOG_ERROR("uv_tcp_connect failed: %s", uv_strerror(r));
        proxy_close_all(ctx);
    }
}

void ts_proxy_handle_connection(uv_stream_t *server, ts_config_t *cfg,
                                uv_loop_t *loop)
{
    ts_proxy_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        LOG_ERROR("Failed to allocate proxy context");
        return;
    }

    ctx->config = cfg;

    uv_tcp_init(loop, &ctx->client);
    uv_tcp_init(loop, &ctx->upstream);

    ctx->client.data   = ctx;
    ctx->upstream.data = ctx;

    int r = uv_accept(server, (uv_stream_t *)&ctx->client);
    if (r < 0) {
        LOG_ERROR("Failed to accept proxy client: %s", uv_strerror(r));
        proxy_close_all(ctx);
        return;
    }

    LOG_INFO("Proxy: new client connection, connecting to %s:%d",
             cfg->target_host, cfg->target_port);

    /* Resolve the target host (works for both IPs and hostnames). */
    uv_getaddrinfo_t *resolver = malloc(sizeof(*resolver));
    if (!resolver) {
        LOG_ERROR("Failed to allocate resolver");
        proxy_close_all(ctx);
        return;
    }
    resolver->data = ctx;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", cfg->target_port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    r = uv_getaddrinfo(loop, resolver, on_resolved,
                       cfg->target_host, port_str, &hints);
    if (r < 0) {
        LOG_ERROR("uv_getaddrinfo failed: %s", uv_strerror(r));
        free(resolver);
        proxy_close_all(ctx);
    }
}
